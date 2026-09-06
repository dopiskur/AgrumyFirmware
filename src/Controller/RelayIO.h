#ifndef RELAYIO_H
#define RELAYIO_H
#include "Arduino.h"

// Routes relay I/O through either direct GPIO or a PCF8574 I2C expander depending on ConfigPin.RELAY_I2C_ADDRESS. `pin` means a GPIO number when i2cAddress==0, a PCF8574 output bit index (0-7) otherwise; sdaPin/sclPin are only read the first time an I2C address is seen (lazy Wire.begin()). activeLow (roadmap #366) only applies to the direct-GPIO path - the PCF8574 path's polarity is fixed by the expander's own wiring convention, ignores it.
void relayPinMode(int pin, int i2cAddress, int sdaPin, int sclPin);
void relayWrite(int pin, bool on, int i2cAddress, int sdaPin, int sclPin, bool activeLow = false);
bool relayRead(int pin, int i2cAddress, int sdaPin, int sclPin, bool activeLow = false);

// True if the last I2C write to the PCF8574 relay expander failed (bus hung/expander not physically present) - roadmap #365, meaningless for a direct-GPIO board (i2cAddress==0) since no I2C write is ever attempted there.
bool relayI2CFaulted();

#endif
