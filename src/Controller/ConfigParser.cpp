#include "Arduino.h"
#include "ArduinoJson.h"

#include "ConfigParser.h"
#include "ServiceController.h"

namespace
{
    // Roadmap #396(4) - recursively parses one JSON ConditionNode (api.Models.ConditionNode) into
    // rule.nodes[], returning its index there, or -1 to reject the WHOLE rule (an unrecognized type,
    // too many total nodes, or a group with zero/too-many children never stores a partial/broken
    // tree). `node` stays a valid reference across the recursive calls below - rule.nodes is a
    // fixed-size array member, never reallocated, so appending further nodes can't invalidate it.
    int parseConditionNode(JsonObject nodeJson, Rule &rule)
    {
        if (rule.nodeCount >= MAX_NODES_PER_RULE)
        {
            return -1;
        }
        int index = rule.nodeCount;
        rule.nodeCount++;
        ConditionNode &node = rule.nodes[index];
        node.type = nodeJson["type"];
        switch (node.type)
        {
        case NODE_COMPARISON:
            node.metric = nodeJson["metric"];
            node.op = nodeJson["operator"];
            node.value1 = nodeJson["value1"];
            node.value2 = nodeJson["value2"] | 0.0;
            node.hysteresis = nodeJson["hysteresis"] | 0.0;
            return index;
        case NODE_INTERVAL:
            node.interval = nodeJson["interval"];
            node.intervalLength = nodeJson["intervalLength"];
            return index;
        case NODE_SCHEDULE:
            node.daysOfWeek = nodeJson["daysOfWeek"];
            node.start = nodeJson["start"];
            node.duration = nodeJson["duration"];
            return index;
        case NODE_GROUP:
        {
            node.groupOperator = nodeJson["groupOperator"];
            JsonArray children = nodeJson["children"];
            if (children.size() == 0 || (int)children.size() > MAX_CHILDREN_PER_GROUP)
            {
                return -1;
            }
            int childCount = 0;
            for (JsonObject childJson : children)
            {
                int childIndex = parseConditionNode(childJson, rule);
                if (childIndex < 0)
                {
                    return -1;
                }
                node.childIndices[childCount] = childIndex;
                childCount++;
            }
            node.childCount = childCount;
            return index;
        }
        default:
            return -1; // unrecognized node type (e.g. Astronomical/RuleTriggered, which should never reach firmware at all - see DeviceModel.h's NodeType remarks)
        }
    }
}

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
    currentConfig.eventlog.errorData = error.c_str();

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
    currentConfig.eventlog.errorData = "missing apiId/apiKey/servicePoint";
    return;
  }
  // currentConfig is re-parsed in place on every call, so a failure flagged above must not linger into the next call that succeeds.
  currentConfig.eventlog.error = false;

  currentConfig.configVersion = config["configVersion"];

  currentConfig.tenantID = config["tenantID"];
  currentConfig.deviceID = config["deviceID"];
  currentConfig.deviceFarmUnitID = config["deviceFarmUnitID"];
  currentConfig.deviceFarmUnitZoneID = config["deviceFarmUnitZoneID"];
  currentConfig.deviceTypeServiceID = config["deviceTypeServiceID"]; // 0 http, 1 https, 2 mqtt

  currentConfig.apiId = apiId;
  currentConfig.apiKey = apiKey;
  currentConfig.servicePoint = servicePoint;
  currentConfig.servicePublicKey = servicePublicKey;

  // Floored here regardless of server-side validation - a sensor-only device has no controller-side floor to fall back on (ActuatorController::computeNextWakeSeconds only applies to relay-type devices), so 0/negative would otherwise loop with no delay.
  int requestedSleepSeconds = config["sleepSeconds"];
  currentConfig.sleepSeconds = requestedSleepSeconds < MIN_SLEEP_SECONDS ? MIN_SLEEP_SECONDS : requestedSleepSeconds;
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
        candidate.targetFunction = r["relayFunction"];
        candidate.targetPercent = r["targetPercent"] | 0; // only meaningful for a positional function (Screen/Vent), ignored otherwise
        candidate.nodeCount = 0;
        JsonObject rootJson = r["root"];
        int rootIndex = rootJson.isNull() ? -1 : parseConditionNode(rootJson, candidate);
        if (rootIndex < 0)
        {
            // Unrecognized node type, too many total nodes, or a group with too many/zero children -
            // reject the WHOLE rule rather than store a partial/broken tree (same spirit as the old
            // flat model's "a bad condition rejects the whole rule").
            currentConfig.rulesRejectedCount++;
            continue;
        }
        candidate.rootIndex = rootIndex;
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

    // Roadmap #219 - capped at MAX_MANUAL_OVERRIDES, same "ArduinoJson has no dynamic growth on-device" reasoning as rules/relays above.
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

    // Roadmap #231 - capped at MAX_PWM_SLOTS, same "ArduinoJson has no dynamic growth on-device" reasoning as relays above. Absent entirely (server never sends pwmSlots) parses to pwmSlotCount 0, same as an empty array.
    JsonArray pwmSlots = deviceConfigController["pwmSlots"];
    currentConfig.configController.pwmSlotCount = 0;
    for (JsonObject p : pwmSlots)
    {
        if (currentConfig.configController.pwmSlotCount >= MAX_PWM_SLOTS)
        {
            break;
        }
        PwmSlot &pwmSlot = currentConfig.configController.pwmSlots[currentConfig.configController.pwmSlotCount];
        pwmSlot.slot = p["slot"];
        pwmSlot.relayFunction = p["relayFunction"];
        pwmSlot.intensityPercent = p["intensityPercent"] | 100;
        currentConfig.configController.pwmSlotCount++;
    }
  }
}
