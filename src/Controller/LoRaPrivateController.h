#ifndef LoRaPrivateController_H
#define LoRaPrivateController_H

#include <Arduino.h>
#include "Logic/LoRaIntervalLogic.h"
#include "Logic/LoRaPayloadLogic.h"
#include "Logic/LoRaPrivateFrameLogic.h"

/// Orchestrates the LoRa private-protocol sensor-node profile (RadioLib raw PHY, private sync word,
/// no LoRaWAN join/session) - alternative to Controller/LoRaController's ChirpStack-facing profile,
/// paired with a Gateway running GatewayProfile.LoRaPrivateProtocol and its radio-frontend board (see
/// Controller/LoRaGatewayBridgeController). No ADR: the spreading factor is a fixed config value this
/// node picks itself, not negotiated with a network server. Uplink, the encrypted wire format, and a
/// real two-window (RX1/RX2) downlink round trip including the bridge's own ack have all been
/// confirmed end to end on real Heltec WiFi LoRa 32 V3 hardware (2026-09-08).
class LoRaPrivateController
{
public:
    /// Loads persisted node/gateway addresses and radio config, then initializes the radio; false if
    /// no config file exists yet (needs pre-provisioning) or radio init fails.
    bool begin();

    /// One full cycle: read sensors, transmit one uplink frame to the configured gateway address,
    /// listen briefly for a downlink reply, return the seconds to sleep before the next cycle.
    uint32_t runCycleAndGetSleepSeconds(bool batteryPowered);

private:
    bool loadConfig();
    LoRaSensorReading readSensors();
    /// Seeds bootNonce from the hardware RNG and derives sessionKey via HKDF - false (refuses to transmit this boot) if the RNG ever hands back an all-zero nonce.
    bool deriveBootSession();

    /// Retransmits /lorabuffer files then queued RTC-RAM frames, oldest first; false if frames are still queued after.
    bool flushBufferedUplinks();
    /// Appends a failed uplink frame to the RTC buffer, spilling it to /lorabuffer first if it wouldn't fit.
    void bufferFailedUplink(const std::string &frame);

    uint16_t nodeAddress = 0;
    uint16_t gatewayAddress = 0;
    float frequencyMHz = 868.0;
    uint8_t spreadingFactor = 9;
    float bandwidthKHz = 125.0;
    uint8_t codingRate = 7;
    int8_t txPowerDbm = 22;
    bool configLoaded = false;

    // Roadmap #395 finding 3 - HKDF input key material, hex-decoded from loraPrivateRegistration.json's "psk" field; a missing/malformed key is treated as "needs pre-provisioning", same as a missing config file. Never used directly for encryption post-#468 - only sessionKey below is.
    uint8_t privateKey[32] = {0};
    // Roadmap #468 - random per boot (hardware RNG), never persisted; replaces the old LittleFS-backed monotonic counter so a uplink costs zero flash writes.
    uint8_t bootNonce[8] = {0};
    // HKDF-SHA256(salt=bootNonce, ikm=privateKey, info="agrumy-lora-v2", length=16) - derived once per boot in deriveBootSession(), used for every uplink this boot.
    uint8_t sessionKey[16] = {0};
    // RAM-only, resets to 0 every boot - replay protection now rests on (bootNonce, counter) never repeating, not on the counter alone ever growing.
    uint32_t uplinkCounter = 0;
};

#endif
