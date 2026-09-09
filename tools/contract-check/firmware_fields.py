# =============================================================================
#  MANUALLY MAINTAINED - update this file whenever you change what the firmware
#  puts on / takes off the wire, i.e. whenever you touch:
#     - src/Model/DeviceModel.h  (ServiceRequest, DeviceConfig, ServiceEndpoint,
#       ConfigSensor, ConfigController, SensorData)
#     - src/Controller/DeviceController.cpp   registerDevice(), loadConfig()
#     - src/Controller/ServiceController.cpp  apiAuthenticate(), apiConfig()
#     - src/Controller/SensorController.cpp   buildSensorDataPayload()
#
#  check_contract.py validates the key sets below against the JSON Schemas in
#  contracts/device-api/ (a copy of AgrumyService/contracts/device-api/ - see that
#  folder's README for the source commit).
#
#  This is the stand-in until real firmware unit tests exist (roadmap #19); it
#  does NOT parse the C++, it trusts the lists here to be kept in sync.
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
        "keys": ["macAddress", "email", "devicePin", "serviceType"],
    },

    "authenticate.request.schema.json": {
        "mode": "empty_body",
        "src": "ServiceController.cpp :: apiAuthenticate()  ->  empty JsonDocument; apiId/apiKey are HTTP headers",
        "keys": [],
    },

    "config.request.schema.json": {
        "mode": "sends_exact",
        "src": "ServiceController.cpp :: apiConfig()  ->  payload[...] (PascalCase; ConfigVersion "
               "sent as string, the roadmap #7 heartbeat diagnostics as numbers/string)",
        "keys": ["ConfigVersion", "Uptime", "Rssi", "FreeHeap", "FirmwareVersion", "Board", "Kit"],  # Board: roadmap #94, Kit: roadmap #149
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
        "src": "ServiceController.cpp :: pushControllerData()  ->  entry[...] (percent only for a positional function, so not in required)",
        "keys": ["relayFunction", "isOn", "dateCreated"],
    },

    "authenticate.response.schema.json": {
        "mode": "reads_subset",
        "src": "ServiceController.cpp :: apiAuthenticate()  ->  payload[\"apiAuth\"]",
        "keys": ["apiAuth"],
    },

    "config.response.schema.json": {
        "mode": "reads_subset",
        "src": "DeviceController.cpp :: loadConfig()  ->  config[...]  (firmware ignores idDeviceConfig* fields)",
        "keys": [
            "servicePoint", "servicePublicKey", "apiId", "apiKey",
            "configVersion", "tenantID", "deviceID", "deviceFarmUnitID", "deviceFarmUnitZoneID",
            "deviceTypeServiceID", "sleepSeconds", "sleepDeep", "utcOffsetSeconds",  # roadmap #39
            "deviceSensorEnabled", "deviceControllerEnabled", "batteryEnabled", "enabled",
            "debug", "reboot", "reset",
            "firmwareUpdate", "firmwareVersion", "firmwareUrl", "firmwareSha256",  # roadmap #3 (OTA) / #131
            "pendingCommand",  # roadmap #34
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
                    "sensorBattery", "sensorTemp", "sensorTempSoil", "sensorHumid", "sensorMoist",
                    "sensorLight", "sensorCo2", "sensorTvoc", "sensorBarometer", "sensorPH",
                    "sensorRainLevel", "sensorWaterLevel", "sensorWind",
                ],
            },
            "deviceConfigController": {
                "def": "deviceConfigController",
                # Threshold/interval/schedule per-condition-type fields were replaced by a single
                # "rules" array (ConfigParser.cpp reads deviceConfigController["rules"], each
                # entry's relayFunction/conditionType/conditionConfig - see ActuatorController for
                # the per-conditionType conditionConfig shape, not re-validated at this flat level).
                # idDeviceConfigController excluded, same as idDeviceConfigSensor/idDeviceConfig*
                # elsewhere in this file - firmware ignores it.
                "keys": [
                    "rules",
                    "waterPumpMaxRunSeconds", "waterPumpCooldownSeconds",
                    "relayEnabled",
                    "relays",  # roadmap #309: each entry's slot/relayFunction - see ConfigParser.cpp, not re-validated at this flat level (same as rules)
                    "skipWaterPumpForRain",
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
