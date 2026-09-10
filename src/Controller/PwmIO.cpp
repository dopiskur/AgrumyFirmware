#include "PwmIO.h"
#include "../Model/DeviceModel.h"

// 8-bit (256 steps) is plenty of resolution for DC MOSFET-driven dimming (grow-light LED strips, fan speed) at
// any per-slot frequency the server configures - well above the flicker-visible range and comfortably above
// most SSR/MOSFET switching limits when the admin picks a sane frequency (e.g. 25kHz ventilator, 1kHz LED).
static const uint8_t PWM_RESOLUTION_BITS = 8;

// This framework version only has the OLD channel-based LEDC API (ledcSetup/ledcAttachPin/ledcWrite(channel,...)),
// not the newer pin-based ledcAttach()/ledcWrite(pin,...) - one channel reserved per possible PWM slot, resolved
// by a simple pin->channel lookup so pwmPinMode() stays idempotent to call every tick like RelayIO's relayPinMode.
static int attachedPins[MAX_PWM_SLOTS] = {-1, -1, -1, -1};

void pwmPinMode(int pin, uint32_t frequencyHz)
{
    for (int channel = 0; channel < MAX_PWM_SLOTS; channel++)
    {
        if (attachedPins[channel] == pin)
        {
            return; // already attached to a channel - nothing to do
        }
    }
    for (int channel = 0; channel < MAX_PWM_SLOTS; channel++)
    {
        if (attachedPins[channel] == -1)
        {
            ledcSetup(channel, frequencyHz > 0 ? frequencyHz : 1000, PWM_RESOLUTION_BITS);
            ledcAttachPin(pin, channel);
            attachedPins[channel] = pin;
            return;
        }
    }
    // No free channel left in the pool (more than MAX_PWM_SLOTS distinct pins attached this boot) - silently
    // does nothing rather than reassigning a live channel out from under another pin.
}

void pwmWrite(int pin, int dutyPercent)
{
    int channel = -1;
    for (int i = 0; i < MAX_PWM_SLOTS; i++)
    {
        if (attachedPins[i] == pin)
        {
            channel = i;
            break;
        }
    }
    if (channel < 0)
    {
        return; // pwmPinMode() was never called (or the channel pool was full) for this pin
    }

    int clamped = dutyPercent < 0 ? 0 : (dutyPercent > 100 ? 100 : dutyPercent);
    uint32_t maxDuty = (1u << PWM_RESOLUTION_BITS) - 1;
    ledcWrite(channel, (clamped * maxDuty) / 100);
}
