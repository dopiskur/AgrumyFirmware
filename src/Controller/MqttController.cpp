#include "Arduino.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "mbedtls/md.h"

#include "MqttController.h"
#include "DeviceController.h"
#include "ServiceController.h"
#include "Logic/CommandReplayLogic.h"

// Root CA bundle embedded via platformio.ini board_build.embed_files - same bundle ServiceController uses for HTTPS.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_data_cert_x509_crt_bundle_bin_start");

// PubSubClient's compiled-in default (256 bytes) is too small for a full SensorData JSON payload.
static const uint16_t MQTT_BUFFER_SIZE = 1024;

MqttController mqtt;

namespace
{
    // Persistent-connection state for the command channel (roadmap #146) - kept at file scope, not
    // as MqttController members, so PubSubClient::setCallback's plain function pointer has something
    // fixed to close over via these globals instead of instance state.
    WiFiClientSecure persistentSecureClient;
    WiFiClient persistentPlainClient;
    PubSubClient persistentClient;
    bool persistentClientInitialized = false;

    // In-RAM only, resets on reboot - closes the realistic same-session replay window (a captured, still-validly-signed message republished later this same uptime), not a persisted, cross-reboot-safe ledger.
    int lastProcessedCommandId = 0;

    // 32 raw HMAC-SHA256 bytes -> 64 lowercase hex chars + NUL, same convention as OtaController's sha256ToHex.
    void hmacSha256Hex(const String &key, const String &message, char out[65])
    {
        unsigned char digest[32];
        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                         (const unsigned char *)key.c_str(), key.length(),
                         (const unsigned char *)message.c_str(), message.length(), digest);
        static const char *hexDigits = "0123456789abcdef";
        for (int i = 0; i < 32; i++)
        {
            out[i * 2] = hexDigits[(digest[i] >> 4) & 0x0F];
            out[i * 2 + 1] = hexDigits[digest[i] & 0x0F];
        }
        out[64] = '\0';
    }

    // Runs synchronously inside persistentClient.loop() (our own call, not an ISR) - safe to call
    // straight into processPendingCommand(), but note that command's own ack POST/OTA work blocks
    // this call until it returns, same as it already blocks the normal poll-driven path.
    void onCommandMessage(char *topic, byte *payload, unsigned int length)
    {
        JsonDocument doc;
        if (deserializeJson(doc, payload, length) != DeserializationError::Ok)
        {
            Serial.println("[Mqtt] Command message failed to parse - ignored");
            return;
        }

        int idDeviceCommand = doc["idDeviceCommand"] | 0;
        int actionType = doc["actionType"] | 0;
        String expiresAt = doc["expiresAt"] | String("");
        String cmdPayload = doc["payload"] | String("");
        String receivedSig = doc["sig"] | String("");

        // Must match MqttCommandPublisher.CanonicalString byte-for-byte - signed with THIS device's own apiKey, not the shared broker credential, so forging a command needs that specific device's key.
        String canonical = String(idDeviceCommand) + "|" + String(actionType) + "|" + expiresAt + "|" + cmdPayload;
        char expectedSigHex[65];
        hmacSha256Hex(deviceConfig.apiKey, canonical, expectedSigHex);
        if (receivedSig.isEmpty() || !receivedSig.equalsIgnoreCase(expectedSigHex))
        {
            Serial.println("[Mqtt] Command message signature missing or invalid - dropped");
            return;
        }

        // A valid signature alone doesn't stop a previously-captured message from being republished later - reject a stale idDeviceCommand or an already-expired expiresAt.
        if (commandIsReplayed(idDeviceCommand, lastProcessedCommandId, isoUtcToEpochSeconds(expiresAt.c_str()), (long)device.getEpochSeconds()))
        {
            Serial.println("[Mqtt] Command message rejected as a replay (stale idDeviceCommand or expired expiresAt)");
            return;
        }
        lastProcessedCommandId = idDeviceCommand;

        if (actionType == COMMAND_FORCE_CONFIG_SYNC)
        {
            // This action type's whole job is refreshing the cached deviceConfig used everywhere else in this switch - acting on it here would just see the same stale data (see ServiceController's own "config already current" case). Polling now instead gets real, current config, and the still-Pending command gets acked/executed normally as part of that same poll's response.
            Serial.println("[Mqtt] ForceConfigSync received via persistent channel, polling for fresh config now");
            service.apiConfig(deviceConfig, serviceRequest, device);
            return;
        }

        deviceConfig.pendingCommand.present = true;
        deviceConfig.pendingCommand.idDeviceCommand = idDeviceCommand;
        deviceConfig.pendingCommand.actionType = actionType;
        deviceConfig.pendingCommand.expiresAt = expiresAt;
        deviceConfig.pendingCommand.payload = cmdPayload;
        Serial.println("[Mqtt] Command received via persistent channel, dispatching immediately");
        service.processPendingCommand(deviceConfig, serviceRequest, device);
    }
}

