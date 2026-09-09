#include "Arduino.h"
#include "WiFi.h"
#include "HTTPClient.h"
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <WiFiClientSecure.h>
#include "NTPClient.h"
#include "ServiceController.h"
#include "AgrumyClient.h"
#include "DeviceController.h"
#include "SensorController.h"
#include "StorageController.h"
#include "ConfigParser.h"
#include "OtaController.h"
#include "../Logic/DiscoveryLogic.h"
#include "../Logic/HttpDateLogic.h"
#include "../Logic/ConfigApplyLogic.h"
#include "../Logic/NetworkRequestLogic.h"
#include "MqttController.h"

#include <ArduinoJson.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

extern const char *firmware; // main.cpp - the RUNNING image's version

// Deliberately matches no catalog board, so a build that bypassed platformio.ini's define is never OTA'd.
#ifndef AGRUMY_BOARD
#define AGRUMY_BOARD "unknown"
#endif

// Which commercial KIT (physical PCB) this image was built for, separate from AGRUMY_BOARD: Board picks the OTA binary, Kit tells the server which relay-hardware capability to look up. Empty on generic chip-target environments.
#ifndef AGRUMY_KIT
#define AGRUMY_KIT ""
#endif

// Roadmap #457 (EPIC A3) - the transport/session logic that used to live here (network task,
// requestPost/requestGet/requestPostSync/requestGetSync, firmwareUpdate, mqttPublish/
// mqttConnectPersistent, maskSecret, apiAuthenticate, isHardResetPending) moved to AgrumyClient (see
// AgrumyClient.h/.cpp). These forwards exist so every existing external caller of `service.xxx(...)`
// (DeviceController, MqttController, ConfigParser, main.cpp) keeps working unchanged.
void ServiceController::beginNetworkTask() { AgrumyClient::beginNetworkTask(); }
TaskHandle_t ServiceController::networkTaskHandle() { return AgrumyClient::networkTaskHandle(); }
String ServiceController::maskSecret(const String &value) { return AgrumyClient::maskSecret(value); }
ServiceData ServiceController::requestPost(const JsonDocument& jsonBuffer, ServiceRequest service) { return agrumyClient.requestPost(jsonBuffer, service); }
ServiceData ServiceController::requestGet(ServiceRequest service) { return agrumyClient.requestGet(service); }
bool ServiceController::firmwareUpdate(const OtaParams& params) { return agrumyClient.firmwareUpdate(params); }
bool ServiceController::mqttPublish(const String& topic, const String& payload) { return agrumyClient.mqttPublish(topic, payload); }
bool ServiceController::mqttConnectPersistent(int tenantID, int deviceID) { return agrumyClient.mqttConnectPersistent(tenantID, deviceID); }
void ServiceController::apiAuthenticate(const DeviceConfig& deviceConfig, ServiceRequest serviceRequest, DeviceController& device) { agrumyClient.apiAuthenticate(deviceConfig, serviceRequest, device); }

// Fire-and-forget: never checks the result or retries, so a failed push doesn't chase itself with another event about its own failure.
void ServiceController::pushEvent(ServiceRequest service, String eventType, String message, int commandId)
{
    service.endpoint = serviceEndpoint.apiEvent;
    service.header.apiKey = ""; // session-auth (apiAuth), same as apiConfig() - not apiKey-auth like Authenticate

    JsonDocument payload;
    payload["EventType"] = eventType;
    payload["Message"] = message;
    if (commandId >= 0)
    {
        payload["CommandId"] = commandId; // links this event back to the specific command row
    }

    Serial.println("[Service] pushEvent: " + eventType + (message.length() > 0 ? " (" + message + ")" : ""));
    requestPost(payload, service);
}

