#ifndef SERVOIO_H
#define SERVOIO_H
#include "Arduino.h"

// Dedicated 50Hz servo PWM output via the ESP32's own LEDC peripheral, same channel-pool pattern
// as PwmIO but a fixed 50Hz/high-resolution channel setup (servos need a standard 20ms period, not an admin-
// configurable frequency like PwmIO's dimming output). `pin` is always a direct ESP32 GPIO.
void servoPinMode(int pin);

// pulseUs is clamped to a sane [500,2500] hardware range before being converted to a duty cycle - RelayLogic::
// computeServoPulseUs already maps the slot's own servoMinPulseUs/servoMaxPulseUs into this range upstream.
void servoWrite(int pin, int pulseUs);

#endif
