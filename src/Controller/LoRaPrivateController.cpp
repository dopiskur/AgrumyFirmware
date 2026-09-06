#include "Controller/LoRaPrivateController.h"
#include "Logic/BatteryLogic.h"
#include <RadioLib.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <SPI.h>

namespace
{
    // Leading "/" required - LittleFS.exists()/open() reject a bare filename (confirmed on real ESP32-S3 hardware).
    const char *CONFIG_FILE = "/loraPrivateRegistration.json";

    // Heltec WiFi LoRa 32 V3 (ESP32-S3+SX1262) pin mapping - confirmed correct on real hardware (radio.begin() succeeds, 2026-09-06).
    const int PIN_SCK = 9;
    const int PIN_MISO = 11;
    const int PIN_MOSI = 10;
    const int PIN_CS = 8;
    const int PIN_RST = 12;
    const int PIN_BUSY = 13;
    const int PIN_DIO1 = 14;
    const int PIN_BATTERY_ADC = 1;

    // Divider resistors are a guess (1:1) until real hardware confirms the actual values - same caveat as LoRaController.cpp.
    const double BATTERY_DIVIDER_R1_OHMS = 100000.0;
    const double BATTERY_DIVIDER_R2_OHMS = 100000.0;

    // How long to listen for a downlink after each uplink - generous relative to this profile's
    // slow, sparse sensor cadence (seconds, not the millisecond-scale RX windows a LoRaWAN Class A
    // device has to respect), unverified against real air time.
    const uint32_t DOWNLINK_LISTEN_TIMEOUT_MS = 3000;
}

SX1262 loRaPrivateRadio = new Module(PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY);

bool LoRaPrivateController::loadConfig()
{
    if (!LittleFS.exists(CONFIG_FILE))
    {
        Serial.println("[LoRaPrivate] No loraPrivateRegistration.json - device needs pre-provisioning (nodeAddress/gatewayAddress).");
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
        Serial.println("[LoRaPrivate] loraPrivateRegistration.json is malformed.");
        return false;
    }

    nodeAddress = doc["nodeAddress"] | 0;
    gatewayAddress = doc["gatewayAddress"] | 0;
    frequencyMHz = doc["frequencyMHz"] | 868.0;
    spreadingFactor = doc["spreadingFactor"] | 9;
    bandwidthKHz = doc["bandwidthKHz"] | 125.0;
    codingRate = doc["codingRate"] | 7;
    txPowerDbm = doc["txPowerDbm"] | 22;

    configLoaded = true;
    return true;
}

bool LoRaPrivateController::begin()
{
    if (!loadConfig())
    {
        return false;
    }

    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    // syncWord defaults to RADIOLIB_SX126X_SYNC_WORD_PRIVATE - deliberate, this is a private
    // point-to-point protocol, not LoRaWAN.
    int state = loRaPrivateRadio.begin(frequencyMHz, bandwidthKHz, spreadingFactor, codingRate,
                                        RADIOLIB_SX126X_SYNC_WORD_PRIVATE, txPowerDbm);
    if (state != RADIOLIB_ERR_NONE)
    {
        Serial.printf("[LoRaPrivate] Radio init failed, code %d\n", state);
        return false;
    }
    loRaPrivateRadio.setCRC(2);

    Serial.printf("[LoRaPrivate] Radio ready: node=%u gateway=%u freq=%.1fMHz SF%u\n",
                  nodeAddress, gatewayAddress, frequencyMHz, spreadingFactor);
    return true;
}

LoRaSensorReading LoRaPrivateController::readSensors()
{
    LoRaSensorReading reading;
    // Minimal sensor set for now, same as Controller/LoRaController.cpp - battery only, a
    // temperature/humidity sensor is the natural next addition once this profile has real
    // hardware to validate against.
    int raw = analogRead(PIN_BATTERY_ADC);
    double measuredVolts = (raw / 4095.0) * 3.3;
    double batteryVolts = computeDividerBatteryVoltage(measuredVolts, BATTERY_DIVIDER_R1_OHMS, BATTERY_DIVIDER_R2_OHMS);
    reading.battery = computeBatteryPercentFromVoltage(batteryVolts);
    return reading;
}

uint32_t LoRaPrivateController::runCycleAndGetSleepSeconds(bool batteryPowered)
{
    if (!configLoaded)
    {
        return (uint32_t)loRaIntervalSecondsForNode(spreadingFactor, batteryPowered);
    }

    LoRaSensorReading reading = readSensors();
    std::string jsonPayload = encodeLoRaSensorUplink(reading);
    std::string frame = encodeLoRaPrivateFrame(gatewayAddress, nodeAddress, jsonPayload);

    int state = loRaPrivateRadio.transmit((const uint8_t *)frame.data(), frame.size());
    if (state != RADIOLIB_ERR_NONE)
    {
        Serial.printf("[LoRaPrivate] Transmit failed, code %d\n", state);
        return (uint32_t)loRaIntervalSecondsForNode(spreadingFactor, batteryPowered);
    }

    uint8_t downlinkBuf[64];
    state = loRaPrivateRadio.receive(downlinkBuf, sizeof(downlinkBuf), DOWNLINK_LISTEN_TIMEOUT_MS);
    if (state == RADIOLIB_ERR_NONE)
    {
        size_t len = loRaPrivateRadio.getPacketLength();
        LoRaPrivateFrame downlink;
        if (decodeLoRaPrivateFrame(downlinkBuf, len, downlink) && downlink.destAddress == nodeAddress)
        {
            JsonDocument doc;
            if (!deserializeJson(doc, downlink.payload) && doc["retryAfterSeconds"].is<int>())
            {
                return (uint32_t)doc["retryAfterSeconds"].as<int>();
            }
        }
    }

    return (uint32_t)loRaIntervalSecondsForNode(spreadingFactor, batteryPowered);
}
