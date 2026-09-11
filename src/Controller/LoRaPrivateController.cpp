#include "Controller/LoRaPrivateController.h"
#include "Controller/StorageController.h"
#include "Logic/BatteryLogic.h"
#include "Logic/LoRaPrivateSessionLogic.h"
#include <RadioLib.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <esp_task_wdt.h>
#include <cstring>
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "bootloader_random.h"
#include "esp_random.h"

namespace
{
    // Leading "/" required - LittleFS.exists()/open() reject a bare filename (confirmed on real ESP32-S3 hardware).
    const char *CONFIG_FILE = "/loraPrivateRegistration.json";
    // HKDF info string - part of the session-key derivation contract shared with api.LoRa.LoRaPrivatePayloadCrypto, must match byte-for-byte.
    const char *SESSION_KEY_INFO = "agrumy-lora-v2";

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

    // Class-A-style RX1/RX2 downlink windows - confirmed reliable against real air time (SF9, two Heltec V3 boards, 2026-09-08); a real command from the Gateway host normally needs the extra serial round trip so it tends to land in RX2, the bridge's own immediate ack tends to land in RX1.
    const uint32_t DOWNLINK_RX1_TIMEOUT_MS = 1000;
    const uint32_t DOWNLINK_RX2_TIMEOUT_MS = 1000;

    // Own SensorController::pushSensorData-shaped RAM-buffer-then-spill instance, in RTC slow memory (not a plain static) since deep sleep between cycles would otherwise wipe it every boot.
    // 8192 (SensorController's own threshold) overflows this chip's 8KB RTC_SLOW segment once DeviceController.cpp's rtc* variables and RTC_SLOW's own reserved slack are accounted for - trimmed down just enough to link.
    const size_t LORA_BUFFER_SPILL_BYTES = 8100;
    RTC_DATA_ATTR uint8_t rtcUplinkBuffer[LORA_BUFFER_SPILL_BYTES];
    RTC_DATA_ATTR size_t rtcUplinkBufferLen = 0;

    // [2-byte big-endian length][frame bytes] per entry, so several queued frames pack into one file without a JSON wrapper.
    void appendFrameToRtcBuffer(const std::string &frame)
    {
        rtcUplinkBuffer[rtcUplinkBufferLen++] = (uint8_t)((frame.size() >> 8) & 0xFF);
        rtcUplinkBuffer[rtcUplinkBufferLen++] = (uint8_t)(frame.size() & 0xFF);
        memcpy(&rtcUplinkBuffer[rtcUplinkBufferLen], frame.data(), frame.size());
        rtcUplinkBufferLen += frame.size();
    }

    void spillRtcBufferToDisk()
    {
        if (rtcUplinkBufferLen == 0)
        {
            return;
        }
        String blob;
        blob.reserve(rtcUplinkBufferLen);
        for (size_t i = 0; i < rtcUplinkBufferLen; i++)
        {
            blob += (char)rtcUplinkBuffer[i];
        }
        if (!StorageController::bufferLoRaUplinkToDisk(blob))
        {
            Serial.println("[LoRaPrivate] RTC uplink buffer could not be persisted to LittleFS - dropped " + String((unsigned)rtcUplinkBufferLen) + " bytes");
        }
        rtcUplinkBufferLen = 0; // reset either way - a failed spill is deliberate data loss, same convention as StorageController's other buffer types
    }
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

    return true;
}

bool LoRaPrivateController::deriveBootSession()
{
    // bootloader_random_enable() requires WiFi/BT to be off - guaranteed on this profile (AGRUMY_PROFILE_LORA never touches either, see main.cpp's file header).
    bootloader_random_enable();
    esp_fill_random(bootNonce, sizeof(bootNonce));
    bootloader_random_disable();

    bool allZero = true;
    for (uint8_t b : bootNonce)
    {
        if (b != 0)
        {
            allZero = false;
            break;
        }
    }
    if (allZero)
    {
        // Astronomically unlikely (1 in 2^64) for a working RNG - a real hit here means the hardware RNG itself is broken, which would silently break replay protection if transmission proceeded anyway.
        Serial.println("[LoRaPrivate] RNG returned an all-zero bootNonce - refusing to transmit this boot (LoRaRngFault).");
        return false;
    }

    // Hand-rolled RFC 5869 HKDF-SHA256 (extract + one expand round) instead of mbedtls_hkdf - this
    // build's mbedtls doesn't link CONFIG_MBEDTLS_HKDF_C (confirmed: undefined reference at build
    // time), while mbedtls_md_hmac is always available. One expand round is exact per RFC 5869,
    // not an approximation, since the requested 16-byte output fits within a single 32-byte
    // HMAC-SHA256 block (T(1) = HMAC(PRK, info || 0x01), OKM = first 16 bytes of T(1)).
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t prk[32];
    if (mbedtls_md_hmac(md, bootNonce, sizeof(bootNonce), privateKey, sizeof(privateKey), prk) != 0)
    {
        Serial.println("[LoRaPrivate] HKDF-extract failed");
        return false;
    }
    size_t infoLen = strlen(SESSION_KEY_INFO);
    uint8_t expandInput[32];
    memcpy(expandInput, SESSION_KEY_INFO, infoLen);
    expandInput[infoLen] = 0x01;
    uint8_t t1[32];
    if (mbedtls_md_hmac(md, prk, sizeof(prk), expandInput, infoLen + 1, t1) != 0)
    {
        Serial.println("[LoRaPrivate] HKDF-expand failed");
        return false;
    }
    memcpy(sessionKey, t1, sizeof(sessionKey));
    return true;
}

