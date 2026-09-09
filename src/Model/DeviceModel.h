#ifndef DATASTRUCTURE_H
#define DATASTRUCTURE_H
#include "Arduino.h"
#include "ServiceTypeIds.h"
#include "FixedString.h"

struct DeviceDefaults {
    const char* servicePoint = "api.agrumy.com";
    int serviceType = ServiceTypeIds::Https;
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
    char errorData[128] = "";
};

// Coarse "where in the loop was the device when it died" marker - RTC_DATA_ATTR-backed (see DeviceController.cpp), survives the panic-reboot a WDT timeout triggers, read back by consumeCrashSummary() to tell a genuine crash from a WDT stall and say roughly where the stall happened.
enum LoopPhase
{
    PHASE_BOOT = 0,
    PHASE_WIFI_INIT,
    PHASE_SENSOR_SETUP,
    PHASE_WIFI_RECONNECT,
    PHASE_API_CONFIG,
    PHASE_SENSOR_READ,
    PHASE_SLEEP_IDLE,
};

enum CommandActionType
{
    COMMAND_REBOOT = 1,
    COMMAND_FORCE_OTA = 2,
    COMMAND_FORCE_CONFIG_SYNC = 3,
    COMMAND_SCAN_FOR_DEVICES = 4,
    COMMAND_PROVISION_DEVICE = 5,
    COMMAND_UPDATE_WIFI = 6,
    COMMAND_DETECT_SENSORS = 7,
};

struct PendingCommand
{
    bool present = false;
    int idDeviceCommand = 0;
    int actionType = 0;
    char expiresAt[32] = ""; // ISO 8601 UTC
    // ProvisionDevice/UpdateWifiCredentials set this - JSON, api.Models.DiscoveryProvisionPayload/WifiUpdatePayload on the server side.
    char payload[512] = "";
};

struct ConfigPin // default values, cannot be changed during the setup phase
{
#if defined(AGRUMY_KIT_KC868_A6)
    // Not physically verified against real KC868-A6 hardware (confirm before first field deploy) - relays sit behind a PCF8574 I2C expander, so RELAY_PINS[0..5] here are PCF8574 bit indices (0-5), not GPIO numbers.
    int POWER_RAIL_PRIMARY=-1; //UNDEFINED
    int POWER_RAIL_SECONDARY=-1; //UNDEFINED

    int STATUS_POWER=-1; //UNDEFINED
    int STATUS_SENSOR=-1; //UNDEFINED
    int STATUS_ERROR=-1; //UNDEFINED

    int DHT=32;
    int TEMPSOIL=33;
    int MOIST=-1; //UNDEFINED
    int WaterTank=-1; //UNDEFINED
    int DEPTH_RX=-1; //UNDEFINED
    int DEPTH_TX=-1; //UNDEFINED
    int PH=-1; //UNDEFINED
    int BATTERY_ADC=-1; //UNDEFINED

    // Not physically wired on any board yet, same "-1 UNDEFINED until a real install assigns it" convention as the pins above.
    int MAX31855_CS=-1; //UNDEFINED
    int MAX31856_CS=-1; //UNDEFINED
    int MAX31865_CS=-1; //UNDEFINED
    int HX711_DOUT=-1; //UNDEFINED
    int HX711_SCK=-1; //UNDEFINED

    int RELAY_PINS[8] = {0, 1, 2, 3, 4, 5, -1, -1}; // slots 7-8 UNDEFINED - -1, not 0, since bit 0 is a real, wired PCF8574 bit here

    // Roadmap #231 - all UNASSIGNED (-1). KC868-A6's relays sit entirely behind the PCF8574 I2C expander above, which has no PWM register at all - dimming here would need genuinely separate direct-GPIO pins wired to external MOSFET/SSR hardware, and which GPIOs are actually free after the relay I2C bus + onboard SX1278 LoRa socket + RS485/I2C peripherals is NOT yet verified against a real schematic (see agrumy-roadmap-todo.md #231's own explicit caveat) - do not assign a pin here without checking real hardware first.
    int PWM_PINS[4] = {-1, -1, -1, -1};
#elif defined(AGRUMY_KIT_ESP32S3_RELAY6CH)
    // Not physically verified against real hardware (confirm before first field deploy) - direct GPIO, same digitalWrite/pinMode model as esp32dev/esp32s3usbotg, no I2C expander on this kit.
    int POWER_RAIL_PRIMARY=-1; //UNDEFINED
    int POWER_RAIL_SECONDARY=-1; //UNDEFINED