// Fire-and-forget: same convention as pushEvent - a dropped push just means the Fleet page's relay/position display lags until the next tick that also has a change, never buffered/retried like SensorData.
void ServiceController::pushControllerData(ServiceRequest service, const ControllerDataChange changes[], int count, const String &dateCreated)
{
    if (count == 0)
    {
        return;
    }
    service.endpoint = serviceEndpoint.apiControllerDataPost;
    service.header.apiKey = ""; // session-auth (apiAuth), same as apiConfig()/pushEvent()

    JsonDocument payload;
    JsonArray array = payload.to<JsonArray>();
    for (int i = 0; i < count; i++)
    {
        JsonObject entry = array.add<JsonObject>();
        entry["relayFunction"] = changes[i].relayFunction;
        entry["isOn"] = changes[i].isOn;
        if (changes[i].isPositional)
        {
            entry["percent"] = changes[i].percent;
        }
        entry["dateCreated"] = dateCreated;
    }

    Serial.println("[Service] pushControllerData: " + String(count) + " change(s)");
    requestPost(payload, service);
}

// Ack happens BEFORE execute: a Reboot has no "after" on this same connection to report from.
void ServiceController::processPendingCommand(DeviceConfig& config, ServiceRequest serviceRequest, DeviceController& device)
{
    // Paired with the "nothing queued" log in apiConfig() below - if THIS fires instead, the config arrived but had no pendingCommand.
    if (!config.pendingCommand.present)
    {
        Serial.println("[Service] processPendingCommand: reached with none present");
        return;
    }

    int commandId = config.pendingCommand.idDeviceCommand;
    int actionType = config.pendingCommand.actionType;

    ServiceRequest ackRequest = serviceRequest;
    ackRequest.endpoint = serviceEndpoint.apiCommandAck;
    ackRequest.header.apiKey = ""; // session-auth, same as apiConfig()/pushEvent()

    JsonDocument ackPayload;
    ackPayload["CommandId"] = commandId;
    Serial.println("[Service] Acking pending command " + String(commandId) + " (actionType=" + String(actionType) + ")");
    requestPost(ackPayload, ackRequest);

    switch (actionType)
    {
    case COMMAND_REBOOT:
        Serial.println("[Service] Executing command " + String(commandId) + ": Reboot");
        device.reboot(); // never returns
        break;

    case COMMAND_FORCE_OTA:
    {
        // "Force" = skip apiConfig()'s normal fwVersion != running-image gate; same device.firmwareUpdate() the regular OTA check uses.
        String fwUrl = config.firmwareUrl;
        String fwSha256 = config.firmwareSha256;
        bool otaHttps = fwUrl.startsWith("https://") || fwUrl.startsWith("HTTPS://");

        if (fwUrl.length() == 0)
        {
            Serial.println("[Service] Command " + String(commandId) + " (ForceOTA): no firmware build available to force");
            pushEvent(serviceRequest, "CommandExecuted", "no firmware build available to force", commandId);
            break;
        }

        if (device.firmwareUpdate(fwUrl, otaHttps, fwSha256))
        {
            Serial.println("[Service] Command " + String(commandId) + " (ForceOTA) succeeded, rebooting into new image");
            pushEvent(serviceRequest, "CommandExecuted", "version=" + config.firmwareVersion, commandId);
            device.reboot(); // never returns
        }

        Serial.println("[Service] Command " + String(commandId) + " (ForceOTA) failed - staying on current firmware");
        pushEvent(serviceRequest, "CommandExecuted", "download/flash failed, version=" + config.firmwareVersion, commandId);
        break;
    }

    case COMMAND_FORCE_CONFIG_SYNC:
        // The config the server just sent in THIS SAME poll response already IS the resync - nothing left to do differently.
        Serial.println("[Service] Command " + String(commandId) + " (ForceConfigSync): config already current from this same poll, nothing further to do");
        pushEvent(serviceRequest, "CommandExecuted", "config already current from this poll", commandId);
        break;

    case COMMAND_SCAN_FOR_DEVICES:
        Serial.println("[Service] Command " + String(commandId) + " (ScanForDevices): scanning for nearby Agrumy_ access points");
        scanAndReportDevices(serviceRequest);
        pushEvent(serviceRequest, "CommandExecuted", "scan complete", commandId);
        break;

    case COMMAND_PROVISION_DEVICE:
    {
        Serial.println("[Service] Command " + String(commandId) + " (ProvisionDevice): connecting to target AP");
        bool provisioned = provisionDiscoveredDevice(config.pendingCommand.payload);
        pushEvent(serviceRequest, "CommandExecuted", provisioned ? "provisioning POST accepted" : "provisioning failed", commandId);
        break;
    }

    case COMMAND_UPDATE_WIFI:
    {
        Serial.println("[Service] Command " + String(commandId) + " (UpdateWifiCredentials): trying new network");
        bool switched = switchWifiNetwork(config.pendingCommand.payload, serviceRequest);
        pushEvent(serviceRequest, "CommandExecuted", switched ? "wifi switch verified and applied" : "wifi switch failed verification, reverted to previous network", commandId);
        break;
    }

    case COMMAND_DETECT_SENSORS:
    {
        Serial.println("[Service] Command " + String(commandId) + " (DetectSensors): scanning I2C bus");
        String scanResult = sensor.detectSensors();
        pushEvent(serviceRequest, "CommandExecuted", scanResult, commandId);
        break;
    }

    default:
        Serial.println("[Service] Command " + String(commandId) + ": unknown actionType " + String(actionType) + ", ignoring");
        break;
    }
}

