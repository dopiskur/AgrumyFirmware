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
};

#endif
