#ifndef LoRaController_H
#define LoRaController_H

#include <Arduino.h>
#include "Logic/LoRaIntervalLogic.h"
#include "Logic/LoRaPayloadLogic.h"

/// Orchestrates the LoRa/Profile B device profile: OTAA join with the session
/// persisted to LittleFS (so a battery node skips a full rejoin after deep sleep), one uplink per
/// cycle built from LoRaPayloadLogic, and a sleep interval scaled by the network's current spreading
/// factor via LoRaIntervalLogic. Deliberately narrow next to DeviceController/ServiceController - no
/// WiFi, no OTA, no command queue: EU868's payload limit (as low as 51 bytes at DR0) leaves no room
/// for that here, see the roadmap's own "significantly more limited device profile" note. The radio
/// pin mapping, join flow and session-persistence contract are UNVERIFIED against real
/// hardware/ChirpStack.
class LoRaController
{
public:
    /// Loads persisted join credentials/session and joins the network; false if no credentials file
    /// exists yet (needs pre-provisioning, see README) or the join itself fails.
    bool begin();

    /// One full cycle: read sensors, uplink, parse the downlink's retry hint, return the seconds to
    /// sleep before the next cycle (scaled to the just-negotiated spreading factor and batteryPowered).
    uint32_t runCycleAndGetSleepSeconds(bool batteryPowered);

private:
    bool loadCredentials();
    void persistSession();
    void restoreSession();
    LoRaSensorReading readSensors();

    uint64_t joinEUI = 0;
    uint64_t devEUI = 0;
    uint8_t appKey[16] = {0};
    uint8_t nwkKey[16] = {0};
    bool credentialsLoaded = false;
    // Worst-case default until the first uplink reports a real negotiated spreading factor.
    int lastSpreadingFactor = 12;
};

#endif