void ServiceController::scanAndReportDevices(ServiceRequest serviceRequest)
{
    int found = WiFi.scanNetworks();
    Serial.println("[Service] Scan complete: " + String(found) + " networks seen");

    ServiceRequest reportRequest = serviceRequest;
    reportRequest.endpoint = serviceEndpoint.apiDiscoveryReport;
    reportRequest.header.apiKey = ""; // session-auth, same as apiConfig()/pushEvent()

    for (int i = 0; i < found; i++)
    {
        std::string mac = extractAgrumyApMac(std::string(WiFi.SSID(i).c_str()));
        if (mac.empty())
        {
            continue;
        }
        Serial.println("[Service] Found Agrumy AP " + String(mac.c_str()) + " (rssi=" + String(WiFi.RSSI(i)) + ")");

        JsonDocument payload;
        payload["DiscoveredApMac"] = mac;
        payload["Rssi"] = WiFi.RSSI(i);
        requestPost(payload, reportRequest);
    }

    WiFi.scanDelete();
}

// A blank targetSsid/password argument to WiFi.begin() is a valid "open network" request on ESP32, so ownPsk staying empty for an open home network is not a special case here.
static bool waitForWifiConnect(unsigned long timeoutMs)
{
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs)
    {
        delay(250);
    }
    return WiFi.status() == WL_CONNECTED;
}

