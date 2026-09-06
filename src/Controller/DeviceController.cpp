#include "Arduino.h"
#include <WiFi.h>
#include <EEPROM.h>
#include "WiFiManager.h"

#include "DeviceController.h"
#include "ServiceController.h"
#include "ConfigParser.h"
#include "StorageController.h"
#include "PowerController.h"
#include "OtaController.h"

#include <NTPClient.h>
#include <WiFiUdp.h>
#include <esp_core_dump.h>

static DeviceDefaults deviceDefaults;

DeviceRegistration deviceRegistration;
JsonDocument config;

// RTC_DATA_ATTR survives ESP.restart() (RTC slow memory) but not a real power cycle - a hard power loss just loses the streak, which is the safe direction to fail.
RTC_DATA_ATTR static int rtcRapidConfigRebootCount = 0;

// Separate from the crash-loop counter above: unconditional (set on every config-triggered reboot, not just rapid ones), answers "did this boot follow a just-applied config" rather than "is this a suspicious streak".
RTC_DATA_ATTR static bool rtcConfigJustAppliedPending = false;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);

// Wall-clock epoch of the last successful sync (0 until the first one) - compared against the CURRENT epoch, not an uptime/loop counter, so the 24h cadence self-corrects regardless of when in a boot cycle it's checked and survives a reboot cleanly (setupController()'s own boot-time sync already resets this).
static unsigned long lastNtpSyncEpoch = 0;
static const unsigned long NTP_RESYNC_INTERVAL_SECONDS = 24UL * 60 * 60;
// Short, millis()-based throttle only for the "never synced yet" retry path - a 49-day millis() wraparound is irrelevant at this timescale, unlike the epoch-based comparison above.
static unsigned long lastNtpAttemptMs = 0;
static const unsigned long NTP_RETRY_THROTTLE_MS = 60000UL;

// Server-epoch fallback (roadmap #381) - only ever read while timeClient.isTimeSet() is false, see getEpochSeconds().
static time_t serverEpochFallbackBase = 0;
static unsigned long serverEpochFallbackSetAtMs = 0;
static bool serverEpochFallbackActive = false;

time_t DeviceController::getEpochSeconds()
{
  if (!timeClient.isTimeSet() && serverEpochFallbackActive)
  {
    return serverEpochFallbackBase + (millis() - serverEpochFallbackSetAtMs) / 1000;
  }
  return timeClient.getEpochTime();
}

void DeviceController::applyServerEpochFallback(time_t serverUtcEpoch)
{
  if (timeClient.isTimeSet())
  {
    return; // real NTP time already available - a coarser server timestamp must never override it
  }
  serverEpochFallbackBase = serverUtcEpoch;
  serverEpochFallbackSetAtMs = millis();
  serverEpochFallbackActive = true;
}

void DeviceController::maybeResyncTime()
{
  bool needsSync = !timeClient.isTimeSet() || (timeClient.getEpochTime() - lastNtpSyncEpoch) >= NTP_RESYNC_INTERVAL_SECONDS;
  if (!needsSync)
  {
    return;
  }
  if (millis() - lastNtpAttemptMs < NTP_RETRY_THROTTLE_MS)
  {
    return; // still-unsynced retries are throttled so a persistent outage doesn't spam NTP requests every loop cycle
  }
  lastNtpAttemptMs = millis();

  if (timeClient.forceUpdate())
  {
    lastNtpSyncEpoch = timeClient.getEpochTime();
    Serial.println("[Device] NTP resync succeeded");
  }
  else
  {
    Serial.println("[Device] NTP resync failed - keeping last known time");
  }
}

String DeviceController::getDateTime()
{

  time_t epochTime = getEpochSeconds();
  struct tm *ptm = gmtime((time_t *)&epochTime);
  int currentYear = ptm->tm_year + 1900;
  int currentMonth = ptm->tm_mon + 1;
  int monthDay = ptm->tm_mday;
  String currentDate = String(currentYear) + "-" + String(currentMonth) + "-" + String(monthDay) + " " + String(timeClient.getFormattedTime());
  Serial.print("[Device] Current datetime: ");
  Serial.println(currentDate);

  return currentDate;
}

