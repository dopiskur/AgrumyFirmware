#include "RelayIO.h"
#include <Wire.h>

// PCF8574 has no output-state readback distinct from an external pull, so relayRead() answers from this shadow byte instead of an I2C read - same convention as digitalRead() reading back what digitalWrite() last set.
static uint8_t i2cRelayShadow = 0xFF; // all bits high = every relay OFF (active-low expander)
static bool i2cBegun = false;

// Roadmap #365: last endTransmission() outcome - a nonzero return (bus hung/expander not physically present) used to be silently ignored, so the shadow byte (and relayRead()) kept reporting the COMMANDED state, never anything actually verified on the bus.
static bool i2cLastWriteFailed = false;

static void i2cWriteShadow(int i2cAddress)
{
    Wire.beginTransmission(i2cAddress);
    Wire.write(i2cRelayShadow);
    uint8_t result = Wire.endTransmission();
    i2cLastWriteFailed = result != 0;
    if (i2cLastWriteFailed)
    {
        Serial.printf("[RelayIO] I2C write to 0x%02X failed (endTransmission error %u) - relay state on the PCF8574 expander is unverified\n", i2cAddress, result);
    }
}

// True since the last successful i2cWriteShadow() call - checked by ActuatorController once per relay-evaluation pass so a bus fault reaches the server as a DeviceEvent instead of failing silently.
bool relayI2CFaulted()
{
    return i2cLastWriteFailed;
}

// Lazy, idempotent: init happens on first real relay touch rather than depending on setup() wiring.
static void ensureI2CReady(int i2cAddress, int sdaPin, int sclPin)
{
    if (i2cAddress == 0 || i2cBegun)
    {
        return;
    }
    Wire.begin(sdaPin, sclPin);
    Wire.setTimeOut(500); // ms - explicit rather than relying on this Wire implementation's undocumented-in-code 50ms default; a hung bus must not block the main loop indefinitely
    i2cWriteShadow(i2cAddress); // start with every relay off, not whatever power-on-reset left them
    i2cBegun = true;
}

void relayPinMode(int pin, int i2cAddress, int sdaPin, int sclPin)
{
    if (i2cAddress != 0)
    {
        ensureI2CReady(i2cAddress, sdaPin, sclPin); // PCF8574 quasi-bidirectional I/O needs nothing beyond this
        return;
    }
    pinMode(pin, OUTPUT);
}

void relayWrite(int pin, bool on, int i2cAddress, int sdaPin, int sclPin)
{
    if (i2cAddress != 0)
    {
        ensureI2CReady(i2cAddress, sdaPin, sclPin);
        // PCF8574 relay expander is active-LOW: writing 0 turns the relay ON, 1 turns it OFF - opposite of the direct-GPIO HIGH-is-on convention every other kit uses.
        if (on)
        {
            i2cRelayShadow &= ~(1 << pin);
        }
        else
        {
            i2cRelayShadow |= (1 << pin);
        }
        i2cWriteShadow(i2cAddress);
        return;
    }
    digitalWrite(pin, on ? HIGH : LOW);
}

bool relayRead(int pin, int i2cAddress, int sdaPin, int sclPin)
{
    if (i2cAddress != 0)
    {
        ensureI2CReady(i2cAddress, sdaPin, sclPin);
        return (i2cRelayShadow & (1 << pin)) == 0; // 0 bit = relay on (active-low)
    }
    return digitalRead(pin) == HIGH;
}