bool ServiceController::provisionDiscoveredDevice(const String& payloadJson)
{
    JsonDocument payload;
    if (deserializeJson(payload, payloadJson) != DeserializationError::Ok)
    {
        Serial.println("[Service] ProvisionDevice: payload failed to parse");
        return false;
    }

    String discoveredApMac = payload["DiscoveredApMac"] | String("");
    if (discoveredApMac.isEmpty())
    {
        Serial.println("[Service] ProvisionDevice: payload missing DiscoveredApMac");
        return false;
    }
    String username = payload["Username"] | String("");
    String pin = payload["Pin"] | String("");
    String ssid = payload["Ssid"] | String("");
    String wifiPassword = payload["WifiPassword"] | String("");
    // Server's own current host, not this scanning device's own (possibly stale) deviceConfig.servicePoint - falls back to it only for a command queued before the server started sending ServicePoint.
    String provisionedServicePoint = payload["ServicePoint"] | deviceConfig.servicePoint;

    // From NVS, not WiFi.SSID()/psk() (roadmap #396(8)) - those only report the currently connected network, blank if this device happens to not be connected right now.
    String ownSsid, ownPsk;
    StorageController::loadWifiCredentialsBackup(ownSsid, ownPsk);

    WiFi.disconnect();
    String targetSsid = "Agrumy_" + discoveredApMac;
    Serial.println("[Service] ProvisionDevice: connecting to " + targetSsid);
    WiFi.begin(targetSsid.c_str());

    const unsigned long connectTimeoutMs = 15000;
    bool success = false;
    if (waitForWifiConnect(connectTimeoutMs))
    {
        String body =
            "s=" + String(urlEncodeFormValue(ssid.c_str()).c_str()) +
            "&p=" + String(urlEncodeFormValue(wifiPassword.c_str()).c_str()) +
            "&login=" + String(urlEncodeFormValue(username.c_str()).c_str()) +
            "&devicePin=" + String(urlEncodeFormValue(pin.c_str()).c_str()) +
            "&servicePoint=" + String(urlEncodeFormValue(provisionedServicePoint.c_str()).c_str()) +
            "&mqttHost=&mqttPort=&mqttUser=&mqttPass=";

        HTTPClient http;
        http.begin("http://192.168.4.1/wifisave");
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        int httpCode = http.POST(body);
        Serial.println("[Service] ProvisionDevice: /wifisave POST returned " + String(httpCode));
        success = (httpCode == 200);
        http.end();
    }
    else
    {
        Serial.println("[Service] ProvisionDevice: could not connect to " + targetSsid);
    }

    WiFi.disconnect();
    WiFi.begin(ownSsid.c_str(), ownPsk.c_str());
    if (!waitForWifiConnect(connectTimeoutMs))
    {
        Serial.println("[Service] ProvisionDevice: failed to reconnect to " + ownSsid + " after provisioning attempt");
    }

    return success;
}

// freeHeap alone doesn't show fragmentation (MaxAllocHeap) or a transient dip since boot (MinFreeHeap), and neither shows how close the loop task itself is to a stack overflow (StackHighWaterMark, bytes never touched - low means close). Shared by both heartbeat call sites below.
static void addHeapDiagnostics(JsonDocument &payload)
{
    payload["FreeHeap"] = ESP.getFreeHeap();
    payload["MinFreeHeap"] = ESP.getMinFreeHeap();
    payload["MaxAllocHeap"] = ESP.getMaxAllocHeap();
    payload["StackHighWaterMark"] = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    // Same margin for the network task (NETWORK_TASK_STACK_SIZE) - it, not loopTask, runs every TLS handshake and OTA download.
    payload["NetworkStackHighWaterMark"] = (uint32_t)uxTaskGetStackHighWaterMark(AgrumyClient::networkTaskHandle());
}

// A real config-poll, not just WiFi.status()==WL_CONNECTED - a wrong/isolated network can still hand out a link with no route to the server. Mirrors apiConfig()'s own single auth-retry, but never touches its reboot/config-apply side effects.
static bool verifyServerReachable(ServiceController& serviceController, ServiceRequest serviceRequest)
{
    serviceRequest.endpoint = serviceEndpoint.apiConfig;
    serviceRequest.header.apiId = deviceConfig.apiId;
    serviceRequest.header.apiKey = "";

    JsonDocument payload;
    payload["ConfigVersion"] = String(deviceConfig.configVersion);
    payload["Uptime"] = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    payload["Rssi"] = WiFi.RSSI();
    addHeapDiagnostics(payload);
    payload["FirmwareVersion"] = firmware;
    payload["Board"] = AGRUMY_BOARD;
    payload["Kit"] = AGRUMY_KIT;

    ServiceData serviceData = serviceController.requestPost(payload, serviceRequest);
    return !serviceData.eventlog.error;
}

