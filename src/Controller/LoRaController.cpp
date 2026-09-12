#include "Controller/LoRaController.h"
#include "Logic/BatteryLogic.h"
#include <RadioLib.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <SPI.h>

namespace
{
    // Leading "/" required - LittleFS.exists()/open() reject a bare filename (confirmed on real
    // ESP32-S3 hardware via Controller/LoRaGatewayBridgeController's identical bug, same fix here).
    const char *CREDENTIALS_FILE = "/loraRegistration.json";
    const char *NONCES_FILE = "/loraNonces.bin";
    const char *SESSION_FILE = "/loraSession.bin";

    // TTGO LoRa32 V2.1 (SX1276, EU868) pin mapping - NOT physically verified on this project's
    // hardware, same caveat as the kc868-a6/esp32-s3-relay-6ch envs' pin mappings.
    const int PIN_SCK = 5;
    const int PIN_MISO = 19;
    const int PIN_MOSI = 27;
    const int PIN_CS = 18;
    const int PIN_RST = 23;
    const int PIN_DIO0 = 26;
    const int PIN_DIO1 = 33;
    const int PIN_BATTERY_ADC = 35;

    // Divider resistors are a guess (1:1) until real hardware confirms the actual values - same
    // "unverified" status as the pin mapping above.
    const double BATTERY_DIVIDER_R1_OHMS = 100000.0;
    const double BATTERY_DIVIDER_R2_OHMS = 100000.0;

    void hexToBytes(const char *hex, uint8_t *out, int count)
    {
        for (int i = 0; i < count; i++)
        {
            char byteStr[3] = {hex[i * 2], hex[i * 2 + 1], 0};
            out[i] = (uint8_t)strtoul(byteStr, nullptr, 16);
        }
    }
}

SX1276 loRaRadio = new Module(PIN_CS, PIN_DIO0, PIN_RST, PIN_DIO1);
LoRaWANNode loRaNode(&loRaRadio, &EU868, 0);

bool LoRaController::loadCredentials()
{
    if (!LittleFS.exists(CREDENTIALS_FILE))
    {
        Serial.println("[LoRa] No loraRegistration.json - device needs pre-provisioning (DevEUI/JoinEUI/AppKey/NwkKey).");
        return false;
    }
    File f = LittleFS.open(CREDENTIALS_FILE, "r");
    if (!f)
    {
        return false;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err)
    {
        Serial.println("[LoRa] loraRegistration.json is malformed.");
        return false;
    }

    joinEUI = strtoull(doc["joinEUI"] | "0", nullptr, 16);
    devEUI = strtoull(doc["devEUI"] | "0", nullptr, 16);
    hexToBytes(doc["appKey"] | "00000000000000000000000000000000", appKey, 16);
    hexToBytes(doc["nwkKey"] | "00000000000000000000000000000000", nwkKey, 16);

    credentialsLoaded = true;
    return true;
}

void LoRaController::restoreSession()
{
    if (LittleFS.exists(NONCES_FILE))
    {
        File f = LittleFS.open(NONCES_FILE, "r");
        uint8_t buf[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
        if (f && f.read(buf, sizeof(buf)) == sizeof(buf))
        {
            loRaNode.setBufferNonces(buf);
        }
        if (f)
        {
            f.close();
        }
    }
    if (LittleFS.exists(SESSION_FILE))
    {
        File f = LittleFS.open(SESSION_FILE, "r");
        uint8_t buf[RADIOLIB_LORAWAN_SESSION_BUF_SIZE];
        if (f && f.read(buf, sizeof(buf)) == sizeof(buf))
        {
            loRaNode.setBufferSession(buf);
        }
        if (f)
        {
            f.close();
        }
    }
}

void LoRaController::persistSession()
{
    File nonceFile = LittleFS.open(NONCES_FILE, "w");
    if (nonceFile)
    {
        nonceFile.write(loRaNode.getBufferNonces(), RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
        nonceFile.close();
    }
    File sessionFile = LittleFS.open(SESSION_FILE, "w");
    if (sessionFile)
    {
        sessionFile.write(loRaNode.getBufferSession(), RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
        sessionFile.close();
    }
}

bool LoRaController::begin()
{
    if (!loadCredentials())
    {
        return false;
    }

    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    // The frequency here only seeds the radio - LoRaWANNode retunes every channel itself once joined.
    int state = loRaRadio.begin(868.0);
    if (state != RADIOLIB_ERR_NONE)
    {
        Serial.printf("[LoRa] Radio init failed, code %d\n", state);
        return false;
    }

    state = loRaNode.beginOTAA(joinEUI, devEUI, nwkKey, appKey);
    if (state != RADIOLIB_ERR_NONE)
    {
        Serial.printf("[LoRa] beginOTAA failed, code %d\n", state);
        return false;
    }

    restoreSession();
    if (loRaNode.isActivated())
    {
        Serial.println("[LoRa] Session restored from LittleFS, skipping join.");
        return true;
    }

    state = loRaNode.activateOTAA();
    if (state != RADIOLIB_LORAWAN_NEW_SESSION)
    {
        Serial.printf("[LoRa] Join failed, code %d\n", state);
        return false;
    }
    persistSession();
    Serial.println("[LoRa] Joined network, session persisted.");
    return true;
}

LoRaSensorReading LoRaController::readSensors()
{
    LoRaSensorReading reading;
    // Minimal Profile B sensor set for now - battery only; a temperature/humidity sensor is the
    // natural next addition once this profile has real hardware to validate against.
    int raw = analogRead(PIN_BATTERY_ADC);
    double measuredVolts = (raw / 4095.0) * 3.3;
    double batteryVolts = computeDividerBatteryVoltage(measuredVolts, BATTERY_DIVIDER_R1_OHMS, BATTERY_DIVIDER_R2_OHMS);
    reading.battery = computeBatteryPercentFromVoltage(batteryVolts);
    return reading;
}

uint32_t LoRaController::runCycleAndGetSleepSeconds(bool batteryPowered)
{
    if (!credentialsLoaded)
    {
        return (uint32_t)loRaIntervalSecondsForNode(lastSpreadingFactor, batteryPowered);
    }

    LoRaSensorReading reading = readSensors();
    std::string payload = encodeLoRaSensorUplink(reading);

    uint8_t downlink[64];
    size_t downlinkLen = sizeof(downlink);
    LoRaWANEvent_t eventUp;
    LoRaWANEvent_t eventDown;
    int state = loRaNode.sendReceive((const uint8_t *)payload.data(), payload.size(), 1,
                                       downlink, &downlinkLen, false, &eventUp, &eventDown);

    lastSpreadingFactor = loRaSpreadingFactorForEu868DataRate(eventUp.datarate);
    // Frame counters advance on every send regardless of a downlink arriving - persist every cycle.
    persistSession();

    if (state > 0 && downlinkLen > 0)
    {
        JsonDocument doc;
        if (!deserializeJson(doc, downlink, downlinkLen) && doc["retryAfterSeconds"].is<int>())
        {
            return (uint32_t)doc["retryAfterSeconds"].as<int>();
        }
    }

    return (uint32_t)loRaIntervalSecondsForNode(lastSpreadingFactor, batteryPowered);
}