void DeviceController::setupController()
{

  timeClient.begin();
  timeClient.update();
  if (timeClient.isTimeSet())
  {
    lastNtpSyncEpoch = timeClient.getEpochTime();
  }
}

bool DeviceController::saveFile(String data, String filename)
{
  return StorageController::saveFile(data, filename);
};

// See StorageController.h for the config-integrity rationale.
bool DeviceController::saveConfigFile(String newConfigJson)
{
  return StorageController::saveConfigFile(newConfigJson);
}

void DeviceController::notePendingConfigReboot(unsigned long uptimeMs)
{
  if (uptimeMs < 60000UL)
  {
    rtcRapidConfigRebootCount++;
    Serial.printf("[Device] Rapid reboot after config update (%lu ms uptime) - streak now %d/3\n", uptimeMs, rtcRapidConfigRebootCount);
  }
  else
  {
    rtcRapidConfigRebootCount = 0; // ran fine for a while first - this config isn't the problem
  }
  rtcConfigJustAppliedPending = true; // unconditional - every config-triggered reboot, not just rapid ones
}

bool DeviceController::consumeRollbackTrigger()
{
  bool trigger = rtcRapidConfigRebootCount >= 3;
  if (trigger)
  {
    rtcRapidConfigRebootCount = 0; // fresh streak after acting on it
  }
  return trigger;
}

bool DeviceController::consumeConfigAppliedPending()
{
  bool pending = rtcConfigJustAppliedPending;
  rtcConfigJustAppliedPending = false;
  return pending;
}

// exc_bt_info.bt holds up to 16 return addresses, but symbolicating ANY of them needs firmware.elf regardless of how many are included - more than a handful adds length without adding usable signal.
static const uint32_t MaxCrashBacktraceAddresses = 8;

String DeviceController::consumeCrashSummary()
{
  if (esp_core_dump_image_check() != ESP_OK)
  {
    return ""; // no dump since the partition was last cleared - the common case, every normal boot
  }

  // Heap-allocated, not a ~250+ byte stack frame that would otherwise sit in setup()'s frame for the rest of the function on every boot.
  esp_core_dump_summary_t *summary = (esp_core_dump_summary_t *)malloc(sizeof(esp_core_dump_summary_t));
  String result = "";
  if (summary != nullptr && esp_core_dump_get_summary(summary) == ESP_OK)
  {
    String backtrace = "";
    uint32_t depth = summary->exc_bt_info.depth;
    if (depth > MaxCrashBacktraceAddresses)
    {
      depth = MaxCrashBacktraceAddresses;
    }
    for (uint32_t i = 0; i < depth; i++)
    {
      if (i > 0)
      {
        backtrace += ",";
      }
      backtrace += "0x" + String(summary->exc_bt_info.bt[i], HEX);
    }

    result = "task=" + String(summary->exc_task) +
             " pc=0x" + String(summary->exc_pc, HEX) +
             " cause=" + String(summary->ex_info.exc_cause) +
             " vaddr=0x" + String(summary->ex_info.exc_vaddr, HEX) +
             (summary->exc_bt_info.corrupted ? " bt(corrupted)=" : " bt=") + backtrace;
    Serial.println("[Device] Pending crash dump found: " + result);
  }
  else
  {
    Serial.println("[Device] Core dump present but its summary could not be read");
  }
  free(summary);

  // Erase regardless of whether the summary above got formatted, so a corrupt/unreadable dump never wedges every future boot into re-attempting the same failed read.
  esp_core_dump_image_erase();

  return result;
}

String DeviceController::loadFile(String filename)
{
  return StorageController::loadFile(filename);
};

bool DeviceController::waitForFileCommitted(String filename, unsigned long timeoutMs)
{
  return StorageController::waitForFileCommitted(filename, timeoutMs);
}

String DeviceController::loadFileRetry(String filename, int maxAttempts, unsigned long retryDelayMs)
{
  return StorageController::loadFileRetry(filename, maxAttempts, retryDelayMs);
}

