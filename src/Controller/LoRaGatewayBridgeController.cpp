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

    // Same Heltec WiFi LoRa 32 V3 pin mapping as Controller/LoRaPrivateController.cpp - the bridge
    // and the sensor nodes are expected to be the same board family. NOT physically verified.
    const int PIN_SCK = 9;
    const int PIN_MISO = 11;
    const int PIN_MOSI = 10;
    const int PIN_CS = 8;
    const int PIN_RST = 12;
    const int PIN_BUSY = 13;
    const int PIN_DIO1 = 14;

    // Short poll window per loop() iteration - long enough to catch a packet mid-air, short enough
    // that serial downlinks still get drained promptly. Unverified against real air time.
    const uint32_t RX_POLL_TIMEOUT_MS = 200;
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

    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    int state = loRaBridgeRadio.begin(frequencyMHz, bandwidthKHz, spreadingFactor, codingRate,
                                       RADIOLIB_SX126X_SYNC_WORD_PRIVATE, txPowerDbm);
    if (state != RADIOLIB_ERR_NONE)
    {
        Serial.printf("[LoRaBridge] Radio init failed, code %d\n", state);
        return false;
    }
    loRaBridgeRadio.setCRC(2);

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
    if (state != RADIOLIB_ERR_NONE)
    {
        return; // timeout or CRC failure - nothing to forward this iteration
    }

    size_t len = loRaBridgeRadio.getPacketLength();
    LoRaPrivateFrame frame;
    if (!decodeLoRaPrivateFrame(buf, len, frame) || frame.destAddress != gatewayAddress)
    {
        return; // not addressed to us, or too short to even hold the address header
    }

    int8_t rssi = (int8_t)loRaBridgeRadio.getRSSI();
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