void MqttController::begin(DeviceController& device)
{
    clientId = "Agrumy_" + device.macAddr();

    String configJson = device.loadFile("mqttConfig.json");
    if (configJson.isEmpty())
    {
        Serial.println("[Mqtt] No mqttConfig.json - publishing disabled");
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, configJson) != DeserializationError::Ok)
    {
        Serial.println("[Mqtt] mqttConfig.json failed to parse - publishing disabled");
        return;
    }

    brokerHost = doc["brokerHost"] | "";
    brokerPort = doc["brokerPort"] | 1883;
    username = doc["username"] | "";
    password = doc["password"] | "";
    persistentCommandChannel = doc["persistentCommandChannel"] | false;

    if (brokerHost.isEmpty())
    {
        Serial.println("[Mqtt] Broker host blank - publishing disabled");
    }
    else
    {
        Serial.println("[Mqtt] Configured: " + brokerHost + ":" + String(brokerPort));
    }
}

// Fresh TCP+TLS connection per call, no persistent session - matches this firmware's per-cycle connect model.
bool MqttController::publish(const String& topic, const String& payload)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[Mqtt] WiFi not connected - skipping publish");
        return false;
    }

    bool useTls = (brokerPort == 8883);
    WiFiClientSecure secureClient;
    WiFiClient plainClient;
    if (useTls)
    {
        secureClient.setCACertBundle(rootca_crt_bundle_start);
    }
    PubSubClient client(useTls ? static_cast<Client&>(secureClient) : static_cast<Client&>(plainClient));
    client.setBufferSize(MQTT_BUFFER_SIZE);
    client.setServer(brokerHost.c_str(), (uint16_t)brokerPort);

    bool connected = username.isEmpty()
        ? client.connect(clientId.c_str())
        : client.connect(clientId.c_str(), username.c_str(), password.c_str());

    if (!connected)
    {
        Serial.printf("[Mqtt] Connect to %s:%d failed, state=%d\n", brokerHost.c_str(), brokerPort, client.state());
        return false;
    }

    bool ok = client.publish(topic.c_str(), payload.c_str());
    Serial.println((ok ? "[Mqtt] Published to " : "[Mqtt] Publish failed to ") + topic);
    client.disconnect();
    return ok;
}

void MqttController::publishSensorData(DeviceConfig& config, JsonDocument& sensorJson)
{
    if (brokerHost.isEmpty())
    {
        return;
    }

    String topic = "agrumy/" + String(config.tenantID) + "/" + String(config.deviceID) + "/sensordata";
    String payload;
    serializeJson(sensorJson, payload);
    publish(topic, payload);
}

void MqttController::beginPersistentIfEnabled(DeviceConfig& config)
{
    if (!persistentCommandChannel || brokerHost.isEmpty() || WiFi.status() != WL_CONNECTED)
    {
        return;
    }
    if (persistentClient.connected())
    {
        return; // already up, nothing to do
    }

    if (!persistentClientInitialized)
    {
        bool useTls = (brokerPort == 8883);
        if (useTls)
        {
            persistentSecureClient.setCACertBundle(rootca_crt_bundle_start);
            persistentClient.setClient(persistentSecureClient);
        }
        else
        {
            persistentClient.setClient(persistentPlainClient);
        }
        persistentClient.setBufferSize(MQTT_BUFFER_SIZE);
        // PubSubClient's 15s default keepalive is shorter than the ~30s gap between poll() calls in main.cpp's chunked idle wait (WDT_TIMEOUT_SECONDS/3) - the broker was killing the connection for inactivity before poll() ever ran again, silently dropping every pushed command (confirmed against a real broker+device). 120s comfortably outlasts a full sleepSeconds cycle.
        persistentClient.setKeepAlive(120);
        persistentClient.setServer(brokerHost.c_str(), (uint16_t)brokerPort);
        persistentClient.setCallback(onCommandMessage);
        persistentClientInitialized = true;
    }

    bool connected = username.isEmpty()
        ? persistentClient.connect(clientId.c_str())
        : persistentClient.connect(clientId.c_str(), username.c_str(), password.c_str());
    if (!connected)
    {
        Serial.printf("[Mqtt] Persistent connect failed, state=%d\n", persistentClient.state());
        return;
    }

    String topic = "agrumy/" + String(config.tenantID) + "/" + String(config.deviceID) + "/command";
    persistentClient.subscribe(topic.c_str());
    Serial.println("[Mqtt] Persistent command channel connected, subscribed to " + topic);
}

void MqttController::poll()
{
    if (persistentCommandChannel && persistentClient.connected())
    {
        persistentClient.loop();
    }
}
