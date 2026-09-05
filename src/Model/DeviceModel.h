#ifndef DATASTRUCTURE_H
#define DATASTRUCTURE_H
#include "Arduino.h"

struct DeviceDefaults {
    String servicePoint = "api.agrumy.com";
    int serviceType = 1; // 0 http, 1 https, 2 mqtt
};


struct DeviceRegistration
{
  char userLogin[128];
  char devicePin[8];
  char servicePoint[256];
  // Optional operator-facing label sent to the server as displayName - never the AP SSID, which stays Agrumy_<mac> regardless.
  char displayName[64] = "";
  bool initialize=false;
};

// Saved to its own mqttConfig.json, not deviceRegistration.json, so broker credentials never ride along in registerDevice()'s server-bound payload; brokerHost empty means MQTT publishing is disabled.
struct MqttConfig
{
  char brokerHost[128] = "";
  char brokerPort[6] = "1883"; // port 8883 selects TLS (WiFiClientSecure + the same CA bundle as HTTPS)
  char username[64] = "";
  char password[64] = "";
};

struct EventLog
{
    bool error = false;
    int errorCode = 0;
    String errorData ="";
};

enum CommandActionType
{
    COMMAND_REBOOT = 1,
    COMMAND_FORCE_OTA = 2,
    COMMAND_FORCE_CONFIG_SYNC = 3,
    COMMAND_SCAN_FOR_DEVICES = 4,
    COMMAND_PROVISION_DEVICE = 5,
};

struct PendingCommand
{
    bool present = false;
    int idDeviceCommand = 0;
    int actionType = 0;
    String expiresAt = "";
    // Only ProvisionDevice sets this - JSON, api.Models.DiscoveryProvisionPayload on the server side.
    String payload = "";
};

struct ConfigPin // default values, cannot be changed during the setup phase
{
#if defined(AGRUMY_KIT_KC868_A6)
    // Not physically verified against real KC868-A6 hardware (confirm before first field deploy) - relays sit behind a PCF8574 I2C expander, so RELAY_PINS[0..5] here are PCF8574 bit indices (0-5), not GPIO numbers.
    int POWER_RAIL_PRIMARY=0; //UNDEFINED
    int POWER_RAIL_SECONDARY=0; //UNDEFINED

    int STATUS_POWER=0; //UNDEFINED
    int STATUS_SENSOR=0; //UNDEFINED
    int STATUS_ERROR=0; //UNDEFINED

    int DHT=32;
    int TEMPSOIL=33;
    int MOIST=0; //UNDEFINED
    int WaterTank=0; //UNDEFINED
    int DEPTH_RX=0; //UNDEFINED
    int DEPTH_TX=0; //UNDEFINED
    int PH=0; //UNDEFINED
    int BATTERY_ADC=0; //UNDEFINED

    int RELAY_PINS[8] = {0, 1, 2, 3, 4, 5, 0, 0}; // slots 7-8 UNDEFINED
#elif defined(AGRUMY_KIT_ESP32S3_RELAY6CH)
    // Not physically verified against real hardware (confirm before first field deploy) - direct GPIO, same digitalWrite/pinMode model as esp32dev/esp32s3usbotg, no I2C expander on this kit.
    int POWER_RAIL_PRIMARY=0; //UNDEFINED
    int POWER_RAIL_SECONDARY=0; //UNDEFINED

    int STATUS_POWER=0; //UNDEFINED
    int STATUS_SENSOR=0; //UNDEFINED
    int STATUS_ERROR=0; //UNDEFINED

    int DHT=0; //UNDEFINED
    int TEMPSOIL=0; //UNDEFINED
    int MOIST=0; //UNDEFINED
    int WaterTank=0; //UNDEFINED
    int DEPTH_RX=0; //UNDEFINED
    int DEPTH_TX=0; //UNDEFINED
    int PH=0; //UNDEFINED
    int BATTERY_ADC=0; //UNDEFINED

    int RELAY_PINS[8] = {1, 2, 41, 42, 45, 46, 0, 0}; // slots 7-8 UNDEFINED
#else
    int POWER_RAIL_PRIMARY=2;
    int POWER_RAIL_SECONDARY=15;

    int STATUS_POWER=4;
    int STATUS_SENSOR=5;
    int STATUS_ERROR=16; // RX2 pin

    int DHT=19;
    int TEMPSOIL=5;
    int MOIST=34;
    int WaterTank=35;
    int DEPTH_RX=13;
    int DEPTH_TX=12;
    int PH=33;
    int BATTERY_ADC=36;

    int RELAY_PINS[8] = {14, 27, 26, 25, 0, 0, 0, 0}; // slots 5-8 UNDEFINED
#endif

