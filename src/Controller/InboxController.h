#ifndef InboxController_H
#define InboxController_H

#include "Arduino.h"
#include "../Model/DeviceModel.h"

class DeviceController;

// Which delivery path handed the item over - the same command can arrive on both, and ForceConfigSync means something different on each.
enum InboxSource
{
    INBOX_FROM_POLL = 1, // pendingCommand inside a /api/Device/Config response
    INBOX_FROM_MQTT = 2, // signed push on the persistent command channel
};

// The one place every server->device delivery lands (config-poll pendingCommand, MQTT push, admin hard-reset flag, ForceConfigSync), so the replay/expiry gate, the persisted last-processed id and the ack-then-execute order exist exactly once. Fail-closed: a command whose expiry cannot be verified against a trusted clock is left unacked for the next poll to re-deliver.
class InboxController
{
public:
    void begin(); // after LittleFS is mounted

    // False when the command was dropped or deferred (already processed, expired, clock not trusted yet) - nothing acked, nothing executed.
    bool handleCommand(DeviceConfig& config, InboxSource source, ServiceRequest serviceRequest, DeviceController& device);
    void handleHardReset(const char* origin, DeviceController& device); // never returns
    void handleConfigChanged(const char* origin, ServiceRequest serviceRequest, DeviceController& device);

    int lastProcessedCommandId() const { return lastProcessedId; }

private:
    int lastProcessedId = 0;
    void persistLastProcessedId(int id);
};

extern InboxController inbox;

#endif
