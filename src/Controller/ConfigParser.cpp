#include "Arduino.h"
#include "ArduinoJson.h"

#include "ConfigParser.h"
#include "ServiceController.h"

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

DeviceConfig ConfigParser::parse(const String &configJson, DeviceConfig currentConfig)
{
  Serial.println("[Device] Load config: " + maskApiKeyInJson(configJson));

  JsonDocument config;
  DeserializationError error = deserializeJson(config, configJson);

  if (error)
  {
    Serial.print("[Device] Load Config; deserializeJson() failed: ");
    Serial.println(error.c_str());
    currentConfig.eventlog.error = true;
    currentConfig.eventlog.errorCode = 20; // 10 is reserved for registerDevice's own gate
    currentConfig.eventlog.errorData = error.c_str();

    return currentConfig;
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
    currentConfig.eventlog.errorData = "missing apiId/apiKey/servicePoint";
    return currentConfig;
  }
  // currentConfig is re-parsed in place on every call, so a failure flagged above must not linger into the next call that succeeds.
  currentConfig.eventlog.error = false;

  currentConfig.configVersion = config["configVersion"];

  currentConfig.tenantID = config["tenantID"];
  currentConfig.deviceID = config["deviceID"];
  currentConfig.deviceUnitID = config["deviceUnitID"];
  currentConfig.deviceUnitZoneID = config["deviceUnitZoneID"];
  currentConfig.deviceTypeServiceID = config["deviceTypeServiceID"]; // 0 http, 1 https, 2 mqtt

  currentConfig.apiId = apiId;
  currentConfig.apiKey = apiKey;
  currentConfig.servicePoint = servicePoint;
  currentConfig.servicePublicKey = servicePublicKey;

  currentConfig.sleepSeconds = config["sleepSeconds"];
  currentConfig.sleepDeep = config["sleepDeep"];
  // Keeps the current offset if an older server doesn't send this key - never silently jump to UTC just because the key was missing.
  currentConfig.utcOffsetSeconds = config["utcOffsetSeconds"] | currentConfig.utcOffsetSeconds;
  currentConfig.deviceSensorEnabled = config["deviceSensorEnabled"];
  currentConfig.deviceControllerEnabled = config["deviceControllerEnabled"];
  currentConfig.batteryEnabled = config["batteryEnabled"];
  currentConfig.enabled = config["enabled"];
  currentConfig.debug = config["debug"];
  currentConfig.reboot = config["reboot"];
  currentConfig.reset = config["reset"];
  currentConfig.emergencyStop = config["emergencyStop"] | false;
  currentConfig.firmwareUpdate = config["firmwareUpdate"];
  currentConfig.firmwareVersion = config["firmwareVersion"] | String("");
  currentConfig.firmwareUrl = config["firmwareUrl"] | String("");
  currentConfig.firmwareSha256 = config["firmwareSha256"] | String("");

  currentConfig.commandVersion = config["commandVersion"] | currentConfig.commandVersion;
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
    currentConfig.pendingCommand.expiresAt = pendingCommandJson["expiresAt"] | String("");
    currentConfig.pendingCommand.payload = pendingCommandJson["payload"] | String("");
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
  }

  if (currentConfig.deviceControllerEnabled)
  {
    JsonObject deviceConfigController = config["deviceConfigController"];

    // Capped at MAX_RULES - ArduinoJson has no dynamic growth on-device, extras are silently dropped (server enforces a matching cap). A rule with zero valid conditions after the inner loop is skipped, not stored empty.
    JsonArray rules = deviceConfigController["rules"];
    currentConfig.configController.ruleCount = 0;
    for (JsonObject r : rules)
    {
        if (currentConfig.configController.ruleCount >= MAX_RULES)
        {
            break;
        }
        Rule &rule = currentConfig.configController.rules[currentConfig.configController.ruleCount];
        rule.targetFunction = r["relayFunction"];

        // Roadmap #212: flat AND/OR list, capped at MAX_CONDITIONS_PER_RULE - same silent-truncation/skip-unrecognized-type reasoning as the rules[] loop above, one level deeper.
        rule.conditionCount = 0;
        JsonArray conditions = r["conditions"];
        for (JsonObject c : conditions)
        {
            if (rule.conditionCount >= MAX_CONDITIONS_PER_RULE)
            {
                break;
            }
            Condition &condition = rule.conditions[rule.conditionCount];
            condition.type = c["conditionType"];
            condition.operatorBefore = c["operator"] | 0;
            JsonObject conditionConfig = c["conditionConfig"];
            switch (condition.type)
            {
            case CONDITION_THRESHOLD:
                condition.threshold = conditionConfig["threshold"];
                condition.hysteresis = conditionConfig["hysteresis"];
                break;
            case CONDITION_INTERVAL:
                condition.interval = conditionConfig["interval"];
                condition.intervalLength = conditionConfig["intervalLength"];
                break;
            case CONDITION_SCHEDULE:
                condition.daysOfWeek = conditionConfig["daysOfWeek"];
                condition.start = conditionConfig["start"];
                condition.duration = conditionConfig["duration"];
                break;
            default:
                continue; // unrecognized conditionType - skip, do not advance conditionCount
            }
            rule.conditionCount++;
        }
        if (rule.conditionCount == 0)
        {
            continue; // nothing valid to evaluate - skip storing this rule, do not advance ruleCount
        }
        currentConfig.configController.ruleCount++;
    }

    currentConfig.configController.waterPumpMaxRunSeconds = deviceConfigController["waterPumpMaxRunSeconds"] | currentConfig.configController.waterPumpMaxRunSeconds;
    currentConfig.configController.waterPumpCooldownSeconds = deviceConfigController["waterPumpCooldownSeconds"] | currentConfig.configController.waterPumpCooldownSeconds;

    // Falls back to the current value so an older server build can't accidentally re-arm a pump the last sync deliberately vetoed.
    currentConfig.configController.skipWaterPumpForRain = deviceConfigController["skipWaterPumpForRain"] | currentConfig.configController.skipWaterPumpForRain;

    currentConfig.configController.relayEnabled = deviceConfigController["relayEnabled"];

    // Capped at MAX_RELAY_SLOTS - same "ArduinoJson has no dynamic growth on-device" reasoning as rules above; only slots the server actually assigned ride along, an unlisted slot is unassigned.
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
        currentConfig.configController.relayCount++;
    }
  }

  return currentConfig;
}