    // SDA/SCL only meaningful when RELAY_I2C_ADDRESS is nonzero (else direct GPIO, no I2C expander).
#if defined(AGRUMY_KIT_KC868_A6)
    int RELAY_I2C_ADDRESS=0x24;
    int RELAY_I2C_SDA=4;
    int RELAY_I2C_SCL=15;
#else
    int RELAY_I2C_ADDRESS=0;
    int RELAY_I2C_SDA=0;
    int RELAY_I2C_SCL=0;
#endif
};

struct ModuleEnabled
{
    bool moisture; // analog
    bool waterLevel;   // Analog water level
    bool dht;          // temperature, moisture
    bool bmp180;       // temperature, pressure
    bool bmp280;       // temperature, pressure
    bool bme280;       // temperature, pressure, moisture
    bool ds18b20;      // temperature
    bool ccs811;       // CO2, TVOC
    bool bh1750;       // Light intensity
    bool liquidPH;     // PH sensor
    bool AJSR04M;      // Digital water level
    bool battery;      // Battery Sensor, voltage
    bool camera;

    bool rtc;   // Clock module
    bool relay;
};

struct SensorType
{
    String battery;
    String temperature; // DHT, BMP180, BME280,
    String humidity;    // DHT, BME280,
    String barometer;  // BMP180, BME280
    String waterTank;   // AJSR04M, waterLevel
    
};


struct ConfigSensor
{
    int sensorBattery;
    // Actual resistors wired (ohms), not an abstract preset ratio; only meaningful when sensorBattery selects VoltageDivider (2001), ignored by MAX17048 (1009).
    double batteryDividerR1 = 100000.0;
    double batteryDividerR2 = 100000.0;
    int sensorTemp;
    int sensorTempSoil;
    int sensorHumid;
    int sensorMoist;
    int sensorLight;
    int sensorCo2;
    int sensorTvoc;
    int sensorBarometer;
    int sensorPH;
    int sensorRainLevel;
    int sensorWaterLevel;
    int sensorWind;
};

enum ConditionType
{
    CONDITION_THRESHOLD = 1,
    CONDITION_INTERVAL = 2,
    CONDITION_SCHEDULE = 3,
};

// Roadmap #212. Operator joining a condition to the PREVIOUS one in its Rule's conditions[] - unused (0) at index 0.
enum LogicalOperator
{
    LOGICAL_AND = 1,
    LOGICAL_OR = 2,
};

// Flat, tagged-union style: only the fields matching `type` are meaningful (not a real C++ union).
struct Condition
{
    int type = 0;           // ConditionType raw value
    int operatorBefore = 0; // LogicalOperator raw value, 0/unused for this Rule's first condition

    // Metric/direction are implicit in the owning Rule's targetFunction (Ventilation=humidity/above, Light=light/below, Heating=temperature/below, WaterPump=waterLevel/below).
    double threshold = 0;
    double hysteresis = 0;

    // On for intervalLength seconds out of every interval-second period, grid-aligned to epoch.
    int interval = 0;
    int intervalLength = 0;

    // daysOfWeek: 7-bit mask, bit0=Sunday..bit6=Saturday. start/duration: seconds since local midnight; a window may not cross midnight.
    int daysOfWeek = 0;
    int start = 0;
    int duration = 0;
};

// Roadmap #212. Beyond this cap, ConfigParser silently drops extra conditions within one rule (server enforces a matching cap) - independent of MAX_RULES below.
static const int MAX_CONDITIONS_PER_RULE = 8;

// One automation rule: targetFunction plus a flat, left-to-right AND/OR fold of conditionCount
// conditions (roadmap #212) - "(A op B) op C", never nested/parenthesized (ActuatorController::evaluateRule).
// Several Rules for the same targetFunction still OR together on top of this, unchanged since before #212.
struct Rule
{
    int targetFunction = 0; // RelayFunctionType raw value: 1=Ventilation,2=Light,3=Heating,4=WaterPump
    Condition conditions[MAX_CONDITIONS_PER_RULE];
    int conditionCount = 0;
};

// Beyond this cap, ConfigParser silently drops extra rules (ArduinoJson has no dynamic growth on-device).
static const int MAX_RULES = 32;

// Ceiling on physically-wired relay slots a board can report - bump this (and each board's ConfigPin.RELAY_PINS array) for a bigger relay bank, no other schema/wire-format change needed (roadmap #309).
static const int MAX_RELAY_SLOTS = 8;

