#include "Arduino.h"
#include <TinyGPSPlus.h>

#include "GpsController.h"

static TinyGPSPlus tinyGps;
static HardwareSerial gpsSerial(1); // UART1 - UART0 stays the USB/programming/debug console, same reservation as every other board's Serial.println() logging.

bool GpsController::begin(const ConfigPin &pin)
{
    if (pin.GPS_RX < 0 || pin.GPS_TX < 0)
    {
        return false;
    }
    gpsSerial.begin(9600, SERIAL_8N1, pin.GPS_RX, pin.GPS_TX); // 9600 baud is the near-universal NMEA default for these modules (u-blox NEO-6M/NEO-8M and clones)
    ready = true;
    return true;
}

void GpsController::tick()
{
    if (!ready)
    {
        return;
    }
    while (gpsSerial.available() > 0)
    {
        tinyGps.encode(gpsSerial.read());
    }
    if (tinyGps.location.isValid() && tinyGps.location.isUpdated())
    {
        lastLatitude = tinyGps.location.lat();
        lastLongitude = tinyGps.location.lng();
        fixAcquired = true;
    }
}
