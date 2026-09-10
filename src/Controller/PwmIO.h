#ifndef PWMIO_H
#define PWMIO_H
#include "Arduino.h"

// Dedicated PWM/dimming output via the ESP32's own LEDC peripheral, entirely separate from RelayIO's on/off GPIO/PCF8574 path (a PCF8574-expander relay bus, like KC868-A6's, has no PWM register at all - see ConfigPin.PWM_PINS' own remarks). `pin` is always a direct ESP32 GPIO, never a PCF8574 bit index. frequencyHz is only read the FIRST time this pin is attached (ledcSetup can't be re-run on a live channel without a detach/reattach) - RelaySlot.pwmFrequencyHz, so a slot's frequency is fixed for the device's uptime once first attached.
void pwmPinMode(int pin, uint32_t frequencyHz);

// dutyPercent is clamped to [0,100] by RelayLogic::computePwmDutyPercent before it ever reaches here - this just maps that into the LEDC resolution below.
void pwmWrite(int pin, int dutyPercent);

#endif
