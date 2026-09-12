#include "Arduino.h"
#include "ArduinoJson.h"

#include "ConfigParser.h"
#include "ServiceController.h"
#include "../Logic/ConfigParseLogic.h"
#include "../Logic/SleepScheduleLogic.h"

String ConfigParser::maskApiKeyInJson(const String &json)
{
  const String needle = "\"apiKey\":\"";
  int start = json.indexOf(needle);
  if (start < 0)
  {
    return json;
  }
  start += needle.length();
  int end = json.indexOf('"', start);
  if (end < 0)
  {
    return json;
  }
  return json.substring(0, start) + ServiceController::maskSecret(json.substring(start, end)) + json.substring(end);
}

static bool containsIgnoreCase(const String &haystack, const char *needle)
{
  String h = haystack;
  h.toLowerCase();
  return h.indexOf(needle) >= 0;
}

String ConfigParser::redactSensitiveFieldsInJson(const String &json)
{
  String result = json;
  int pos = 0;
  while (pos < (int)result.length())
  {
    int keyQuoteStart = result.indexOf('"', pos);
    if (keyQuoteStart < 0)
    {
      break;
    }
    // An escaped quote (\") means this key sits inside another JSON string's already-escaped text (e.g. a pendingCommand.payload value) - the matching close is also escaped, not a bare ".
    bool escaped = keyQuoteStart > 0 && result.charAt(keyQuoteStart - 1) == '\\';
    int keyTextStart = keyQuoteStart + 1;
    int keyQuoteEnd = result.indexOf(escaped ? "\\\"" : "\"", keyTextStart);
    if (keyQuoteEnd < 0)
    {
      break;
    }
    String key = result.substring(keyTextStart, keyQuoteEnd);
    pos = keyQuoteEnd + (escaped ? 2 : 1);

    if (!containsIgnoreCase(key, "password") && !containsIgnoreCase(key, "secret"))
    {
      continue;
    }

    int colon = result.indexOf(':', pos);
    if (colon < 0)
    {
      break;
    }
    int valQuoteStart = result.indexOf('"', colon);
    if (valQuoteStart < 0)
    {
      break;
    }
    bool valEscaped = result.charAt(valQuoteStart - 1) == '\\';
    int valTextStart = valQuoteStart + 1;
    int valQuoteEnd = result.indexOf(valEscaped ? "\\\"" : "\"", valTextStart);
    if (valQuoteEnd < 0)
    {
      break;
    }

    const String replacement = "[REDACTED]";
    result = result.substring(0, valTextStart) + replacement + result.substring(valQuoteEnd);
    pos = valTextStart + replacement.length() + (valEscaped ? 2 : 1);
  }
  return result;
}