bool ServiceController::switchWifiNetwork(const String& payloadJson, ServiceRequest serviceRequest)
{
    JsonDocument payload;
    if (deserializeJson(payload, payloadJson) != DeserializationError::Ok)
    {
        Serial.println("[Service] UpdateWifiCredentials: payload failed to parse");
        return false;
    }
    String newSsid = payload["Ssid"] | String("");
    String newPassword = payload["WifiPassword"] | String("");
    if (newSsid.isEmpty())
    {
        Serial.println("[Service] UpdateWifiCredentials: payload missing Ssid");
        return false;
    }

    // From NVS, not WiFi.SSID()/psk() (roadmap #396(8)) - those only report the currently connected network, blank if this device happens to not be connected right now.
    String oldSsid, oldPsk;
    StorageController::loadWifiCredentialsBackup(oldSsid, oldPsk);

    const unsigned long connectTimeoutMs = 15000;
    bool verified = false;

    // persistent(false): WiFi.begin() below writes nothing to flash while the new network is only a trial - the old credentials stay the ones actually saved until this succeeds.
    WiFi.persistent(false);
    WiFi.disconnect();
    Serial.println("[Service] UpdateWifiCredentials: trying " + newSsid);
    WiFi.begin(newSsid.c_str(), newPassword.c_str());

    if (waitForWifiConnect(connectTimeoutMs))
    {
        verified = verifyServerReachable(*this, serviceRequest);
        if (!verified)
        {
            Serial.println("[Service] UpdateWifiCredentials: connected to " + newSsid + " but server was not reachable on it");
        }
    }
    else
    {
        Serial.println("[Service] UpdateWifiCredentials: could not connect to " + newSsid);
    }

    WiFi.persistent(true);
    if (verified)
    {
        // Same call again now that persistence is back on, so the now-proven network is the one actually written to flash.
        WiFi.begin(newSsid.c_str(), newPassword.c_str());
        waitForWifiConnect(connectTimeoutMs);
        StorageController::saveWifiCredentialsBackup(newSsid, newPassword);
        return true;
    }

    WiFi.disconnect();
    WiFi.begin(oldSsid.c_str(), oldPsk.c_str());
    if (!waitForWifiConnect(connectTimeoutMs))
    {
        Serial.println("[Service] UpdateWifiCredentials: failed to reconnect to " + oldSsid + " after a failed switch");
    }
    return false;
}

