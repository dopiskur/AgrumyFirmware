#ifndef DisplayController_H
#define DisplayController_H
#include "Arduino.h"

#include "../Model/DeviceModel.h"
#include "ActuatorController.h"

// Read-only OLED status screen (SSD1306 128x64), KC868-A6 only (dedicated slot, shares
// the PCF8574 relay expander's I2C bus). Strictly passive: never writes deviceConfig or touches relay
// output, only visualizes state the device already has locally. Auto-rotates through a few pages so a
// small screen with no input device can still show everything without scrolling/buttons.
class DisplayController
{
public:
    // Returns false (screen left unused) if the OLED isn't physically detected at ConfigPin.DISPLAY_I2C_ADDRESS - never blocks/retries boot for a missing screen.
    bool begin(const ConfigPin &pin);

    // Call once per main loop() cycle, after sensor/relay state has settled for this cycle - refreshes the cached snapshot tick() redraws from and renders the current page immediately.
    void update(const DeviceConfig &config, const SensorData &sensorData, const ActuatorController &controller, time_t lastConfigSyncEpoch, time_t nowEpoch);

    // Call frequently from the inter-cycle idle wait - advances to the next page once PAGE_MS has elapsed, redrawing from the snapshot update() last cached (never re-reads sensors/relays itself).
    void tick();

private:
    static const unsigned long PAGE_MS = 4000UL;
    static const int PAGE_COUNT = 4;

    void drawCurrentPage();
    void drawIdentityPage();
    void drawReadingsPage();
    void drawRelaysPage();
    void drawStatusPage();

    bool ready = false;
    int currentPage = 0;
    unsigned long lastPageChangeMs = 0;

    // Only the two DeviceConfig fields drawIdentityPage() actually reads - a full DeviceConfig copy here would double this controller's static RAM footprint for nothing.
    int cachedDeviceID = 0;
    String cachedFirmwareVersion;
    SensorData cachedSensorData;
    bool cachedRelayOn[4] = {false, false, false, false}; // Ventilation, Light, Heating, WaterPump - same order as ActuatorController's own functions[4]
    time_t cachedLastSyncEpoch = 0;
    time_t cachedNowEpoch = 0;
};

#endif
