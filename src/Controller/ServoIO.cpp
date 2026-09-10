#include "ServoIO.h"
#include "../Model/DeviceModel.h"

static const uint32_t SERVO_FREQUENCY_HZ = 50; // standard servo period (20ms), not admin-configurable like PwmIO's dimming frequency
static const uint8_t SERVO_RESOLUTION_BITS = 16; // plenty of headroom at 50Hz for sub-microsecond pulse steps
static const uint32_t SERVO_PERIOD_US = 20000;
static const int SERVO_MIN_SAFE_PULSE_US = 500;
static const int SERVO_MAX_SAFE_PULSE_US = 2500;

// Own channel pool, separate from PwmIO's - a 50Hz/16-bit channel setup is incompatible with PwmIO's admin-configurable kHz-range frequency, so they can never share a channel.
static int attachedPins[MAX_SERVO_SLOTS] = {-1, -1, -1, -1};

void servoPinMode(int pin)
{
    for (int channel = 0; channel < MAX_SERVO_SLOTS; channel++)
    {
        if (attachedPins[channel] == pin)
        {
            return;
        }
    }
    for (int channel = 0; channel < MAX_SERVO_SLOTS; channel++)
    {
        if (attachedPins[channel] == -1)
        {
            // Channel numbers here share the same 0..15 LEDC namespace as PwmIO's pool - offset into the upper half so the two pools never collide on a live channel.
            int ledcChannel = channel + MAX_PWM_SLOTS;
            ledcSetup(ledcChannel, SERVO_FREQUENCY_HZ, SERVO_RESOLUTION_BITS);
            ledcAttachPin(pin, ledcChannel);
            attachedPins[channel] = pin;
            return;
        }
    }
    // No free channel left in the pool - same "silently do nothing rather than steal a live channel" convention as PwmIO.
}

void servoWrite(int pin, int pulseUs)
{
    int channel = -1;
    for (int i = 0; i < MAX_SERVO_SLOTS; i++)
    {
        if (attachedPins[i] == pin)
        {
            channel = i;
            break;
        }
    }
    if (channel < 0)
    {
        return; // servoPinMode() was never called (or the channel pool was full) for this pin
    }

    int clamped = pulseUs < SERVO_MIN_SAFE_PULSE_US ? SERVO_MIN_SAFE_PULSE_US : (pulseUs > SERVO_MAX_SAFE_PULSE_US ? SERVO_MAX_SAFE_PULSE_US : pulseUs);
    uint32_t maxDuty = (1u << SERVO_RESOLUTION_BITS) - 1;
    uint32_t duty = ((uint64_t)clamped * maxDuty) / SERVO_PERIOD_US;
    ledcWrite(channel + MAX_PWM_SLOTS, duty);
}
