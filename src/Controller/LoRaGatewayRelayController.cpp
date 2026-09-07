#include "Controller/LoRaGatewayRelayController.h"
#include "Controller/ServiceController.h"
#include "Controller/StorageController.h"
#include "Logic/LoRaPrivateFrameLogic.h"
#include <RadioLib.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include "mbedtls/base64.h"

namespace
{
    // Roadmap #395 finding 3 - frame.payload is now AES-256-GCM ciphertext (this controller stays "deliberately dumb", it never decrypts), and JSON strings must be valid UTF-8 text - base64 is how it crosses the RelayUplink HTTP/JSON boundary intact.
    String base64Encode(const std::string &data)
    {
        size_t outLen = 0;
        mbedtls_base64_encode(nullptr, 0, &outLen, (const unsigned char *)data.data(), data.size());
        String out;
        out.reserve(outLen);
        std::string buffer(outLen, '\0');
        size_t written = 0;
        mbedtls_base64_encode((unsigned char *)&buffer[0], outLen, &written, (const unsigned char *)data.data(), data.size());
        out.concat(buffer.data(), written);
        return out;
    }

    // Needs real margin, same reasoning as LoRaGatewayBridgeController's identical constant - a
    // node's uplink arrives with no shared clock, a short window can start mid-preamble and miss it.
    const uint32_t RX_POLL_TIMEOUT_MS = 1000;

#if defined(AGRUMY_KIT_KC868_A6)
    // KC868-A6 (SX1278) pin mapping - sourced from KinCony's own ESPHome device page
    // (devices.esphome.io/devices/kincony-kc868-a6) and forum (kincony.com/forum, thread 2576),
    // NOT independently hardware-verified by this project (unlike the Heltec pins below).
    const int PIN_SCK = 18;
    const int PIN_MISO = 19;
    const int PIN_MOSI = 23;
    const int PIN_CS = 5;
    const int PIN_RST = 21;
    const int PIN_DIO0 = 2;
    // REV2+ boards only - REV1 needs a jumper wire from the SX1278's own IO1 to GPIO13 (kincony.com/forum, thread 2576).
    const int PIN_DIO1 = 13;
    SX1278 loRaGatewayRadio = new Module(PIN_CS, PIN_DIO0, PIN_RST, PIN_DIO1);
#else
    // Heltec WiFi LoRa 32 V3/V4 (SX1262) - confirmed on real V3 hardware (radio.begin() + real RF,
    // 2026-09-06, same pins as LoRaPrivateController.cpp/LoRaGatewayBridgeController.cpp). V4
    // pins assumed identical (Heltec markets V4 as pin-compatible with V3) - not independently verified.
    const int PIN_SCK = 9;
    const int PIN_MISO = 11;
    const int PIN_MOSI = 10;
    const int PIN_CS = 8;
    const int PIN_RST = 12;
    const int PIN_BUSY = 13;
    const int PIN_DIO1 = 14;
    // Gates the FET powering the SX1262's RF stage - radio.begin() (pure SPI register access) succeeds without it, but transmit/receive radiate nothing until this is driven LOW.
    const int PIN_VEXT = 36;
    SX1262 loRaGatewayRadio = new Module(PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY);
#endif

    // Disk backlog goes first, oldest file first, so the server receives uplinks in chronological order (roadmap #396(7), same pattern as SensorController::flushBufferedSensorData) - a broken-off flush means the connection is down again, so the caller's own live send this cycle is left to fail and buffer normally rather than retried here.
    void flushBufferedRelayUplinks(ServiceController &service, ServiceRequest serviceRequest)
    {
        String filename = StorageController::oldestBufferedRelayFile();
        while (!filename.isEmpty())
        {
            String payloadJson = StorageController::loadFile(filename);
            JsonDocument payload;
            if (payloadJson.isEmpty() || deserializeJson(payload, payloadJson) != DeserializationError::Ok)
            {
                Serial.println("[LoRaGatewayRelay] Buffered file /" + filename + " unreadable - dropping it");
                StorageController::removeBufferedFile(filename);
            }
            else
            {
                ServiceData result = service.requestPost(payload, serviceRequest);
                if (result.eventlog.error)
                {
                    Serial.println("[LoRaGatewayRelay] Flush stopped at /" + filename + " - connection lost again, remaining files stay queued");
                    return;
                }
                StorageController::removeBufferedFile(filename);
            }
            filename = StorageController::oldestBufferedRelayFile();
        }
    }
}

bool LoRaGatewayRelayController::begin()
{
#if !defined(AGRUMY_KIT_KC868_A6)
    pinMode(PIN_VEXT, OUTPUT);
    digitalWrite(PIN_VEXT, LOW);
    delay(50); // let the RF-stage power rail settle before touching the radio over SPI
#endif
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    int state = loRaGatewayRadio.begin(868.0, 125.0, 9, 7);
    radioReady = (state == RADIOLIB_ERR_NONE);
    if (radioReady)
    {
        Serial.println("[LoRaGatewayRelay] Radio ready");
    }
    else
    {
        Serial.printf("[LoRaGatewayRelay] Radio init failed, code %d - no LoRa chip physically present/wired?\n", state);
    }
    return radioReady;
}

void LoRaGatewayRelayController::poll(ServiceController &service, ServiceRequest serviceRequest, const DeviceConfig &deviceConfig)
{
    if (!radioReady)
    {
        return;
    }

    uint8_t buf[256];
    int state = loRaGatewayRadio.receive(buf, sizeof(buf), RX_POLL_TIMEOUT_MS);
    if (state != RADIOLIB_ERR_NONE)
    {
        return; // timeout (nothing in the air) or a receive error - either way, nothing to relay this poll window
    }

    size_t len = loRaGatewayRadio.getPacketLength();
    LoRaPrivateFrame frame;
    if (!decodeLoRaPrivateFrame(buf, len, frame))
    {
        Serial.printf("[LoRaGatewayRelay] Received %u bytes, too short to decode\n", (unsigned)len);
        return;
    }

    Serial.printf("[LoRaGatewayRelay] Uplink from node=%u len=%u, relaying via WiFi\n", frame.srcAddress, (unsigned)frame.payload.size());

    // Own permanent credential, not the shared session apiAuth - RelayUplink authenticates the same way Batch does (DeviceAuth.ApiKeyPolicy).
    serviceRequest.endpoint = "/api/Gateway/RelayUplink";
    serviceRequest.header.apiId = deviceConfig.apiId;
    serviceRequest.header.apiKey = deviceConfig.apiKey;

    JsonDocument body;
    body["SourceAddress"] = frame.srcAddress;
    body["Payload"] = base64Encode(frame.payload);

    flushBufferedRelayUplinks(service, serviceRequest);
    ServiceData result = service.requestPost(body, serviceRequest);
    if (result.eventlog.error)
    {
        String payloadJson;
        serializeJson(body, payloadJson);
        if (!StorageController::bufferRelayUplinkToDisk(payloadJson))
        {
            Serial.println("[LoRaGatewayRelay] Uplink buffer discarded - LittleFS full or write failed");
        }
    }
}
