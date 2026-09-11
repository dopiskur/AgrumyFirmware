# =============================================================================
#  MANUALLY MAINTAINED - update this file whenever you change what the firmware
#  puts on / takes off the wire, i.e. whenever you touch:
#     - src/Model/DeviceModel.h  (ServiceRequest, DeviceConfig, ServiceEndpoint,
#       ConfigSensor, ConfigController, SensorData)
#     - src/Controller/DeviceController.cpp   registerDevice(), loadConfig()
#     - src/Controller/ServiceController.cpp  apiAuthenticate(), apiConfig()
#     - src/Controller/SensorController.cpp   buildSensorDataPayload()
#     - src/Controller/ConfigParser.cpp       parseConfig() - the config/register
#       response field reads (config[...], deviceConfigSensor[...],
#       deviceConfigController[...]) now live here, not in DeviceController.cpp
#
#  check_contract.py validates the key sets below against the JSON Schemas in
#  contracts/device-api/ (a copy of AgrumyService/contracts/device-api/ - see that
#  folder's README for the source commit).
#
#  verify_extraction.py cross-checks the "config.response.schema.json"/
#  "register.response.schema.json" entries' keys (including "nested") below against
#  what ConfigParser.cpp actually reads (grepped ["..."] string literals on the
#  config/deviceConfigSensor/deviceConfigController JsonObject variables) - this is
#  what catches a new field ConfigParser.cpp starts reading that never got added
#  here - exactly the kind of drift this file had accumulated silently before.
#  It does NOT replace this file or attempt real C++ parsing - a genuinely new
#  container variable, or a key read through something other than a plain
#  ["..."] subscript, still needs a human to notice and update both files.
# =============================================================================

# modes:
#   sends_exact            firmware is the PRODUCER: keys must EQUAL schema.required
#   sends_exact_array_item firmware produces a JSON array; keys must EQUAL schema.items.required
#   empty_body             firmware sends no body (creds in headers)
#   reads_subset           firmware is the CONSUMER: keys must be a SUBSET of schema.properties
#   same_as                identical to another entry (kept as its own file for symmetry)

CONTRACT = {
    "register.request.schema.json": {
        "mode": "sends_exact",
        "src": "DeviceController.cpp :: registerDevice()  ->  payload[...]",
        "keys": ["macAddress", "email", "devicePin", "serviceType", "displayName"],
    },

    "authenticate.request.schema.json": {
        "mode": "empty_body",
        "src": "ServiceController.cpp :: apiAuthenticate()  ->  empty JsonDocument; apiId/apiKey are HTTP headers",
        "keys": [],
    },

    "config.request.schema.json": {
        "mode": "sends_exact",
        "src": "ServiceController.cpp :: apiConfig()  ->  payload[...] (PascalCase; heartbeat "
               "diagnostics as numbers/string) - only the fields schema.required actually demands; "
               "ConfigSchemaVersion/Latitude/Longitude are also sent but stay optional (older "
               "firmware or no GPS fix omits them), so sends_exact's required-set match doesn't "
               "track them here.",
        "keys": ["Uptime", "Rssi", "FreeHeap", "FirmwareVersion", "Board", "Kit"],
    },

    "sensordata.request.schema.json": {
        "mode": "sends_exact_array_item",
        "src": "SensorController.cpp :: buildSensorDataPayload()  ->  jsonSensorData[...]",
        "keys": [
            "deviceID", "tenantID", "deviceFarmUnitID", "deviceFarmUnitZoneID",
            "temperature", "soilTemperature", "humidity", "battery", "moisture", "light",
            "co2", "tvoc", "barometer", "liquidPH", "rainLevel", "waterLevel", "wind", "dateCreated",
        ],
    },

    "controllerdata.request.schema.json": {
        "mode": "sends_exact_array_item",
        "src": "ServiceController.cpp :: pushControllerData()  ->  entry[...] (percent is the fold's "
               "target percent for every function now, not just a positional one, so it's unconditional)",
        "keys": ["relayFunction", "isOn", "percent", "dateCreated"],
    },

    "authenticate.response.schema.json": {
        "mode": "reads_subset",
        "src": "ServiceController.cpp :: apiAuthenticate()  ->  payload[\"apiAuth\"]",
        "keys": ["apiAuth"],
    },

    "config.response.schema.json": {
        "mode": "reads_subset",
        "src": "DeviceController.cpp :: loadConfig() -> ConfigParser::parse() -> config[...]  (firmware ignores idDeviceConfig* fields)",
        "keys": [
            "servicePoint", "servicePublicKey", "apiId", "apiKey", "schemaVersion",
            "configVersion", "tenantID", "deviceID", "deviceFarmUnitID", "deviceFarmUnitZoneID",
            "deviceTypeServiceID", "sleepSeconds", "sleepDeep", "loRaGatewayEnabled",
            "utcOffsetSeconds", "serverUtcEpoch",
            "deviceSensorEnabled", "deviceControllerEnabled", "batteryEnabled", "enabled",
            "debug", "reset", "emergencyStop",
            "firmwareUpdate", "firmwareVersion", "firmwareUrl", "firmwareSha256",
            "pendingCommand",
            "deviceConfigSensor", "deviceConfigController",
        ],
        "nested": {
            "pendingCommand": {
                "def": "pendingCommand",
                "keys": ["idDeviceCommand", "actionType", "expiresAt"],
            },
            "deviceConfigSensor": {
                "def": "deviceConfigSensor",
                "keys": [
                    "sensorBattery", "batteryDividerR1", "batteryDividerR2",
                    "sensorTemp", "sensorTempSoil", "sensorHumid", "sensorMoist",
                    "sensorLight", "sensorCo2", "sensorTvoc", "sensorBarometer", "sensorPH",
                    "sensorRainLevel", "sensorWaterLevel", "sensorWind",
                    "sensorEc", "ecCalibrationSlope", "ecCalibrationOffset",
                    "sensorWeight", "weightCalibrationFactor", "weightTareOffset",
                ],
            },
            "deviceConfigController": {
                "def": "deviceConfigController",
                # Threshold/interval/schedule per-condition-type fields were replaced by a single
                # "rules" array (ConfigParser.cpp reads deviceConfigController["rules"], each
                # entry's relayFunction/conditionType/conditionConfig - see ActuatorController for
                # the per-conditionType conditionConfig shape, not re-validated at this flat level).
                # relays[]/functionControl[]/manualOverrides[] entries are the same story - each
                # item's own fields (slot/outputKind/pairSlot/.../controlMode/pidKp/.../mode/
                # targetMetric/...) live in ConfigParser.cpp but aren't re-validated at this flat
                # level, only that the container key itself is present.
                # idDeviceConfigController excluded, same as idDeviceConfigSensor/idDeviceConfig*
                # elsewhere in this file - firmware ignores it.
                "keys": [
                    "rules",
                    "waterPumpMaxRunSeconds", "waterPumpCooldownSeconds", "waterPumpMinLevel",
                    "waterLevelRawEmpty", "waterLevelRawFull",
                    "relayEnabled",
                    "relays",
                    "skipWaterPumpForRain",
                    "heatingFailSafePolicy",
                    "manualOverrides",
                    "functionControl",
                ],
            },
        },
    },

    # loadConfig() parses the Register response with the exact same code path as the
    # Config response, so the two schemas - and the expected field set - are identical.
    "register.response.schema.json": {
        "mode": "same_as",
        "same_as": "config.response.schema.json",
        "src": "DeviceController.cpp :: registerDevice() -> saveFile(config.json); parsed later by loadConfig()",
    },
}
