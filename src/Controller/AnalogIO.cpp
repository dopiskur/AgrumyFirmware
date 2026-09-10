#include "AnalogIO.h"

#if CONFIG_IDF_TARGET_ESP32

void analogPinMode(int pin)
{
    // dacWrite() needs no pinMode()/attach step on classic ESP32 - GPIO25/26 are dedicated DAC pins, always ready.
    (void)pin;
}

void analogWrite8Bit(int pin, int value)
{
    int clamped = value < 0 ? 0 : (value > 255 ? 255 : value);
    dacWrite(pin, clamped);
}

#else

// No DAC peripheral on this target (S3 and others) - ConfigPin.ANALOG_PINS ships all-unassigned (-1) here, so
// these should never actually be called, but are defined as safe no-ops rather than leaving a link error.
void analogPinMode(int pin)
{
    (void)pin;
}

void analogWrite8Bit(int pin, int value)
{
    (void)pin;
    (void)value;
}

#endif
