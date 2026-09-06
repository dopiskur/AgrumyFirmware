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
/// node picks itself, not negotiated with a network server. Pin mapping and a single-board cycle
/// (sense/encode/transmit/listen-timeout/sleep) confirmed on real Heltec WiFi LoRa 32 V3 hardware
/// (2026-09-06) - the actual two-radio RF exchange with a gateway bridge is still unverified.
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

    uint16_t nodeAddress = 0;
    uint16_t gatewayAddress = 0;
    float frequencyMHz = 868.0;
    uint8_t spreadingFactor = 9;
    float bandwidthKHz = 125.0;
    uint8_t codingRate = 7;
    int8_t txPowerDbm = 22;
    bool configLoaded = false;
};

#endif
