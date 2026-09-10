#ifndef ANALOGIO_H
#define ANALOGIO_H
#include "Arduino.h"

// Native ESP32 DAC output (Analog0to10V outputKind, paired with an external op-amp to scale the
// chip's 0-3.3V DAC range up to a real 0-10V signal for a VFD/regulator). CONFIG_IDF_TARGET_ESP32-only - the S3
// silicon dropped the DAC peripheral entirely, see ConfigPin.ANALOG_PINS' own per-board remarks; an MCP4725
// external I2C DAC (the S3-compatible alternative the roadmap item names) is not implemented here yet.
void analogPinMode(int pin);

// value is the raw 0-255 DAC value from RelayLogic::computeAnalogDacValue, not a percent.
void analogWrite8Bit(int pin, int value);

#endif