bool ServiceController::apiConfig(DeviceConfig& deviceConfig, ServiceRequest serviceRequest, DeviceController& device)
{
    waitSeconds = 0; // only set below on a 429 ("Wait") response - stale from a previous cycle otherwise
    String configVersion=String(deviceConfig.configVersion);

    Serial.print("[Service] Current configVersion: ");
    Serial.println(configVersion);

    serviceRequest.endpoint = serviceEndpoint.apiConfig;
    serviceRequest.header.apiId = deviceConfig.apiId;
    serviceRequest.header.apiKey = "";

    ServiceData serviceData;
    JsonDocument payload;
    payload["ConfigVersion"] = configVersion;
    // The config poll doubles as the heartbeat. esp_timer, not millis(): 64-bit, no 49-day wrap.
    payload["Uptime"] = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    payload["Rssi"] = WiFi.RSSI();
    addHeapDiagnostics(payload);
    payload["FirmwareVersion"] = firmware;
    payload["Board"] = AGRUMY_BOARD; // PlatformIO env name from the build flag, never guessed from the chip at runtime
    payload["Kit"] = AGRUMY_KIT;
    payload["ConfigSchemaVersion"] = CONFIG_SCHEMA_VERSION;

    serviceData = requestPost(payload, serviceRequest);

    if(serviceData.eventlog.errorCode==401){

        Serial.println("[Service] apiConfig: failed to authenticate: ");
        apiAuthenticate(deviceConfig,serviceRequest, device);
        serviceData = requestPost(payload, serviceRequest);
    }

    // Every cycle carries the server's clock via the Date header, config body or not - a device that only ever
    // gets heartbeat 200s (nothing changed) must not wait up to 24h for a full config poll to seed its fallback.
    if (serviceData.dateHeaderEpoch > 0)
    {
        device.applyServerEpochFallback((time_t)serviceData.dateHeaderEpoch);
    }

    // A relay under load (or the server's own rate limiter) asking us to back off, not a real failure - honor it and skip straight to the next normal cycle instead of feeding the reboot-escalation counter below.
    if (serviceData.eventlog.errorCode == 429)
    {
        waitSeconds = serviceData.retryAfterSeconds > 0 ? serviceData.retryAfterSeconds : 30;
        waitSeconds = constrain(waitSeconds, 10, 300); // same bound as ServerConfig's RelayWaitWindowSeconds
        Serial.printf("[Service] apiConfig: server asked us to wait %d s before the next cycle\n", waitSeconds);
        return false;
    }

    if (serviceData.eventlog.error)
    {
        Serial.print("[Service] Error accessing service point: ");
        Serial.println(serviceData.eventlog.errorCode);
    }
    else
    {
        lastConfigSyncEpoch = device.getEpochSeconds();
    }

    // A prolonged server/network outage must not, by itself, reboot the device (same "keep running on local rules" philosophy as the auth-failure fix above) - a raw failed-HTTP-cycle count says nothing about the device's own health. Reboot only on real memory pressure, which a reboot actually fixes.
    const uint32_t LOW_HEAP_REBOOT_THRESHOLD_BYTES = 20000;
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < LOW_HEAP_REBOOT_THRESHOLD_BYTES)
    {
        Serial.printf("[Service] Free heap critically low (%u bytes), rebooting.\n", freeHeap);
        pushEvent(serviceRequest, "LowMemoryReboot", "freeHeap=" + String(freeHeap));
        device.reboot();
    }

    // Derive firmware state from the config about to run - the new one if received, else the boot config (admin may set the flag without bumping configVersion -> 200, no body).
    bool   fwFlag    = deviceConfig.firmwareUpdate;
    String fwVersion = deviceConfig.firmwareVersion;
    String fwUrl     = deviceConfig.firmwareUrl;
    String fwSha256  = deviceConfig.firmwareSha256;

    bool receivedNewConfig = !serviceData.payload.isEmpty();
    // Heap-allocated once for the process lifetime, never on the stack - DeviceConfig is tens of KB (32 Rules x 8 ConditionNodes plus String fields), and a per-cycle stack copy of that is what actually overflowed loopTask, not the TLS handshake.
    static DeviceConfig *configCandidate = new DeviceConfig();

    // An empty body means the server decided nothing changed AND nothing is queued for this device.
    if (!receivedNewConfig) {
        Serial.println("[Service] apiConfig: no new config this cycle (up to date, nothing queued)");
    }

    if (receivedNewConfig) {
        // This was previously logged completely unmasked - apiKey and any password/secret field (e.g. a pendingCommand.payload's WifiPassword) both leaked in full.
        Serial.println(ConfigParser::maskApiKeyInJson(ConfigParser::redactSensitiveFieldsInJson(serviceData.payload)));
        // Parse-gate BEFORE persisting - a truncated body must neither clobber config.json nor be applied.
        JsonDocument parseCheck;
        if (deserializeJson(parseCheck, serviceData.payload) != DeserializationError::Ok) {
            Serial.println("[Service] New config payload failed to parse - ignoring it this cycle");
            receivedNewConfig = false;
        } else {
            // Seed the candidate with the CURRENT live config so ConfigParser::parse's "|" fallbacks resolve to today's values, then parse into the candidate only - the live deviceConfig stays untouched until the commit below, so a rejected config can never leave it half-overwritten.
            *configCandidate = deviceConfig;
            // loadConfig() gates on required identity keys (apiId/apiKey/servicePoint) and does no disk I/O, so it runs before saveConfigFile.
            bool configOk = device.loadConfig(serviceData.payload, *configCandidate);
            if (!configOk) {
                Serial.println("[Service] New config rejected (code " + String(configCandidate->eventlog.errorCode) + "): " + configCandidate->eventlog.errorData);
                pushEvent(serviceRequest, "ConfigSyncFailed", "code=" + String(configCandidate->eventlog.errorCode) + " " + configCandidate->eventlog.errorData);
                receivedNewConfig = false;
            } else {
                // The same admin-set flag isHardResetPending() checks on a 401 also rides along here on an ordinary, successfully-authenticated poll - a healthy device doesn't need the narrow apiId-only path, it just sees this in its next config.
                if (configCandidate->reset) {
                    Serial.println("[Service] Hard reset requested by admin - reseting device to defaults...");
                    device.reset(); // never returns
                }

                Serial.println("[Service] New config received, saving new config");
                device.saveConfigFile(serviceData.payload); // backs up the old config.json before overwriting it
                device.waitForFileCommitted("config.json"); // verified, not a bare delay()

                fwFlag    = configCandidate->firmwareUpdate;
                fwVersion = configCandidate->firmwareVersion;
                fwUrl     = configCandidate->firmwareUrl;
                fwSha256  = configCandidate->firmwareSha256;

                // Ahead of the regular OTA gate below on purpose: a pending Reboot must fire before anything else this cycle, and a pending ForceOTA gets its own shot even if the version-mismatch gate would otherwise skip it.
                processPendingCommand(*configCandidate, serviceRequest, device);
            }
        }
    }

    // OTA only when the server asks AND the offered version differs from this image, so a stale flag can't loop forever; do it before the reboot below.
    if (fwFlag && fwUrl.length() > 0 && fwVersion != String(firmware)) {
        bool otaHttps = fwUrl.startsWith("https://") || fwUrl.startsWith("HTTPS://");
        Serial.println("[Service] Firmware update " + fwVersion + " available (running " + String(firmware) + ")");
        if (device.firmwareUpdate(fwUrl, otaHttps, fwSha256)) {
            Serial.println("[Service] OTA succeeded, rebooting into new image");
            device.reboot();
        }
        // failed download: fall through, keep running current firmware, retry next cycle
        Serial.println("[Service] OTA failed - staying on current firmware, will retry next config cycle");
        pushEvent(serviceRequest, "OtaFailed", "version=" + fwVersion);
    }

    if (receivedNewConfig) {
        // deviceConfig is still the OLD (pre-update) config here - the commit below hasn't run yet - so this is a real diff, not a compare-against-itself.
        // Reboot only when a field tied to boot-time state changed (transport/TLS setup, identity, sleep mode); everything else is read from deviceConfig every cycle and applies without a reboot.
        bool rebootRequired =
            configCandidate->deviceTypeServiceID != deviceConfig.deviceTypeServiceID ||
            configCandidate->servicePoint        != deviceConfig.servicePoint ||
            configCandidate->servicePublicKey    != deviceConfig.servicePublicKey ||
            configCandidate->apiId               != deviceConfig.apiId ||
            configCandidate->apiKey              != deviceConfig.apiKey ||
            configCandidate->sleepDeep           != deviceConfig.sleepDeep;

        if (rebootRequired) {
            // Feed the crash-loop-guard counter ONLY here, never on the OTA or too-many-failures reboots, so an unrelated reboot cause never falsely triggers a rollback in setup().
            device.notePendingConfigReboot(millis());
            device.reboot(); // boot into the newly saved config
        }

        // Tight scope on purpose - the relay task (ActuatorController::beginRelayTask) takes the same lock for its whole tick, so this commit must never be held any longer than the assignment itself.
        {
            ActuatorStateLock lock;
            deviceConfig = *configCandidate;
        }
        applyEpochFallbackIfCommitted(true, deviceConfig.serverUtcEpoch,
            [&device](long epoch) { device.applyServerEpochFallback((time_t)epoch); });
        Serial.println("[Service] Config hot-applied without reboot (version " + String(deviceConfig.configVersion) + ")");
        pushEvent(serviceRequest, "ConfigApplied", "version=" + String(deviceConfig.configVersion));
        // Surfaced so an admin actually finds out a rule silently isn't doing what they configured, instead of a quietly-truncated AND/OR chain misbehaving forever.
        if (deviceConfig.rulesRejectedCount > 0)
        {
            pushEvent(serviceRequest, "RuleRejected", String(deviceConfig.rulesRejectedCount) + " rule(s) rejected - unrecognized or over-cap condition, not evaluated");
        }
        return true;
    }

    Serial.println("[Service] Config didn't change, do nothing");
    return false;
}