// Primary (LittleFS) first: if that's unreadable, fall back to the NVS backup rather than treating the device as never-registered - the primary file may be only locally corrupted, so a successful fallback also repairs it.
String DeviceController::loadRegistrationWithFallback()
{
  String primary = loadFileRetry("deviceRegistration.json");
  if (!primary.isEmpty())
  {
    return primary;
  }

  Serial.println("[Device] deviceRegistration.json unreadable - trying NVS backup");
  String backup = StorageController::loadRegistrationBackup();
  if (backup.isEmpty())
  {
    Serial.println("[Device] No NVS backup either - device genuinely unregistered or both copies lost");
    return String();
  }

  Serial.println("[Device] NVS backup found - repairing deviceRegistration.json from it");
  saveFile(backup, "deviceRegistration.json");
  return backup;
}

void DeviceController::initializeWifi()
{

  WiFi.mode(WIFI_STA);
  WiFiManager wifiManager;
  // A router still rebooting (post-outage) at device boot must not block setup() forever - 120s times out the portal so the device falls through to loop() on cached config and retries WiFi in the background instead of freezing releys at their power-on state indefinitely.
  wifiManager.setConfigPortalTimeout(120);
  wifiManager.autoConnect();
}

void DeviceController::initializeDevice()
{
  Serial.println("[Device]: initializeDevice");
  WiFi.mode(WIFI_STA);

  WiFi.mode(WIFI_STA);
  WiFiManager wifiManager;
  wifiManager.setCaptivePortalEnable(false);

  WiFiManagerParameter userLogin("login", "User Login", deviceRegistration.userLogin, 128); // id_user can be sent trough wifisave GET
  WiFiManagerParameter userPin("devicePin", "User PIN", deviceRegistration.devicePin, 8);
  WiFiManagerParameter servicePoint("servicePoint", "Service Point (default:api.agrumy.com)", deviceRegistration.servicePoint, 256);
  WiFiManagerParameter deviceDisplayName("displayName", "Device Name (optional)", deviceRegistration.displayName, 64);

  MqttConfig mqttRegistration;
  WiFiManagerParameter mqttHost("mqttHost", "MQTT Broker (optional, blank=disabled)", mqttRegistration.brokerHost, 128);
  WiFiManagerParameter mqttPort("mqttPort", "MQTT Port (1883, 8883=TLS)", mqttRegistration.brokerPort, 6);
  WiFiManagerParameter mqttUser("mqttUser", "MQTT Username (optional)", mqttRegistration.username, 64);
  WiFiManagerParameter mqttPass("mqttPass", "MQTT Password (optional)", mqttRegistration.password, 64);

  wifiManager.addParameter(&userLogin);
  wifiManager.addParameter(&userPin);
  wifiManager.addParameter(&servicePoint);
  wifiManager.addParameter(&deviceDisplayName);
  wifiManager.addParameter(&mqttHost);
  wifiManager.addParameter(&mqttPort);
  wifiManager.addParameter(&mqttUser);
  wifiManager.addParameter(&mqttPass);

  wifiManager.startConfigPortal(("Agrumy_" + macAddr()).c_str());

  // strncpy(dst, src, sizeof(dst)) doesn't guarantee null termination when src is >= sizeof(dst).
  strncpy(deviceRegistration.userLogin, userLogin.getValue(), sizeof(deviceRegistration.userLogin) - 1);
  deviceRegistration.userLogin[sizeof(deviceRegistration.userLogin) - 1] = 0;
  strncpy(deviceRegistration.devicePin, userPin.getValue(), sizeof(deviceRegistration.devicePin) - 1);
  deviceRegistration.devicePin[sizeof(deviceRegistration.devicePin) - 1] = 0;
  strncpy(deviceRegistration.servicePoint, servicePoint.getValue(), sizeof(deviceRegistration.servicePoint) - 1);
  deviceRegistration.servicePoint[sizeof(deviceRegistration.servicePoint) - 1] = 0;
  strncpy(deviceRegistration.displayName, deviceDisplayName.getValue(), sizeof(deviceRegistration.displayName) - 1);
  deviceRegistration.displayName[sizeof(deviceRegistration.displayName) - 1] = 0;

  deviceRegistration.initialize = true;

  strncpy(mqttRegistration.brokerHost, mqttHost.getValue(), sizeof(mqttRegistration.brokerHost) - 1);
  mqttRegistration.brokerHost[sizeof(mqttRegistration.brokerHost) - 1] = 0;
  strncpy(mqttRegistration.brokerPort, mqttPort.getValue(), sizeof(mqttRegistration.brokerPort) - 1);
  mqttRegistration.brokerPort[sizeof(mqttRegistration.brokerPort) - 1] = 0;
  strncpy(mqttRegistration.username, mqttUser.getValue(), sizeof(mqttRegistration.username) - 1);
  mqttRegistration.username[sizeof(mqttRegistration.username) - 1] = 0;
  strncpy(mqttRegistration.password, mqttPass.getValue(), sizeof(mqttRegistration.password) - 1);
  mqttRegistration.password[sizeof(mqttRegistration.password) - 1] = 0;


  JsonDocument config;
  config["userLogin"] = deviceRegistration.userLogin;
  config["devicePin"] = deviceRegistration.devicePin;
  config["servicePoint"] = deviceRegistration.servicePoint;
  config["displayName"] = deviceRegistration.displayName;
  if (config["servicePoint"] == "")
  {

    config["servicePoint"] = deviceDefaults.servicePoint;
  }

  String data;
  serializeJsonPretty(config, data);
  Serial.println("[Device] Saving registration data " + data);
  saveFile(data, "deviceRegistration.json");
  waitForFileCommitted("deviceRegistration.json");
  // Mirrored to NVS (separate flash partition from LittleFS) so a LittleFS-specific corruption still leaves a readable copy of who this device is.
  StorageController::saveRegistrationBackup(data);

  // Blank brokerHost is saved too, so a re-run of this portal (factory reset) always overwrites any prior MQTT settings.
  JsonDocument mqttConfigJson;
  mqttConfigJson["brokerHost"] = mqttRegistration.brokerHost;
  mqttConfigJson["brokerPort"] = strlen(mqttRegistration.brokerPort) > 0 ? atoi(mqttRegistration.brokerPort) : 1883;
  mqttConfigJson["username"] = mqttRegistration.username;
  mqttConfigJson["password"] = mqttRegistration.password;
  String mqttData;
  serializeJsonPretty(mqttConfigJson, mqttData);
  Serial.println("[Device] Saving MQTT config: broker=" + String(mqttRegistration.brokerHost) + ":" + String(mqttRegistration.brokerPort)); // password never printed, even masked
  saveFile(mqttData, "mqttConfig.json");
  waitForFileCommitted("mqttConfig.json");

  reboot();
};

