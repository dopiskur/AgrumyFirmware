#ifndef CommandReplayLogic_H
#define CommandReplayLogic_H

// Pure, native-testable accept/reject gate for server->device commands, whichever channel delivered
// them (InboxController feeds it idDeviceCommand/expiresAt/now) - a valid signature or a trusted
// TLS poll alone doesn't stop an old, still-valid-looking command from being re-delivered later.

// Parses an ISO-8601 UTC timestamp ("YYYY-MM-DDTHH:MM:SS[.fff]Z", System.Text.Json's DateTime
// format) into Unix epoch seconds; 0 on anything that doesn't match, same "no reliable epoch"
// sentinel HttpDateLogic's httpDateToEpochSeconds already uses.
long isoUtcToEpochSeconds(const char *iso);

enum CommandInboxDecision
{
    INBOX_ACCEPT = 0,
    INBOX_REJECT_ALREADY_PROCESSED = 1, // id not above the persisted last-processed id
    INBOX_REJECT_EXPIRED = 2,
    INBOX_REJECT_CLOCK_UNVERIFIABLE = 3, // nowEpochSeconds below MIN_PLAUSIBLE_EPOCH - deferred, not dropped for good
    INBOX_REJECT_NO_EXPIRY = 4,          // expiresAt missing/unparseable (0)
};

// Fail-closed: anything but INBOX_ACCEPT means don't ack, don't execute - an expiry that can't be checked against a trusted clock waits for the next poll instead of running.
CommandInboxDecision commandInboxDecision(int idDeviceCommand, int lastProcessedCommandId, long expiresAtEpochSeconds, long nowEpochSeconds);

// True for anything but INBOX_ACCEPT.
bool commandIsReplayed(int idDeviceCommand, int lastProcessedCommandId, long expiresAtEpochSeconds, long nowEpochSeconds);

#endif