void ConfigParser::parse(const String &configJson, DeviceConfig &currentConfig)
{
  Serial.println("[Device] Load config: " + maskApiKeyInJson(redactSensitiveFieldsInJson(configJson)));

  JsonDocument config;
  DeserializationError error = deserializeJson(config, configJson);

  if (error)
  {
    Serial.print("[Device] Load Config; deserializeJson() failed: ");
    Serial.println(error.c_str());
    currentConfig.eventlog.error = true;
    currentConfig.eventlog.errorCode = 20; // 10 is reserved for registerDevice's own gate
    copyStr(currentConfig.eventlog.errorData, error.c_str());

    return;
  }

  String servicePoint = config["servicePoint"];
  // "| """ required - a bare null assignment doesn't reliably yield an empty ArduinoJson String, which would wrongly trip servicePublicKey.length()>0 downstream and feed garbage into setCACert().
  String servicePublicKey = config["servicePublicKey"] | "";
  String apiId = config["apiId"];
  String apiKey = config["apiKey"];

  // "{}" (or any contract-drifted payload) is valid, non-empty JSON, so it passes the deserializeJson gate above with missing keys silently reading back "". Reject BEFORE any deviceConfig field is touched, rather than overwriting identity with blanks.
  if (apiId.isEmpty() || apiKey.isEmpty() || servicePoint.isEmpty())
  {
    Serial.println("[Device] Load Config: missing required apiId/apiKey/servicePoint - rejecting (contract drift or malformed payload), keeping current config");
    currentConfig.eventlog.error = true;
    currentConfig.eventlog.errorCode = 21; // 20 is deserializeJson failure, 10 is reserved for registerDevice's own gate
    copyStr(currentConfig.eventlog.errorData, "missing apiId/apiKey/servicePoint");
    return;
  }
  // currentConfig is re-parsed in place on every call, so a failure flagged above must not linger into the next call that succeeds.
  currentConfig.eventlog.error = false;

  int serverSchemaVersion = config["schemaVersion"] | 0; // 0 from a server build that predates this field
  if (isConfigSchemaNewerThanFirmware(serverSchemaVersion, CONFIG_SCHEMA_VERSION))
  {
    Serial.printf("[Device] Config schemaVersion %d is newer than this firmware understands (%d) - some new fields may be silently ignored, consider an OTA update\n", serverSchemaVersion, CONFIG_SCHEMA_VERSION);
  }

  currentConfig.configVersion = config["configVersion"];

  currentConfig.tenantID = config["tenantID"];
  currentConfig.deviceID = config["deviceID"];
  currentConfig.deviceFarmUnitID = config["deviceFarmUnitID"];
  currentConfig.deviceFarmUnitZoneID = config["deviceFarmUnitZoneID"];
  currentConfig.deviceTypeServiceID = config["deviceTypeServiceID"]; // 0 http, 1 https, 2 mqtt

  copyStr(currentConfig.apiId, apiId.c_str());
  copyStr(currentConfig.apiKey, apiKey.c_str());
  copyStr(currentConfig.servicePoint, servicePoint.c_str());
  copyStr(currentConfig.servicePublicKey, servicePublicKey.c_str());

  // Floored here regardless of server-side validation - a sensor-only device has no controller-side floor to fall back on (ActuatorController::computeNextWakeSeconds only applies to relay-type devices), so 0/negative would otherwise loop with no delay.
  int requestedSleepSeconds = config["sleepSeconds"];
  currentConfig.sleepSeconds = clampToSleepFloor(requestedSleepSeconds, MIN_SLEEP_SECONDS);
  currentConfig.sleepDeep = config["sleepDeep"];
  currentConfig.loRaGatewayEnabled = config["loRaGatewayEnabled"] | false;
  // Keeps the current offset if an older server doesn't send this key - never silently jump to UTC just because the key was missing.
  currentConfig.utcOffsetSeconds = config["utcOffsetSeconds"] | currentConfig.utcOffsetSeconds;
  currentConfig.serverUtcEpoch = config["serverUtcEpoch"] | 0L;
  currentConfig.deviceSensorEnabled = config["deviceSensorEnabled"];
  currentConfig.deviceControllerEnabled = config["deviceControllerEnabled"];
  currentConfig.batteryEnabled = config["batteryEnabled"];
  currentConfig.enabled = config["enabled"];
  currentConfig.debug = config["debug"];
  currentConfig.reset = config["reset"];
  currentConfig.emergencyStop = config["emergencyStop"] | false;
  currentConfig.firmwareUpdate = config["firmwareUpdate"];
  copyStr(currentConfig.firmwareVersion, config["firmwareVersion"] | "");
  copyStr(currentConfig.firmwareUrl, config["firmwareUrl"] | "");
  copyStr(currentConfig.firmwareSha256, config["firmwareSha256"] | "");

  JsonVariant pendingCommandJson = config["pendingCommand"];
  if (pendingCommandJson.isNull())
  {
    currentConfig.pendingCommand.present = false;
  }
  else
  {
    currentConfig.pendingCommand.present = true;
    currentConfig.pendingCommand.idDeviceCommand = pendingCommandJson["idDeviceCommand"];
    currentConfig.pendingCommand.actionType = pendingCommandJson["actionType"];
    copyStr(currentConfig.pendingCommand.expiresAt, pendingCommandJson["expiresAt"] | "");
    copyStr(currentConfig.pendingCommand.payload, pendingCommandJson["payload"] | "");
  }

  if (currentConfig.deviceSensorEnabled)
  {
    JsonObject deviceConfigSensor = config["deviceConfigSensor"];

    currentConfig.configSensor.sensorBattery = deviceConfigSensor["sensorBattery"];
    // Falls back to the existing value so an older server that omits these keys doesn't zero out a configured divider calibration.
    currentConfig.configSensor.batteryDividerR1 = deviceConfigSensor["batteryDividerR1"] | currentConfig.configSensor.batteryDividerR1;
    currentConfig.configSensor.batteryDividerR2 = deviceConfigSensor["batteryDividerR2"] | currentConfig.configSensor.batteryDividerR2;
    currentConfig.configSensor.sensorTemp = deviceConfigSensor["sensorTemp"];
    currentConfig.configSensor.sensorTempSoil = deviceConfigSensor["sensorTempSoil"];
    currentConfig.configSensor.sensorHumid = deviceConfigSensor["sensorHumid"];
    currentConfig.configSensor.sensorMoist = deviceConfigSensor["sensorMoist"];
    currentConfig.configSensor.sensorLight = deviceConfigSensor["sensorLight"];
    currentConfig.configSensor.sensorCo2 = deviceConfigSensor["sensorCo2"];
    currentConfig.configSensor.sensorTvoc = deviceConfigSensor["sensorTvoc"];
    currentConfig.configSensor.sensorBarometer = deviceConfigSensor["sensorBarometer"];
    currentConfig.configSensor.sensorPH = deviceConfigSensor["sensorPH"];
    currentConfig.configSensor.sensorRainLevel = deviceConfigSensor["sensorRainLevel"];
    currentConfig.configSensor.sensorWaterLevel = deviceConfigSensor["sensorWaterLevel"];
    currentConfig.configSensor.sensorWind = deviceConfigSensor["sensorWind"];
    currentConfig.configSensor.sensorEc = deviceConfigSensor["sensorEc"];
    currentConfig.configSensor.sensorWeight = deviceConfigSensor["sensorWeight"];
    currentConfig.configSensor.weightCalibrationFactor = deviceConfigSensor["weightCalibrationFactor"] | currentConfig.configSensor.weightCalibrationFactor;
    currentConfig.configSensor.weightTareOffset = deviceConfigSensor["weightTareOffset"] | currentConfig.configSensor.weightTareOffset;
    currentConfig.configSensor.ecCalibrationSlope = deviceConfigSensor["ecCalibrationSlope"] | currentConfig.configSensor.ecCalibrationSlope;
    currentConfig.configSensor.ecCalibrationOffset = deviceConfigSensor["ecCalibrationOffset"] | currentConfig.configSensor.ecCalibrationOffset;
  }

  if (currentConfig.deviceControllerEnabled)
  {
    JsonObject deviceConfigController = config["deviceConfigController"];

    // Capped at MAX_RULES - ArduinoJson has no dynamic growth on-device, extras are silently dropped (server enforces a matching cap, and this is a whole-rule cap, not the per-rule node-tree rejection handled below).
    JsonArray rules = deviceConfigController["rules"];
    currentConfig.configController.ruleCount = 0;
    currentConfig.rulesRejectedCount = 0;
    for (JsonObject r : rules)
    {
        if (currentConfig.configController.ruleCount >= MAX_RULES)
        {
            break;
        }

        Rule candidate;
        if (!parseRule(r, candidate))
        {
            // Unrecognized node type, too many total nodes, or a group with too many/zero children -
            // reject the WHOLE rule rather than store a partial/broken tree (same spirit as the old
            // flat model's "a bad condition rejects the whole rule").
            currentConfig.rulesRejectedCount++;
            continue;
        }
        currentConfig.configController.rules[currentConfig.configController.ruleCount] = candidate;
        currentConfig.configController.ruleCount++;
    }

    currentConfig.configController.waterPumpMaxRunSeconds = deviceConfigController["waterPumpMaxRunSeconds"] | currentConfig.configController.waterPumpMaxRunSeconds;
    currentConfig.configController.waterPumpCooldownSeconds = deviceConfigController["waterPumpCooldownSeconds"] | currentConfig.configController.waterPumpCooldownSeconds;
    currentConfig.configController.waterPumpMinLevel = deviceConfigController["waterPumpMinLevel"] | currentConfig.configController.waterPumpMinLevel;
    currentConfig.configController.waterLevelRawEmpty = deviceConfigController["waterLevelRawEmpty"] | currentConfig.configController.waterLevelRawEmpty;
    currentConfig.configController.waterLevelRawFull = deviceConfigController["waterLevelRawFull"] | currentConfig.configController.waterLevelRawFull;

    // Falls back to the current value so an older server build can't accidentally re-arm a pump the last sync deliberately vetoed.
    currentConfig.configController.skipWaterPumpForRain = deviceConfigController["skipWaterPumpForRain"] | currentConfig.configController.skipWaterPumpForRain;

    // Absent/null (zone never set one) falls back to 0 (Hold) - see ConfigController::heatingFailSafePolicy's own remarks for the value convention.
    currentConfig.configController.heatingFailSafePolicy = deviceConfigController["heatingFailSafePolicy"] | 0;

    // Capped at MAX_MANUAL_OVERRIDES, same "ArduinoJson has no dynamic growth on-device" reasoning as rules/relays above.
    JsonArray manualOverrides = deviceConfigController["manualOverrides"];
    currentConfig.configController.manualOverrideCount = 0;
    for (JsonObject mo : manualOverrides)
    {
        if (currentConfig.configController.manualOverrideCount >= MAX_MANUAL_OVERRIDES)
        {
            break;
        }
        ManualOverride &override = currentConfig.configController.manualOverrides[currentConfig.configController.manualOverrideCount];
        override.relayFunction = mo["relayFunction"];
        override.mode = mo["mode"];
        override.expiresAtEpoch = (time_t)(long)mo["expiresAtEpoch"];
        override.targetMetric = mo["targetMetric"] | 0;
        override.targetThreshold = mo["targetThreshold"] | 0.0;
        override.targetHysteresis = mo["targetHysteresis"] | 0.0;
        currentConfig.configController.manualOverrideCount++;
    }

    currentConfig.configController.relayEnabled = deviceConfigController["relayEnabled"];

    // Capped at MAX_RELAY_SLOTS - same "ArduinoJson has no dynamic growth on-device" reasoning as rules above; only slots the server actually assigned ride along, an unlisted slot is unassigned. outputKind and every kind-specific field ride the same slot object now (the old separate pwmSlots[] array is gone, a Pwm-output function is just a relays[] entry with outputKind=Pwm).
    JsonArray relays = deviceConfigController["relays"];
    currentConfig.configController.relayCount = 0;
    for (JsonObject r : relays)
    {
        if (currentConfig.configController.relayCount >= MAX_RELAY_SLOTS)
        {
            break;
        }
        RelaySlot &relaySlot = currentConfig.configController.relays[currentConfig.configController.relayCount];
        relaySlot.slot = r["slot"];
        relaySlot.relayFunction = r["relayFunction"];
        relaySlot.outputKind = r["outputKind"] | OUTPUT_KIND_RELAY;
        relaySlot.pairSlot = r["pairSlot"] | 0;
        relaySlot.travelSeconds = r["travelSeconds"] | 0;
        relaySlot.deadTimeSeconds = r["deadTimeSeconds"] | 0;
        relaySlot.pwmFrequencyHz = r["pwmFrequencyHz"] | 1000;
        relaySlot.servoMinPulseUs = r["servoMinPulseUs"] | 1000;
        relaySlot.servoMaxPulseUs = r["servoMaxPulseUs"] | 2000;
        relaySlot.servoSafePositionPercent = r["servoSafePositionPercent"] | 0;
        relaySlot.latchingPulseMs = r["latchingPulseMs"] | 250;
        relaySlot.rateLimitPercentPerSecond = r["rateLimitPercentPerSecond"] | 0;
        relaySlot.minOnSeconds = r["minOnSeconds"] | 0;
        relaySlot.minOffSeconds = r["minOffSeconds"] | 0;
        relaySlot.timeProportioningPeriodSeconds = r["timeProportioningPeriodSeconds"] | 0;
        currentConfig.configController.relayCount++;
    }

    // One entry per RelayFunctionType (1..RELAY_FUNCTION_COUNT), indexed function-1; absent entirely (server never sends functionControl, or a function's own entry is missing) parses to CONTROL_MODE_THRESHOLD (the default-initialized struct), same as today's ordinary rule fold.
    JsonArray functionControl = deviceConfigController["functionControl"];
    for (JsonObject fc : functionControl)
    {
        int function = fc["relayFunction"] | 0;
        int idx = function - 1;
        if (idx < 0 || idx >= RELAY_FUNCTION_COUNT)
        {
            continue;
        }
        FunctionControlConfig &control = currentConfig.configController.functionControl[idx];
        control.controlMode = fc["controlMode"] | CONTROL_MODE_THRESHOLD;
        control.pidSetpointMetric = fc["pidSetpointMetric"] | 0;
        control.pidSetpoint = fc["pidSetpoint"] | 0.0;
        control.pidKp = fc["pidKp"] | 0.0;
        control.pidKi = fc["pidKi"] | 0.0;
        control.pidKd = fc["pidKd"] | 0.0;
        control.pidSampleIntervalSeconds = fc["pidSampleIntervalSeconds"] | 0.0;
    }
  }
}