void DeviceController::registerDevice(String configRegistration)
{
  Serial.println("[Device] Loading registration data " + configRegistration);

  DeserializationError error = deserializeJson(config, configRegistration);

  // This payload may itself have come straight from the primary file (loadRegistrationWithFallback() only substitutes the NVS backup when the primary is EMPTY, not when it opens fine but parses badly) - try the backup here too before treating this as real corruption.
  if (error)
  {
    Serial.println("[Device] RegisterDevice: primary registration data failed to parse - trying NVS backup");
    String backup = StorageController::loadRegistrationBackup();
    error = backup.isEmpty() ? error : deserializeJson(config, backup);
    if (!error)
    {
      Serial.println("[Device] NVS backup parsed - repairing deviceRegistration.json from it");
      saveFile(backup, "deviceRegistration.json");
    }
  }

  // Both copies are gone or unparseable - real corruption, not a transient glitch. format()+restart is the last resort here, not the first reflex.
  if (error)
  {
    Serial.print("[Device] RegisterDevice; deserializeJson() failed on both copies, reseting to defaults ");
    reset();
  }

  JsonDocument payload;
  payload["macAddress"] = macAddr();
  payload["email"] = config["userLogin"];
  payload["devicePin"] = config["devicePin"];
  payload["serviceType"] = deviceDefaults.serviceType;
  // "| \"\"" defaults a registration file saved before this field existed - deserializeJson leaves the key absent rather than erroring.
  payload["displayName"] = config["displayName"] | "";

  String servicePoint = config["servicePoint"];
  ServiceRequest serviceRequest;
  // Bootstrap call carrying email+PIN before any server config exists, so force HTTPS.
  serviceRequest.serviceType = serviceType(1, serviceRequest.isHttps);
  serviceRequest.servicePoint = servicePoint;
  serviceRequest.endpoint = serviceEndpoint.apiRegister;

  String serviceURL = serviceRequest.url();
  Serial.println("[Device] Fetching config from " + serviceURL);
  String exitData;
  serializeJsonPretty(payload, exitData);
  Serial.println("[Device] Payload: " + exitData);
  ServiceData serviceData = service.requestPost(payload, serviceRequest);

  if (serviceData.eventlog.errorCode == 401)
  {
    Serial.println("[Device] Wrong user or pin - wiping and restarting the config portal");
    reset();
  }

  // Only a 2xx with a body is a real config - anything else must NOT be written to config.json (would persist an empty file and boot-loop).
  if ((serviceData.eventlog.errorCode != 200 && serviceData.eventlog.errorCode != 201) ||
      serviceData.payload.isEmpty())
  {
    Serial.println("[Device] Registration failed (code " + String(serviceData.eventlog.errorCode) +
                   "), retrying after reboot");
    delay(10000);
    reboot();
  }

  Serial.println("[Device] config: " + ConfigParser::maskApiKeyInJson(ConfigParser::redactSensitiveFieldsInJson(serviceData.payload)));

  // A truncated-but-non-empty body would pass both checks above (200/201 status, non-empty) and get persisted - saveFile()'s atomic write only protects DISK integrity, not CONTENT validity, so parse-gate before persisting.
  JsonDocument parseCheck;
  if (deserializeJson(parseCheck, serviceData.payload) != DeserializationError::Ok)
  {
    Serial.println("[Device] Registration payload failed to parse - retrying after reboot");
    delay(10000);
    reboot();
  }

  saveFile(serviceData.payload, "config.json");
  waitForFileCommitted("config.json");
  reboot();
}

