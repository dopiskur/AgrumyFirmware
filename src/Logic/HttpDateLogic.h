#ifndef HttpDateLogic_H
#define HttpDateLogic_H

// Pure, native-testable RFC 7231 HTTP Date header parsing - lets ServiceController feed the server's
// clock into DeviceController's serverEpochFallback on EVERY response, not just a full config JSON body.

// Parses an IMF-fixdate Date header ("Tue, 15 Nov 1994 08:12:31 GMT") into Unix epoch seconds; 0 on
// anything that doesn't match, same "no reliable epoch" sentinel DeviceConfig::serverUtcEpoch already uses.
long httpDateToEpochSeconds(const char *httpDate);

#endif