// One physically-wired relay position (Slot, 1-based, indexes ConfigPin.RELAY_PINS[Slot-1]) and which RelayFunctionType it's assigned to - only slots the server actually assigned arrive over the wire, an unlisted slot is unassigned.
struct RelaySlot
{
    int slot = 0;
    int relayFunction = 0;
};

struct ConfigController
{
    // Empty (ruleCount 0) when the device has no assigned zone, meaning every relay function stays off.
    Rule rules[MAX_RULES];
    int ruleCount = 0;

    // WaterPump-only device-side hard safety limits, independent of whichever rule turned the pump on. 0 disables either one.
    int waterPumpMaxRunSeconds = 0;
    int waterPumpCooldownSeconds = 0;

    // Server-computed rain veto for WaterPump; the device just applies this flag.
    bool skipWaterPumpForRain = false;

    int relayEnabled;
    RelaySlot relays[MAX_RELAY_SLOTS];
    int relayCount = 0;
};


struct DeviceConfig
{
    String WifiSSID;
    String WifiPassword;
    String userLogin; // Device registration
    String devicePin; // Device registration

    int configVersion;

    // Separate from configVersion on purpose: a queued command must not force a full config re-apply, and vice versa.
    int commandVersion = 0;
    PendingCommand pendingCommand;

    int tenantID;
    int deviceID;
    int deviceUnitID;
    int deviceUnitZoneID;
    int deviceTypeServiceID;

    String apiId;
    String apiKey;
    String servicePoint;
    String servicePublicKey;

    int sleepSeconds;
    bool sleepDeep;

    // Current UTC offset in seconds (positive east of UTC), refreshed on every config sync; lets scheduleRelayFunction() compute local day/time with plain integer math, no on-device IANA/DST database.
    int utcOffsetSeconds = 0;

    bool deviceSensorEnabled;
    bool deviceControllerEnabled;
    bool batteryEnabled;
    bool enabled;
    bool debug;           // 0 serial print disabled, 1 serial print enabled
    bool reboot;
    bool reset;
    bool emergencyStop; // tenant-wide fail-closed switch (roadmap #230) - forces every relay off ahead of any rule, independent of configController.relayEnabled

    bool firmwareUpdate; // 0 no update, 1 update available
    String firmwareVersion; // newest published version for this device type, "" if none
    String firmwareUrl;     // .bin download URL, paired with firmwareVersion
    // Expected SHA-256 (lowercase hex) of the .bin at firmwareUrl; "" means OtaController skips verification instead of failing closed.
    String firmwareSha256;

    ConfigSensor configSensor;
    ConfigController configController;
    ConfigPin configPin;
    
    EventLog eventlog;
};



struct SensorData
{
    int tenantID;
    int deviceID;
    int deviceUnitID;
    int deviceUnitZoneID;

    // NAN means "no reading this cycle" (sensor absent/disabled/failed) - never a real 0, and never the String+atof heap churn a 24/7 device would otherwise accumulate (roadmap #326).
    double battery = NAN;
    double temperature = NAN;
    double temperatureSoil = NAN;
    double humidity = NAN;
    double moisture = NAN;
    double light = NAN;
    double co2 = NAN;
    double tvoc = NAN;
    double barometer = NAN;
    double liquidPH = NAN;
    double rainLevel = NAN;
    double waterLevel = NAN;
    double wind = NAN;
    String dateCreated;
    EventLog eventlog;
};

// HTTP/MQTT payload
struct ServiceData
{
    String payload="";
    EventLog eventlog;
    // Seconds from a 429 response's Retry-After header; -1 when the response wasn't a 429 or carried no such header.
    int retryAfterSeconds = -1;
};


struct ServiceHeader{
    String apiId="";
    String apiKey="";
};


struct ServiceRequest
{
    String serviceType="";
    bool isHttps=false; // set alongside serviceType by DeviceController::serviceType(), so requestPost need not re-parse the prefix
    String servicePoint="";
    String endpoint="";
    ServiceHeader header;

    String url() const { return serviceType + servicePoint + endpoint; }
};




struct ServiceEndpoint
{
    String apiRegister = "/api/Device/Register";
    String apiConfig = "/api/Device/Config";
    String apiAuthenticate = "/api/Device/Authenticate";
    String apiEvent = "/api/Device/Event";
    String apiCommandAck = "/api/Device/Command/Ack";
    String apiDiscoveryReport = "/api/Discovery/Report";

    String apiSensorDataPost="/api/SensorData";
    String apiSensorDataGet="";

};

// Single canonical instance, defined once in main.cpp: every translation unit reads/writes the same object, so a config update is visible everywhere without re-copying.
extern DeviceConfig deviceConfig;
extern ServiceEndpoint serviceEndpoint;
extern ServiceRequest serviceRequest;



#endif