String DeviceController::macAddr()
{
  byte mac[6];
  WiFi.macAddress(mac);
  char macAddr[18];
  sprintf(macAddr, "%2X%2X%2X%2X%2X%2X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  for (int i = 0; i < 18; i++) // pad spaces with '0'
  {
    if (macAddr[i] == ' ')
      macAddr[i] = '0';
  }
  return macAddr;
}

void DeviceController::powerRailPrimary(bool state)
{
  PowerController::railPrimary(deviceConfig.configPin.POWER_RAIL_PRIMARY, state);
}

void DeviceController::powerRailSecondary(bool state)
{
  PowerController::railSecondary(deviceConfig.configPin.POWER_RAIL_SECONDARY, state);
}

void DeviceController::sleep()
{
  PowerController::sleep(deviceConfig.sleepSeconds, deviceConfig.sleepDeep);
}

void DeviceController::sleep(int overrideSleepSeconds)
{
  PowerController::sleep(overrideSleepSeconds, deviceConfig.sleepDeep);
}

void DeviceController::reboot()
{
  PowerController::reboot();
}

bool DeviceController::firmwareUpdate(String url, bool isHttps, String expectedSha256)
{
  return OtaController::update(url, isHttps, deviceConfig.servicePublicKey, deviceConfig.servicePoint, expectedSha256);
}

void DeviceController::reset()
{
  PowerController::reset();
}

bool DeviceController::bufferSensorDataToDisk(String payloadJson)
{
  return StorageController::bufferSensorDataToDisk(payloadJson);
}

String DeviceController::oldestBufferedSensorFile()
{
  return StorageController::oldestBufferedSensorFile();
}

void DeviceController::removeBufferedFile(String filename)
{
  StorageController::removeBufferedFile(filename);
}

// Mutates the single canonical deviceConfig, not just a local copy.
DeviceConfig DeviceController::loadConfig(String configJson)
{
  ConfigParser::parse(configJson, deviceConfig);
  if (deviceConfig.serverUtcEpoch > 0)
  {
    applyServerEpochFallback((time_t)deviceConfig.serverUtcEpoch);
  }
  return deviceConfig;
};

String DeviceController::serviceType(int deviceServiceTypeID, bool& isHttps)
{
  String serviceType;

  switch (deviceServiceTypeID)
  {
  case 0:
    serviceType = "http://";
    isHttps = false;
    break;
  case 1:
    serviceType = "https://";
    isHttps = true;
    break;
  case 2:
    serviceType = "mqtt://";
    isHttps = false;
    break;
  default:
    serviceType = "https://";
    isHttps = true;
    break;
  }

  return serviceType;
}