    int STATUS_POWER=-1; //UNDEFINED
    int STATUS_SENSOR=-1; //UNDEFINED
    int STATUS_ERROR=-1; //UNDEFINED

    int DHT=-1; //UNDEFINED
    int TEMPSOIL=-1; //UNDEFINED
    int MOIST=-1; //UNDEFINED
    int WaterTank=-1; //UNDEFINED
    int DEPTH_RX=-1; //UNDEFINED
    int DEPTH_TX=-1; //UNDEFINED
    int PH=-1; //UNDEFINED
    int BATTERY_ADC=-1; //UNDEFINED

    // Not physically wired on any board yet, same "-1 UNDEFINED until a real install assigns it" convention as the pins above.
    int MAX31855_CS=-1; //UNDEFINED
    int MAX31856_CS=-1; //UNDEFINED
    int MAX31865_CS=-1; //UNDEFINED
    int HX711_DOUT=-1; //UNDEFINED
    int HX711_SCK=-1; //UNDEFINED

    int RELAY_PINS[8] = {1, 2, 41, 42, 45, 46, -1, -1}; // slots 7-8 UNDEFINED

    // Roadmap #231 - UNASSIGNED (-1) until a real schematic confirms which GPIOs are actually free after the relay bank above (see agrumy-roadmap-todo.md #231's own caveat - do not guess a pin here).
    int PWM_PINS[4] = {-1, -1, -1, -1};
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

    // Not physically wired on any board yet, same "-1 UNDEFINED until a real install assigns it" convention as elsewhere in this struct.
    int MAX31855_CS=-1; //UNDEFINED
    int MAX31856_CS=-1; //UNDEFINED
    int MAX31865_CS=-1; //UNDEFINED
    int HX711_DOUT=-1; //UNDEFINED
    int HX711_SCK=-1; //UNDEFINED

    int RELAY_PINS[8] = {14, 27, 26, 25, -1, -1, -1, -1}; // slots 5-8 UNDEFINED

    // Roadmap #231 - UNASSIGNED (-1) until a real schematic confirms which GPIOs are actually free after the relay bank above (see agrumy-roadmap-todo.md #231's own caveat - do not guess a pin here).
    int PWM_PINS[4] = {-1, -1, -1, -1};
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

    // Roadmap #133 - this kit's dedicated OLED slot, shares the RELAY_I2C_SDA/SCL bus above (different address, same wires). 0 means "no local display" - Controller/DisplayController.cpp isn't even compiled in for other kits (see platformio.ini build_src_filter).
#if defined(AGRUMY_KIT_KC868_A6)
    int DISPLAY_I2C_ADDRESS=0x3C;
#else
    int DISPLAY_I2C_ADDRESS=0;
#endif

    // Direct-GPIO kits only (RELAY_I2C_ADDRESS==0) - a PCF8574-expander kit like KC868-A6 already drives its relays active-low by the expander's own wiring convention (see RelayIO.cpp), unrelated to this flag. false on every kit below since none has been field-verified as active-low yet; this only adds the lever, it changes no kit's current behavior.
    bool RELAY_ACTIVE_LOW=false;
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
    int sensorEc;     // electrical conductivity (ADS1115Ec)
    int sensorWeight; // load cell (HX711)
    double weightCalibrationFactor = 1.0; // HX711 set_scale() divisor - raw counts per real-world unit, calibrated per install
    long weightTareOffset = 0; // HX711 set_offset() raw reading with the scale empty, applied every boot instead of tare()ing whatever's currently on the scale
    // No universal analog-EC-probe formula exists (same reason Wind/pH/rainLevel stayed unimplemented for so long) - identity default (1.0/0.0) reports raw millivolts until a real install calibrates against known-EC reference solutions, same convention as batteryDividerR1/R2 above.
    double ecCalibrationSlope = 1.0;
    double ecCalibrationOffset = 0.0;
};

#include "../Logic/ConditionTree.h"


// Bumped only when a config wire-format change is big enough that an old firmware silently misreading/ignoring a field would matter; ConfigParser::parse() logs (not rejects) when the server's schemaVersion is newer than this, so a stale-firmware-after-server-upgrade mismatch is visible in the serial log during OTA rollback triage instead of just "some field is mysteriously wrong".
static const int CONFIG_SCHEMA_VERSION = 1;

// Beyond this cap, ConfigParser silently drops extra rules (ArduinoJson has no dynamic growth on-device).
static const int MAX_RULES = 32;

// Ceiling on physically-wired relay slots a board can report - bump this (and each board's ConfigPin.RELAY_PINS array) for a bigger relay bank, no other schema/wire-format change needed.
static const int MAX_RELAY_SLOTS = 8;

