#include "Controller/LoRaGatewayBridgeController.h"
#include "Logic/LoRaPrivateFrameLogic.h"
#include "Logic/AgrumySerialFrameLogic.h"
#include <RadioLib.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <SPI.h>

namespace
{
    // Leading "/" required - LittleFS.exists()/open() reject a bare filename (confirmed on real ESP32-S3 hardware).
    const char *CONFIG_FILE = "/loraGatewayBridgeConfig.json";

    // Same Heltec WiFi LoRa 32 V3 pin mapping as Controller/LoRaPrivateController.cpp - confirmed correct on real hardware (radio.begin() succeeds, 2026-09-06).
    const int PIN_SCK = 9;
    const int PIN_MISO = 11;
    const int PIN_MOSI = 10;
    const int PIN_CS = 8;
    const int PIN_RST = 12;
    const int PIN_BUSY = 13;
    const int PIN_DIO1 = 14;
    // GPIO36 gates the FET powering the SX1262's RF stage - radio.begin() (pure SPI register access) succeeds without it, but transmit/receive radiate nothing until this is driven LOW.
    const int PIN_VEXT = 36;

    // Needs real margin: a node's uplink arrives with no shared clock, so a short window can start mid-preamble and miss the packet - confirmed on real hardware that anything much below 1s is unreliable at SF9.
    const uint32_t RX_POLL_TIMEOUT_MS = 1000;
}

SX1262 loRaBridgeRadio = new Module(PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY);

bool LoRaGatewayBridgeController::begin()
{
    if (!LittleFS.exists(CONFIG_FILE))
    {
        Serial.println("[LoRaBridge] No loraGatewayBridgeConfig.json - needs pre-provisioning (gatewayAddress).");
        return false;
    }
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f)
    {
        return false;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err)
    {
        Serial.println("[LoRaBridge] loraGatewayBridgeConfig.json is malformed.");
        return false;
    }

    gatewayAddress = doc["gatewayAddress"] | 0;
    frequencyMHz = doc["frequencyMHz"] | 868.0;
    spreadingFactor = doc["spreadingFactor"] | 9;
    bandwidthKHz = doc["bandwidthKHz"] | 125.0;
    codingRate = doc["codingRate"] | 7;
    txPowerDbm = doc["txPowerDbm"] | 22;

    pinMode(PIN_VEXT, OUTPUT);
    digitalWrite(PIN_VEXT, LOW);
    delay(50); // let the RF-stage power rail settle before touching the radio over SPI

    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    // Default tcxoVoltage left as-is - see LoRaPrivateController.cpp's identical begin() call for why.
    int state = loRaBridgeRadio.begin(frequencyMHz, bandwidthKHz, spreadingFactor, codingRate,
                                       RADIOLIB_SX126X_SYNC_WORD_PRIVATE, txPowerDbm);
    if (state != RADIOLIB_ERR_NONE)
    {
        Serial.printf("[LoRaBridge] Radio init failed, code %d\n", state);
        return false;
    }
    configLoaded = true;
    Serial.printf("[LoRaBridge] Ready: gateway=%u freq=%.1fMHz SF%u\n", gatewayAddress, frequencyMHz, spreadingFactor);
    return true;
}

void LoRaGatewayBridgeController::runOnce()
{
    if (!configLoaded)
    {
        return;
    }
    pollRadioForUplink();
    pollSerialForDownlink();
}

void LoRaGatewayBridgeController::pollRadioForUplink()
{
    uint8_t buf[256];
    int state = loRaBridgeRadio.receive(buf, sizeof(buf), RX_POLL_TIMEOUT_MS);
    if (state == RADIOLIB_ERR_RX_TIMEOUT)
    {
        // Ambient-noise heartbeat, throttled to ~1/2s - confirms the receiver is actually live even when nothing arrives.
        static unsigned long lastHeartbeatMs = 0;
        if (millis() - lastHeartbeatMs > 2000)
        {
            lastHeartbeatMs = millis();
            Serial.printf("[LoRaBridge] Listening, ambient RSSI=%.1f dBm\n", loRaBridgeRadio.getRSSI());
        }
        return; // nothing in the air this poll window - the common case, stay quiet
    }
    if (state != RADIOLIB_ERR_NONE)
    {
        Serial.printf("[LoRaBridge] Receive error, code %d\n", state);
        return;
    }

    size_t len = loRaBridgeRadio.getPacketLength();
    int8_t rssi = (int8_t)loRaBridgeRadio.getRSSI();
    LoRaPrivateFrame frame;
    if (!decodeLoRaPrivateFrame(buf, len, frame))
    {
        Serial.printf("[LoRaBridge] Received %u bytes, too short to decode\n", (unsigned)len);
        return;
    }
    if (frame.destAddress != gatewayAddress)
    {
        Serial.printf("[LoRaBridge] Received frame for address %u, not ours (%u) - ignored\n", frame.destAddress, gatewayAddress);
        return;
    }

    Serial.printf("[LoRaBridge] Uplink from node=%u rssi=%d len=%u: %s\n", frame.srcAddress, rssi, (unsigned)frame.payload.size(), frame.payload.c_str());

    // Echoes the plaintext prefix straight back over LoRa as the node's RX1-window ack, before the non-time-critical serial forward below - this bridge never decrypts, so it never sees anything past this prefix either way. The prefix is always 13 bytes: [0x02][bootNonce:8][counter:4].
    constexpr size_t AckPrefixLen = 13;
    if (frame.payload.size() >= AckPrefixLen)
    {
        std::string ackFrame = encodeLoRaPrivateFrame(frame.srcAddress, gatewayAddress, frame.payload.substr(0, AckPrefixLen));
        loRaBridgeRadio.transmit((const uint8_t *)ackFrame.data(), ackFrame.size());
    }

    std::string serialFrame = encodeAgrumySerialUplink(frame.srcAddress, rssi, frame.payload);
    Serial.write((const uint8_t *)serialFrame.data(), serialFrame.size());
}

void LoRaGatewayBridgeController::pollSerialForDownlink()
{
    while (Serial.available() > 0 && serialBufferLength < SERIAL_BUFFER_CAPACITY)
    {
        serialBuffer[serialBufferLength++] = (uint8_t)Serial.read();
    }

    size_t offset = 0;
    while (offset < serialBufferLength)
    {
        AgrumySerialDownlink downlink;
        bool hasFrame = false;
        size_t consumed = tryDecodeAgrumySerialDownlink(serialBuffer + offset, serialBufferLength - offset, downlink, hasFrame);
        if (consumed == 0)
        {
            break; // wait for more bytes
        }
        offset += consumed;
        if (hasFrame)
        {
            std::string rfFrame = encodeLoRaPrivateFrame(downlink.destAddress, gatewayAddress, downlink.payload);
            loRaBridgeRadio.transmit((const uint8_t *)rfFrame.data(), rfFrame.size());
        }
    }

    // Shift any unconsumed trailing bytes (a partial frame) back to the front of the buffer.
    if (offset > 0)
    {
        memmove(serialBuffer, serialBuffer + offset, serialBufferLength - offset);
        serialBufferLength -= offset;
    }
}
