#ifndef ConfigParser_H
#define ConfigParser_H
#include "Arduino.h"

#include "../Model/DeviceModel.h"

class ConfigParser
{
public:
    // Mutates currentConfig in place (by reference) so every "|" fallback keeps whatever value is already there when the server omits a key - also avoids copy-constructing the (large) DeviceConfig struct on the stack. eventlog error codes: 20 (deserializeJson failure), 21 (missing apiId/apiKey/servicePoint).
    static void parse(const String &configJson, DeviceConfig &currentConfig);

    // Masks the "apiKey":"..." field value in place - shared by parse()'s config-sync log and DeviceController::registerDevice()'s own log.
    static String maskApiKeyInJson(const String &json);

    // Roadmap #370: fully replaces (never partially, unlike maskApiKeyInJson's "first4****last4" style) the value of every "<key>":"<value>" pair whose key contains "password" or "secret", case-insensitive - handles both a plain top-level field and one nested inside another JSON string's escaped quotes (e.g. a WifiPassword inside a pendingCommand.payload string).
    static String redactSensitiveFieldsInJson(const String &json);
};

#endif