// Roadmap #231 - separate cap from MAX_RELAY_SLOTS since PWM outputs use their own dedicated ConfigPin.PWM_PINS array, never shared with the relay bank.
static const int MAX_PWM_SLOTS = 4;

// Floor for sleepSeconds, applied at parse time regardless of server-side validation - same value ActuatorController::computeNextWakeSeconds already floors sleep-schedule boundaries to, so both stay in agreement.
static const int MIN_SLEEP_SECONDS = 30;

// One physically-wired relay position (Slot, 1-based, indexes ConfigPin.RELAY_PINS[Slot-1]) and which RelayFunctionType it's assigned to - only slots the server actually assigned arrive over the wire, an unlisted slot is unassigned.
struct RelaySlot
{
    int slot = 0;
    int relayFunction = 0;
};

// Roadmap #231 - one dedicated PWM output position (Slot, 1-based, indexes ConfigPin.PWM_PINS[Slot-1]), mirroring a relay function's on/off decision as a proportional signal rather than driving its own independent state - see ActuatorController::initController's remarks. Only takes effect when relayFunction also has a RelaySlot assigned (no relay assignment means no on/off decision to mirror), and only when the target board's PWM_PINS[Slot-1] is actually assigned (>=0) - every board ships with all four UNASSIGNED until a real schematic confirms free GPIOs.
struct PwmSlot
{
    int slot = 0;
    int relayFunction = 0;
    int intensityPercent = 100;
};

// Roadmap #219.
enum ManualOverrideMode
{
    MANUAL_OVERRIDE_DURATION = 1,
    MANUAL_OVERRIDE_TARGET = 2,
};

// Mirrors api.Models.SensorMetric's numeric values exactly (only the three Target mode allows) - a raw wire int, not the full server-side enum.
enum ManualOverrideTargetMetric
{
    TARGET_METRIC_TEMPERATURE = 1,
    TARGET_METRIC_HUMIDITY = 3,
    TARGET_METRIC_MOISTURE = 5,
};

// One admin-triggered manual actuation - the server is authoritative for start/stop/expiry, this is a snapshot delivered fresh on every config poll (present only while the server still considers it active). expiresAtEpoch is the hard safety cap regardless of mode, computed server-side from the zone's own MaxRunSeconds for this function - see RelayLogic::evaluateManualOverride.
struct ManualOverride
{
    int relayFunction = 0; // RelayFunctionType raw value
    int mode = 0;           // ManualOverrideMode raw value
    time_t expiresAtEpoch = 0;
    // Target mode only - metric selection mirrors api.Models.SensorMetric's Temperature/Humidity/Moisture (roadmap #219's allowed subset for Target mode).
    int targetMetric = 0;
    double targetThreshold = 0;
    double targetHysteresis = 0;
};

// At most one per manually-triggerable function (Ventilation/Heating/WaterPump today) - headroom for one more without a wire-format change.
static const int MAX_MANUAL_OVERRIDES = 4;

struct ConfigController
{
    // Empty (ruleCount 0) when the device has no assigned zone, meaning every relay function stays off.
    Rule rules[MAX_RULES];
    int ruleCount = 0;

    // WaterPump-only device-side hard safety limits, independent of whichever rule turned the pump on. 0 disables either one.
    int waterPumpMaxRunSeconds = 0;
    int waterPumpCooldownSeconds = 0;

    // Dry-run protection: blocks WaterPump (Interval/Schedule/Manual included, not just Threshold) whenever the tank's
    // computed fill percent is below this. <=0, or rawEmpty==rawFull (uncalibrated - a Water Valve zone with no tank
    // sensor), disables it entirely - same api.Utils.TankCalculator percent math the server already uses for the
    // low-tank refill alert, just evaluated on-device instead of server-side.
    double waterPumpMinLevel = 0;
    int waterLevelRawEmpty = 0;
    int waterLevelRawFull = 0;

    // Server-computed rain veto for WaterPump; the device just applies this flag.
    bool skipWaterPumpForRain = false;

    // What a Heating rule does once its temperature reading has been stale (NaN) for longer than MAX_HEATING_SENSOR_STALE_SECONDS - matches api.Shared.Models.HeatingFailSafePolicyType exactly (0=Hold, 1=Off, 2=ScheduleOnly); an unrecognized value also falls back to Hold (see ActuatorController::evaluateRule).
    int heatingFailSafePolicy = 0;

    // Roadmap #219 - present only while the server still considers the command active, see ManualOverride's own remarks.
    ManualOverride manualOverrides[MAX_MANUAL_OVERRIDES];
    int manualOverrideCount = 0;