bool LoRaPrivateController::begin()
{
    if (!loadConfig())
    {
        return false;
    }
    if (!deriveBootSession())
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

void LoRaPrivateController::bufferFailedUplink(const std::string &frame)
{
    if (frame.size() + 2 > LORA_BUFFER_SPILL_BYTES)
    {
        Serial.println("[LoRaPrivate] Uplink frame larger than the entire RTC buffer - dropping it");
        return;
    }
    if (rtcUplinkBufferLen + 2 + frame.size() > LORA_BUFFER_SPILL_BYTES)
    {
        spillRtcBufferToDisk();
    }
    appendFrameToRtcBuffer(frame);
}

// Disk backlog (oldest file first) then RTC RAM - same "backlog before a live attempt" ordering as SensorController::pushSensorData; stops at the first failed retransmit, leaving the rest queued.
bool LoRaPrivateController::flushBufferedUplinks()
{
    String filename = StorageController::oldestBufferedLoRaUplinkFile();
    while (!filename.isEmpty())
    {
        String blob = StorageController::loadFile(filename);
        if (blob.isEmpty())
        {
            Serial.println("[LoRaPrivate] Buffered file /" + filename + " unreadable - dropping it");
            StorageController::removeBufferedFile(filename);
            filename = StorageController::oldestBufferedLoRaUplinkFile();
            esp_task_wdt_reset();
            continue;
        }

        size_t pos = 0;
        size_t len = (size_t)blob.length();
        bool allSent = true;
        while (pos + 2 <= len)
        {
            uint16_t frameLen = ((uint8_t)blob[pos] << 8) | (uint8_t)blob[pos + 1];
            pos += 2;
            if (pos + frameLen > len)
            {
                // Truncated/corrupt tail - a poison entry here would wedge the queue forever, so drop the rest of this file and move on.
                Serial.println("[LoRaPrivate] Buffered file /" + filename + " truncated - dropping remainder");
                break;
            }

            int state = loRaPrivateRadio.transmit((const uint8_t *)blob.c_str() + pos, frameLen);
            pos += frameLen;
            if (state != RADIOLIB_ERR_NONE)
            {
                Serial.printf("[LoRaPrivate] Flush retransmit failed, code %d - remaining buffer stays queued\n", state);
                allSent = false;
                break;
            }
        }

        if (!allSent)
        {
            return false;
        }

        Serial.println("[LoRaPrivate] Flushed /" + filename);
        StorageController::removeBufferedFile(filename);

        esp_task_wdt_reset(); // a deep backlog could otherwise outlast the task WDT without a per-file feed
        filename = StorageController::oldestBufferedLoRaUplinkFile();
    }

    // Disk is clear - now drain whatever's still queued in RTC RAM, oldest (front) frame first.
    size_t pos = 0;
    while (pos + 2 <= rtcUplinkBufferLen)
    {
        uint16_t frameLen = ((uint16_t)rtcUplinkBuffer[pos] << 8) | rtcUplinkBuffer[pos + 1];
        if (pos + 2 + frameLen > rtcUplinkBufferLen)
        {
            break; // shouldn't happen - appendFrameToRtcBuffer() never writes a partial frame
        }

        int state = loRaPrivateRadio.transmit(&rtcUplinkBuffer[pos + 2], frameLen);
        if (state != RADIOLIB_ERR_NONE)
        {
            Serial.printf("[LoRaPrivate] RTC buffer retransmit failed, code %d - remaining frames stay queued\n", state);
            break;
        }
        pos += 2 + frameLen;
    }
    if (pos > 0)
    {
        // Compact: drop the frames just sent, keep whatever's left (usually nothing) at the front.
        memmove(rtcUplinkBuffer, &rtcUplinkBuffer[pos], rtcUplinkBufferLen - pos);
        rtcUplinkBufferLen -= pos;
    }

    return rtcUplinkBufferLen == 0;
}

uint32_t LoRaPrivateController::runCycleAndGetSleepSeconds(bool batteryPowered)
{
    if (!configLoaded)
    {
        return (uint32_t)loRaIntervalSecondsForNode(spreadingFactor, batteryPowered);
    }

    bool backlogClear = flushBufferedUplinks();

    LoRaSensorReading reading = readSensors();
    std::string jsonPayload = encodeLoRaSensorUplink(reading);

    // RAM-only, never persisted - replay protection is (bootNonce, counter) never repeating across boots, not the counter alone growing forever, so a plain in-memory increment is safe.
    uint32_t counter = uplinkCounter + 1;
    uplinkCounter = counter;

    uint8_t nonce[12];
    buildLoRaPrivateNonceV2(bootNonce, counter, nonce);
    std::string ciphertext(jsonPayload.size(), '\0');
    uint8_t tag[16];
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, sessionKey, 128); // sessionKey is the 16-byte HKDF output, not the 32-byte master privateKey
    int gcmResult = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, jsonPayload.size(), nonce, sizeof(nonce), nullptr, 0,
                                               (const unsigned char *)jsonPayload.data(), (unsigned char *)&ciphertext[0], sizeof(tag), tag);
    mbedtls_gcm_free(&gcm);
    if (gcmResult != 0)
    {
        Serial.printf("[LoRaPrivate] AES-GCM encrypt failed, code %d\n", gcmResult);
        return (uint32_t)loRaIntervalSecondsForNode(spreadingFactor, batteryPowered);
    }

    std::string wirePayload = encodeLoRaPrivateCipherFrameV2(bootNonce, counter, ciphertext, tag);
    std::string frame = encodeLoRaPrivateFrame(gatewayAddress, nodeAddress, wirePayload);

    if (!backlogClear)
    {
        // Same radio, same failure just now - a live attempt this cycle would likely fail too, so buffer straight away instead of wasting airtime confirming it.
        Serial.println("[LoRaPrivate] Backlog still queued - buffering this uplink instead of a doomed live attempt");
        bufferFailedUplink(frame);
        return (uint32_t)loRaIntervalSecondsForNode(spreadingFactor, batteryPowered);
    }

    int state = loRaPrivateRadio.transmit((const uint8_t *)frame.data(), frame.size());
    if (state != RADIOLIB_ERR_NONE)
    {
        Serial.printf("[LoRaPrivate] Transmit failed, code %d\n", state);
        bufferFailedUplink(frame);
        return (uint32_t)loRaIntervalSecondsForNode(spreadingFactor, batteryPowered);
    }
    Serial.printf("[LoRaPrivate] Sent %u encrypted bytes (counter=%u) to gateway=%u\n", (unsigned)wirePayload.size(), (unsigned)counter, gatewayAddress);

    uint32_t sleepSeconds = (uint32_t)loRaIntervalSecondsForNode(spreadingFactor, batteryPowered);
    bool acked = false;
    // Standard two-window Class-A-style listen: RX1 right after the uplink, RX2 only if RX1 caught nothing - the bridge's own ack (LoRaGatewayBridgeController) normally lands in RX1, RX2 is the retry margin.
    const uint32_t rxTimeoutsMs[2] = {DOWNLINK_RX1_TIMEOUT_MS, DOWNLINK_RX2_TIMEOUT_MS};
    for (int window = 0; window < 2; window++)
    {
        uint8_t downlinkBuf[64];
        state = loRaPrivateRadio.receive(downlinkBuf, sizeof(downlinkBuf), rxTimeoutsMs[window]);
        if (state == RADIOLIB_ERR_RX_TIMEOUT)
        {
            continue;
        }
        if (state != RADIOLIB_ERR_NONE)
        {
            Serial.printf("[LoRaPrivate] Downlink listen error (RX%d), code %d\n", window + 1, state);
            continue;
        }

        size_t len = loRaPrivateRadio.getPacketLength();
        LoRaPrivateFrame downlink;
        if (!decodeLoRaPrivateFrame(downlinkBuf, len, downlink) || downlink.destAddress != nodeAddress)
        {
            continue;
        }

        // A 13-byte v2 payload echoing this uplink's own (bootNonce, counter) is the bridge's ack, not a command - see LoRaGatewayBridgeController::pollRadioForUplink.
        if (isCounterAckV2(downlink.payload, bootNonce, counter))
        {
            Serial.printf("[LoRaPrivate] Uplink counter=%u acknowledged by gateway (RX%d)\n", (unsigned)counter, window + 1);
            acked = true;
            break;
        }

        Serial.printf("[LoRaPrivate] Downlink received: %s\n", downlink.payload.c_str());
        JsonDocument doc;
        if (!deserializeJson(doc, downlink.payload) && doc["retryAfterSeconds"].is<int>())
        {
            sleepSeconds = (uint32_t)doc["retryAfterSeconds"].as<int>();
        }
        break;
    }
    if (!acked)
    {
        Serial.printf("[LoRaPrivate] Uplink counter=%u not acknowledged\n", (unsigned)counter);
    }

    return sleepSeconds;
}
