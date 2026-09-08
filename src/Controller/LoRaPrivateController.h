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
/// node picks itself, not negotiated with a network server. Full cycle including a real two-radio
/// uplink to a gateway bridge confirmed on real Heltec WiFi LoRa 32 V3 hardware (2026-09-06) - that
/// run predates this controller's AES-256-GCM payload encryption (added 2026-09-07), so it verified
/// plaintext framing only, not the current encrypted wire format; a real downlink is still unverified.
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
    uint64_t loadCounter();
    bool saveCounter(uint64_t value);

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

    // Roadmap #395 finding 3 - AES-256-GCM key for uplink encryption, hex-decoded from loraPrivateRegistration.json's "psk" field; a missing/malformed key is treated as "needs pre-provisioning", same as a missing config file.
    uint8_t privateKey[32] = {0};
    // Persisted separately from CONFIG_FILE (own small file) so a routine per-uplink counter save never risks rewriting/corrupting the provisioned psk/addresses.
    uint64_t uplinkCounter = 0;
};

#endif
