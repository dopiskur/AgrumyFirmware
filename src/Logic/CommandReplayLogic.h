#ifndef CommandReplayLogic_H
#define CommandReplayLogic_H

// Pure, native-testable replay protection for MQTT-delivered device commands (roadmap #395) - a
// valid HMAC signature alone doesn't stop a previously-captured, still-validly-signed message from
// being republished later; MqttController's onCommandMessage feeds it idDeviceCommand/expiresAt/now.

// Parses an ISO-8601 UTC timestamp ("YYYY-MM-DDTHH:MM:SS[.fff]Z", System.Text.Json's DateTime
// format) into Unix epoch seconds; 0 on anything that doesn't match, same "no reliable epoch"
// sentinel HttpDateLogic's httpDateToEpochSeconds already uses.
long isoUtcToEpochSeconds(const char *iso);

// True when idDeviceCommand is no higher than the last one actually processed (replay of an old,
// still-validly-signed message) or expiresAtEpochSeconds is already in the past by the device's own
// clock. nowEpochSeconds below MIN_PLAUSIBLE_EPOCH means the clock itself isn't trustworthy yet -
// never block on an unverifiable clock, same permissive-until-proven-implausible rule
// ActuatorController's own interval/schedule gates already use.
bool commandIsReplayed(int idDeviceCommand, int lastProcessedCommandId, long expiresAtEpochSeconds, long nowEpochSeconds);

#endif
