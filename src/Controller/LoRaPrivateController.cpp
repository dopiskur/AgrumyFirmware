#include "Controller/LoRaPrivateController.h"
#include "Logic/BatteryLogic.h"
#include "Logic/LoRaPrivatePayloadFramingLogic.h"
#include <RadioLib.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include "mbedtls/gcm.h"

namespace
{
    // Leading "/" required - LittleFS.exists()/open() reject a bare filename (confirmed on real ESP32-S3 hardware).
    const char *CONFIG_FILE = "/loraPrivateRegistration.json";
    // 8 raw bytes, big-endian - separate from CONFIG_FILE so a per-uplink counter save is a small, fast, isolated write.
    const char *COUNTER_FILE = "/loraPrivateCounter.dat";

    // Heltec WiFi LoRa 32 V3 (ESP32-S3+SX1262) pin mapping - confirmed correct on real hardware (radio.begin() succeeds, 2026-09-06).
    const int PIN_SCK = 9;
    const int PIN_MISO = 11;
    const int PIN_MOSI = 10;
    const int PIN_CS = 8;
    const int PIN_RST = 12;
    const int PIN_BUSY = 13;
    const int PIN_DIO1 = 14;
    const int PIN_BATTERY_ADC = 1;
    // GPIO36 gates the FET powering the SX1262's RF stage - radio.begin() (pure SPI register access) succeeds without it, but transmit/receive radiate nothing until this is driven LOW.
    const int PIN_VEXT = 36;

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

    // Roadmap #395 finding 3 - mandatory from here on, AgrumyService's RelayUplink now rejects any uplink from a device with no key provisioned, so an un-keyed node has nothing useful to transmit.
    String pskHex = doc["psk"] | String("");
    if (pskHex.length() != 64)
    {
        Serial.println("[LoRaPrivate] loraPrivateRegistration.json has no valid 64-char hex 'psk' - device needs (re-)provisioning via DeviceApiController.LoRaPrivateKeyGenerate.");
        return false;
    }
    for (int i = 0; i < 32; i++)
    {
        privateKey[i] = (uint8_t)strtoul(pskHex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
    }

    uplinkCounter = loadCounter();
    return true;
}

uint64_t LoRaPrivateController::loadCounter()
{
    if (!LittleFS.exists(COUNTER_FILE))
    {
        return 0;
    }
    File f = LittleFS.open(COUNTER_FILE, "r");
    if (!f || f.size() < 8)
    {
        if (f)
        {
            f.close();
        }
        return 0;
    }
    uint64_t value = 0;
    for (int i = 0; i < 8; i++)
    {
        value = (value << 8) | (uint8_t)f.read();
    }
    f.close();
    return value;
}

bool LoRaPrivateController::saveCounter(uint64_t value)
{
    File f = LittleFS.open(COUNTER_FILE, "w");
    if (!f)
    {
        return false;
    }
    for (int shift = 56; shift >= 0; shift -= 8)
    {
        f.write((uint8_t)((value >> shift) & 0xFF));
    }
    f.close();
    return true;
}

bool LoRaPrivateController::begin()
{
    if (!loadConfig())
    {
        return false;
    }

    pinMode(PIN_VEXT, OUTPUT);
    digitalWrite(PIN_VEXT, LOW);
    delay(50); // let the RF-stage power rail settle before touching the radio over SPI

    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    // syncWord defaults to RADIOLIB_SX126X_SYNC_WORD_PRIVATE - deliberate, this is a private
    // point-to-point protocol, not LoRaWAN. Default tcxoVoltage left as-is: RadioLib's modSetup()
    // already auto-falls-back from TCXO to XTAL on an oscillator-start error, so this board's plain
    // crystal is handled without forcing tcxoVoltage=0 (which skips that path and fails outright).
    int state = loRaPrivateRadio.begin(frequencyMHz, bandwidthKHz, spreadingFactor, codingRate,
                                        RADIOLIB_SX126X_SYNC_WORD_PRIVATE, txPowerDbm);
    if (state != RADIOLIB_ERR_NONE)
    {
        Serial.printf("[LoRaPrivate] Radio init failed, code %d\n", state);
        return false;
    }
    configLoaded = true;
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

    // Counter saved BEFORE transmit, not after - a crash/power-loss between transmit and save could otherwise let the same counter (and its nonce) be reused on the next boot, which breaks AES-GCM's security guarantee.
    uint64_t counter = uplinkCounter + 1;
    if (!saveCounter(counter))
    {
        Serial.println("[LoRaPrivate] Could not persist uplink counter - skipping this uplink rather than risk nonce reuse.");
        return (uint32_t)loRaIntervalSecondsForNode(spreadingFactor, batteryPowered);
    }
    uplinkCounter = counter;

    uint8_t nonce[12] = {0}; // 4 zero bytes + the 8-byte counter, matching api.LoRa.LoRaPrivatePayloadCrypto's nonce derivation
    for (int i = 0; i < 8; i++)
    {
        nonce[4 + i] = (uint8_t)((counter >> (56 - i * 8)) & 0xFF);
    }
    std::string ciphertext(jsonPayload.size(), '\0');
    uint8_t tag[16];
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, privateKey, 256);
    int gcmResult = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, jsonPayload.size(), nonce, sizeof(nonce), nullptr, 0,
                                               (const unsigned char *)jsonPayload.data(), (unsigned char *)&ciphertext[0], sizeof(tag), tag);
    mbedtls_gcm_free(&gcm);
    if (gcmResult != 0)
    {
        Serial.printf("[LoRaPrivate] AES-GCM encrypt failed, code %d\n", gcmResult);
        return (uint32_t)loRaIntervalSecondsForNode(spreadingFactor, batteryPowered);
    }

    std::string wirePayload = encodeLoRaPrivateCipherFrame(counter, ciphertext, tag);
    std::string frame = encodeLoRaPrivateFrame(gatewayAddress, nodeAddress, wirePayload);

    int state = loRaPrivateRadio.transmit((const uint8_t *)frame.data(), frame.size());
    if (state != RADIOLIB_ERR_NONE)
    {
        Serial.printf("[LoRaPrivate] Transmit failed, code %d\n", state);
        return (uint32_t)loRaIntervalSecondsForNode(spreadingFactor, batteryPowered);
    }
    Serial.printf("[LoRaPrivate] Sent %u encrypted bytes (counter=%llu) to gateway=%u\n", (unsigned)wirePayload.size(), (unsigned long long)counter, gatewayAddress);

    uint8_t downlinkBuf[64];
    state = loRaPrivateRadio.receive(downlinkBuf, sizeof(downlinkBuf), DOWNLINK_LISTEN_TIMEOUT_MS);
    if (state == RADIOLIB_ERR_NONE)
    {
        size_t len = loRaPrivateRadio.getPacketLength();
        LoRaPrivateFrame downlink;
        if (decodeLoRaPrivateFrame(downlinkBuf, len, downlink) && downlink.destAddress == nodeAddress)
        {
            Serial.printf("[LoRaPrivate] Downlink received: %s\n", downlink.payload.c_str());
            JsonDocument doc;
            if (!deserializeJson(doc, downlink.payload) && doc["retryAfterSeconds"].is<int>())
            {
                return (uint32_t)doc["retryAfterSeconds"].as<int>();
            }
        }
    }
    else if (state != RADIOLIB_ERR_RX_TIMEOUT)
    {
        Serial.printf("[LoRaPrivate] Downlink listen error, code %d\n", state);
    }

    return (uint32_t)loRaIntervalSecondsForNode(spreadingFactor, batteryPowered);
}