    int relayEnabled;
    RelaySlot relays[MAX_RELAY_SLOTS];
    int relayCount = 0;

    // Roadmap #231 - see PwmSlot's own remarks.
    PwmSlot pwmSlots[MAX_PWM_SLOTS];
    int pwmSlotCount = 0;
};


struct DeviceConfig
{
    int configVersion;

    PendingCommand pendingCommand;

    int tenantID;
    int deviceID;
    int deviceFarmUnitID;
    int deviceFarmUnitZoneID;
    int deviceTypeServiceID;

    char apiId[64] = "";
    char apiKey[64] = "";
    char servicePoint[128] = "";
    char servicePublicKey[2048] = ""; // PEM CA certificate for a self-hosted server, "" means the built-in bundle

    int sleepSeconds;
    bool sleepDeep;
    // Roadmap #383 - only meaningful under AGRUMY_LORA_GATEWAY_CAPABLE; main.cpp's loop() gates LoRaGatewayRelayController::poll() on this.
    bool loRaGatewayEnabled = false;

    // Current UTC offset in seconds (positive east of UTC), refreshed on every config sync; lets scheduleRelayFunction() compute local day/time with plain integer math, no on-device IANA/DST database.
    int utcOffsetSeconds = 0;

    // Server wall-clock at response time (roadmap #381) - DeviceController::loadConfig feeds this to applyServerEpochFallback(), which only takes effect while NTP has never synced.
    long serverUtcEpoch = 0;

    bool deviceSensorEnabled;
    bool deviceControllerEnabled;
    bool batteryEnabled;
    bool enabled;
    bool debug;           // 0 serial print disabled, 1 serial print enabled
    bool reset;
    bool emergencyStop; // tenant-wide fail-closed switch (roadmap #230) - forces every relay off ahead of any rule, independent of configController.relayEnabled

    bool firmwareUpdate; // 0 no update, 1 update available
    char firmwareVersion[32] = ""; // newest published version for this device type, "" if none
    char firmwareUrl[256] = "";    // .bin download URL, paired with firmwareVersion
    // Expected SHA-256 (lowercase hex) of the .bin at firmwareUrl; "" means OtaController skips verification instead of failing closed.
    char firmwareSha256[65] = "";

    ConfigSensor configSensor;
    ConfigController configController;
    ConfigPin configPin;

    EventLog eventlog;

    // How many rules ConfigParser::parse() rejected wholesale this cycle (unrecognized/over-cap condition) - checked once by whoever applies newConfig, then its job is done.
    int rulesRejectedCount = 0;
};

// Roughly 27KB on esp32dev (every string a fixed char array, no heap) - this struct must never be passed/returned by value or declared as a stack local (that's what overflowed loopTask before ServiceController::apiConfig's heap-allocated configCandidate). Ceiling is rounded up so a future field addition that blows the budget fails the build instead of a device.
static_assert(sizeof(DeviceConfig) <= 32768, "DeviceConfig grew past the 32KB ceiling - check for a stack-unsafe growth before raising this");



struct SensorData
{
    int tenantID;
    int deviceID;
    int deviceFarmUnitID;
    int deviceFarmUnitZoneID;

    // NAN means "no reading this cycle" (sensor absent/disabled/failed) - never a real 0, and never the String+atof heap churn a 24/7 device would otherwise accumulate.
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
    double ec = NAN;
    double weight = NAN;
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
    // Server clock from the response's Date header, any successful HTTP response; 0 when absent/unparseable, same sentinel as DeviceConfig::serverUtcEpoch.
    long dateHeaderEpoch = 0;
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
    const char* apiRegister = "/api/Device/Register";
    const char* apiConfig = "/api/Device/Config";
    const char* apiAuthenticate = "/api/Device/Authenticate";
    const char* apiEvent = "/api/Device/Event";
    const char* apiCommandAck = "/api/Device/Command/Ack";
    const char* apiHardResetPending = "/api/Device/HardResetPending";
    const char* apiDiscoveryReport = "/api/Discovery/Report";

    const char* apiSensorDataPost = "/api/SensorData";
    const char* apiSensorDataGet = "";

    const char* apiControllerDataPost = "/api/ControllerData";

};

// Single canonical instance, defined once in main.cpp: every translation unit reads/writes the same object, so a config update is visible everywhere without re-copying.
extern DeviceConfig deviceConfig;
extern ServiceEndpoint serviceEndpoint;
extern ServiceRequest serviceRequest;



#endif