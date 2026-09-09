#include "InboxController.h"
#include "ServiceController.h"
#include "DeviceController.h"
#include "../Logic/CommandReplayLogic.h"
#include "LittleFS.h"

InboxController inbox;

namespace
{
    // Same path the MQTT channel used before the inbox existed, so an upgraded device keeps its counter instead of re-accepting an old id.
    const char *LAST_COMMAND_ID_FILE = "/mqttLastProcessedCommandId.dat";

    int loadLastProcessedId()
    {
        if (!LittleFS.exists(LAST_COMMAND_ID_FILE))
        {
            return 0;
        }
        File f = LittleFS.open(LAST_COMMAND_ID_FILE, "r");
        if (!f || f.size() < 4)
        {
            if (f)
            {
                f.close();
            }
            return 0;
        }
        uint32_t value = 0;
        for (int i = 0; i < 4; i++)
        {
            value = (value << 8) | (uint8_t)f.read();
        }
        f.close();
        return (int)value;
    }

    const char *sourceName(InboxSource source)
    {
        return source == INBOX_FROM_MQTT ? "mqtt" : "poll";
    }

    const char *decisionName(CommandInboxDecision decision)
    {
        switch (decision)
        {
        case INBOX_REJECT_ALREADY_PROCESSED: return "already processed (id not above the persisted last id)";
        case INBOX_REJECT_EXPIRED: return "expired by the device clock";
        case INBOX_REJECT_CLOCK_UNVERIFIABLE: return "device clock not trusted yet - deferred, the next poll re-delivers it";
        case INBOX_REJECT_NO_EXPIRY: return "expiresAt missing or unparseable";
        default: return "accepted";
        }
    }
}

void InboxController::begin()
{
    lastProcessedId = loadLastProcessedId();
    Serial.printf("[Inbox] Last processed command id: %d\n", lastProcessedId);
}

void InboxController::persistLastProcessedId(int id)
{
    lastProcessedId = id;
    File f = LittleFS.open(LAST_COMMAND_ID_FILE, "w");
    if (!f)
    {
        return;
    }
    uint32_t unsignedValue = (uint32_t)id;
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        f.write((uint8_t)((unsignedValue >> shift) & 0xFF));
    }
    f.close();
}

bool InboxController::handleCommand(DeviceConfig& config, InboxSource source, ServiceRequest serviceRequest, DeviceController& device)
{
    const PendingCommand& command = config.pendingCommand;
    if (!command.present)
    {
        return false;
    }

    // Over MQTT this command's whole job is fetching fresh config; the poll it triggers re-delivers the same id, which is then acked and marked by the poll path - so nothing is marked here.
    if (source == INBOX_FROM_MQTT && command.actionType == COMMAND_FORCE_CONFIG_SYNC)
    {
        handleConfigChanged("mqtt ForceConfigSync", serviceRequest, device);
        return true;
    }

    CommandInboxDecision decision = commandInboxDecision(command.idDeviceCommand, lastProcessedId,
                                                         isoUtcToEpochSeconds(command.expiresAt), (long)device.getEpochSeconds());
    if (decision != INBOX_ACCEPT)
    {
        Serial.printf("[Inbox] Command %d via %s dropped: %s\n", command.idDeviceCommand, sourceName(source), decisionName(decision));
        return false;
    }

    // Marked before the ack and the execute - a Reboot never returns, and a crash mid-execute must not re-run the same id after boot.
    persistLastProcessedId(command.idDeviceCommand);
    Serial.printf("[Inbox] Command %d via %s accepted (actionType=%d)\n", command.idDeviceCommand, sourceName(source), command.actionType);
    service.processPendingCommand(config, serviceRequest, device);
    return true;
}

void InboxController::handleHardReset(const char* origin, DeviceController& device)
{
    Serial.printf("[Inbox] Hard reset requested by admin (%s) - resetting device to defaults\n", origin);
    device.reset(); // never returns
}

void InboxController::handleConfigChanged(const char* origin, ServiceRequest serviceRequest, DeviceController& device)
{
    Serial.printf("[Inbox] Config changed (%s) - polling for fresh config now\n", origin);
    service.apiConfig(deviceConfig, serviceRequest, device);
}
