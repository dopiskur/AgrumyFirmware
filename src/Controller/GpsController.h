#ifndef GpsController_H
#define GpsController_H
#include "Arduino.h"

#include "../Model/DeviceModel.h"

// Automatic device location via an external GPS module, AGRUMY_GPS_ENABLED only.
// Wraps TinyGPSPlus over a dedicated HardwareSerial - never blocks setup()/loop() waiting for a fix.
class GpsController
{
public:
    // Returns false (GPS left unused) if GPS_RX/GPS_TX aren't wired for this board (ConfigPin default) - never blocks/retries boot for a missing module.
    bool begin(const ConfigPin &pin);

    // Call every loop() iteration so NMEA bytes queued on the UART are drained before the buffer overruns - updates the cached fix in place, does not return it.
    void tick();

    bool hasFix() const { return fixAcquired; }
    double latitude() const { return lastLatitude; }
    double longitude() const { return lastLongitude; }

private:
    bool ready = false;
    bool fixAcquired = false;
    double lastLatitude = NAN;
    double lastLongitude = NAN;
};

#endif
