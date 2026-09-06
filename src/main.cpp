// Profile B (LoRa, roadmap #220/#225) is a fundamentally different device: no WiFi/HTTP, no
// OTA/buffer/command queue - EU868's tiny payload limit leaves no room for that stack. Kept as one
// file with the WiFi profile below (not a separate main.cpp) so both profiles stay buildable from
// the same source tree, same pattern as the existing AGRUMY_KIT_KC868_A6 board-variant branching.
#ifdef AGRUMY_PROFILE_LORA

#include <esp_task_wdt.h>
#include <esp_sleep.h>
#include "LittleFS.h"
#include "Controller/LoRaController.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0-dev"
#endif

static const uint32_t LORA_WDT_TIMEOUT_SECONDS = 30;

LoRaController loRaController;

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("[LoRa] Initialization started");

    if (!LittleFS.begin(true))
    {
        Serial.println("[LoRa] LittleFS mount/format FAILED");
    }

    if (!loRaController.begin())
    {
        Serial.println("[LoRa] begin() failed - will retry next boot");
    }

    esp_task_wdt_deinit();
    esp_task_wdt_init(LORA_WDT_TIMEOUT_SECONDS, true);
    esp_task_wdt_add(NULL);
}

void loop()
{
    // Mains-powered vs battery is not yet configurable for this profile - assume battery, the more
    // conservative (longer sleep) choice, until #225's real hardware pass adds provisioning for it.
    uint32_t sleepSeconds = loRaController.runCycleAndGetSleepSeconds(true);
    esp_task_wdt_reset();

    Serial.printf("[LoRa] Sleeping %u seconds\n", sleepSeconds);
    // uint64 math: seconds * 1e6 overflows int32 past ~35 minutes, same overflow PowerController::sleep already guards against.
    esp_sleep_enable_timer_wakeup((uint64_t)sleepSeconds * 1000000ULL);
    esp_deep_sleep_start(); // never returns
}

#else

#include <NTPClient.h>
#include <Esp.h>
#include "EEPROM.h"
#include "LittleFS.h"
#include <Wire.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <esp_sleep.h>

#include "Model/DeviceModel.h"

#include "Controller/DeviceController.h"
#include "Controller/SensorController.h"
#include "Controller/ServiceController.h"
#include "Controller/ActuatorController.h"
#include "Controller/MqttController.h"

// Injected by tools/firmware_version.py (git tag / FIRMWARE_VERSION env var); the fallback only covers a build that skipped extra_scripts.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0-dev"
#endif
const char *firmware = FIRMWARE_VERSION;
const String CONFIG_BASE = "deviceRegistration.json";
const String CONFIG_DEFAULTS = "config.json";

// Default 8192-byte loopTask stack overflows under chained TLS handshakes (apiConfig's 401 retry into apiAuthenticate); sdkconfig.h blocks the CONFIG_ARDUINO_LOOP_STACK_SIZE build-flag fix, so SET_LOOP_TASK_STACK_SIZE is used instead. 24576 was enough for that path, but setup()'s own loadConfig/ConfigParser::parse call chain (JsonDocument, DeviceConfig struct copies, nested String concatenations) overflowed even 32768, confirmed via vApplicationStackOverflowHook - 49152 covers it with margin.
SET_LOOP_TASK_STACK_SIZE(49152);

// Reboots if a loop() cycle wedges before completing. Sized to clear ~4 sequential HTTPClient calls per cycle (config sync, re-auth, retry, sensor push) at the default 5s TCP timeout each, with margin. Independent of server-set sleepSeconds - the inter-cycle sleep is fed separately in loop().
static const uint32_t WDT_TIMEOUT_SECONDS = 90;

static JsonDocument jsonData;
static String servicePoint;

// Single canonical instances (see externs in DeviceModel.h and each controller's own header).
DeviceConfig deviceConfig;
ServiceEndpoint serviceEndpoint;

// Non-static (see extern ServiceRequest in DeviceModel.h) - MqttController's persistent-connection
// command callback needs it to hand a received command straight to ServiceController::processPendingCommand.
ServiceRequest serviceRequest;
static ServiceData serviceData;

DeviceController device;
ServiceController service;
SensorController sensor;
ActuatorController controller;



void setup()
{
  Serial.begin(115200);
  Serial.println();
  Serial.println("[Initialization started]");

  // Read (and clear) any core dump before anything else touches flash/WiFi - it has no dependency on either, and the summary is needed by the reboot-outcome reporting block further down.
  String crashSummary = device.consumeCrashSummary();

  // On a deep-sleeping node every cycle re-enters setup() - say so explicitly, otherwise a serial log full of reboots is indistinguishable from a crash loop.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER)
  {
    Serial.println("[Boot] Woke from deep sleep (timer)");
  }

  // format-on-fail: a partition still holding SPIFFS bytes must fail the mount here so the format runs - if it does not, flash the board with a full chip erase.
  if (!LittleFS.begin(true))
  {
    Serial.println("[Main] LittleFS mount/format FAILED");
  }
  Serial.printf("[FS] LittleFS total=%u used=%u bytes\n", LittleFS.totalBytes(), LittleFS.usedBytes());
  EEPROM.begin(512);
  // LittleFS.begin(true) above can trigger a format on the format-on-fail branch - the flash subsystem needs time to fully settle before the next access is reliable (empirically confirmed: file reads intermittently fail without this).
  delay(500);

  String configRegistration = device.loadFileRetry(CONFIG_BASE);
  if (configRegistration.isEmpty())
  {
    Serial.println("[Main] Registration file not found, starting initialization...");
    device.initializeDevice(); // blocks in the Agrumy_<mac> portal, then reboots
  }

  device.initializeWifi();
  // wifiManager.autoConnect() inside initializeWifi() already blocks until connected, but WL_CONNECTED can report before the IP stack/DNS resolver is actually ready for a real HTTP request - this covers that gap, not the connection itself.
  delay(500);

  // 3 config-triggered reboots in a row, each within 60s of its own boot, means the last config update is likely the cause - load the backup instead of repeating the same crash forever.
  bool rollbackToBackup = device.consumeRollbackTrigger();
  // Always consumed regardless of rollbackToBackup; a rollback boot confirms CrashLoopRollback, never ConfigApplied, even though both flags are set together on the reboot that triggered the rollback.
  bool configJustApplied = device.consumeConfigAppliedPending();
  String configDefaults = rollbackToBackup ? device.loadFileRetry("config.json.bak") : device.loadFileRetry(CONFIG_DEFAULTS);
  if (rollbackToBackup)
  {
    if (configDefaults.isEmpty())
    {
      Serial.println("[Main] Rollback requested but config.json.bak is missing/empty - falling back to config.json");
      configDefaults = device.loadFile(CONFIG_DEFAULTS);
    }
    else
    {
      Serial.println("[Main] Repeated rapid reboots after a config update detected - rolling back to config.json.bak");
      device.saveFile(configDefaults, CONFIG_DEFAULTS); // persist so the rollback sticks even if this boot reboots again for any other reason
    }
  }
  if (configDefaults.isEmpty())
  {
    Serial.println("[Main] Config file not found, starting initialization...");
    device.registerDevice(configRegistration);
  }

  deviceConfig = device.loadConfig(configDefaults);

  serviceRequest.serviceType = device.serviceType(deviceConfig.deviceTypeServiceID, serviceRequest.isHttps);
  serviceRequest.servicePoint = deviceConfig.servicePoint;

  sensor.serviceRequest = serviceRequest;

  // Confirm the outcome of the reboot back to the server. A rollback boot always wins over configJustApplied - both flags are set together on the reboot that triggered the rollback, but that boot applies the OLD backup, not the new config. A crash summary only reports when neither happened.
  if (rollbackToBackup || configJustApplied || !crashSummary.isEmpty())
  {
    // apiAuthenticate() takes serviceRequest by value, so its internal apiId/apiKey/endpoint mutations never reach this copy - re-set apiId here before reusing serviceRequest below.
    service.apiAuthenticate(deviceConfig, serviceRequest, device);
    serviceRequest.header.apiId = deviceConfig.apiId;
    if (rollbackToBackup)
    {
      service.pushEvent(serviceRequest, "CrashLoopRollback", "3 consecutive early reboots detected");
    }
    else if (configJustApplied)
    {
      service.pushEvent(serviceRequest, "ConfigApplied", "version=" + String(deviceConfig.configVersion));
    }
    else
    {
      service.pushEvent(serviceRequest, "Crash", crashSummary);
    }
  }

  // Unconditional on purpose: for a non-battery (mains) device, loop()'s batteryEnabled cycling never touches these pins, so this is the ONLY place that powers them. A battery device's loop() takes over duty-cycling from the next iteration.
  device.powerRailPrimary(true);
  device.powerRailSecondary(true);

  sensor.setupSensor();    // early init for more precise measurement
  device.setupController(); // initialize time
  mqtt.begin(device);        // loads mqttConfig.json - no-op if MQTT was never configured

  // Arm the watchdog only now that setup (incl. the blocking WiFi portal/registration path) is done - those legitimately take longer than one loop cycle. esp_task_wdt_init() is a no-op if the WDT (arduino-esp32's own 5s default) is already initialized, so tear it down first.
  esp_task_wdt_deinit();
  esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true); // true: panic-handler reboot on timeout
  esp_task_wdt_add(NULL);                       // watch the Arduino loop task

  Serial.println("[Initialization] Finished: ");
}

void loop()
{
  Serial.println("[Loop]-----> Start <-----[Loop]");
  if (deviceConfig.batteryEnabled)
  {
    device.powerRailPrimary(true);
    device.powerRailSecondary(true);
  }

  // deviceConfig is passed by reference and is the single canonical instance, so a hot-applied config (no reboot) is visible to every module the instant apiConfig() returns.
  service.apiConfig(deviceConfig, serviceRequest, device);
  // >0 only right after a 429 ("Wait") - skip the sensor push too, no point adding another request while the relay/server asked us to back off.
  bool waitingForServer = service.waitSeconds > 0;

  if (deviceConfig.enabled && !waitingForServer) {
    sensor.buildSensorData(deviceConfig);
  }

  if (deviceConfig.batteryEnabled)
  {
    device.powerRailPrimary(false);
    device.powerRailSecondary(false);
  }

  Serial.println("[Loop]-----> END <-----[Loop]");
  Serial.println("");

  // A full cycle finished without wedging - feed the watchdog. Anything that hangs inside apiConfig()/buildSensorData() never reaches this point, so the reboot backstop stays effective against a real stall.
  esp_task_wdt_reset();

  // Roadmap #325: a relay-driving device dynamically sleeps toward its nearest Schedule/Interval
  // boundary instead of a fixed sleepSeconds, which can otherwise skip a short window entirely or
  // run past its end.
  uint32_t cycleSeconds;
  if (waitingForServer)
  {
    cycleSeconds = (uint32_t)service.waitSeconds;
  }
  else if (deviceConfig.deviceControllerEnabled)
  {
    cycleSeconds = (uint32_t)controller.computeNextWakeSeconds(device.getEpochSeconds(), deviceConfig.sleepSeconds);
  }
  else
  {
    cycleSeconds = (uint32_t)deviceConfig.sleepSeconds;
  }

  // A device that drives relays must NOT deep sleep: GPIO outputs drop during deep sleep and ActuatorController's interval state is lost, so it falls through to the powered chunked delay below.
  if (deviceConfig.sleepDeep && cycleSeconds > 0)
  {
    if (!deviceConfig.deviceControllerEnabled)
    {
      waitingForServer ? device.sleep((int)cycleSeconds) : device.sleep(); // never returns
    }
    Serial.println("[Sleep] sleepDeep is set but this device drives relays - staying powered");
  }

  // Bounded idle wait, not work that can hang - keep petting the watchdog through it in steps shorter than the timeout, otherwise a server-set sleepSeconds longer than WDT_TIMEOUT_SECONDS looks like a stall.
  uint32_t sleepRemaining = cycleSeconds * 1000UL;
  const uint32_t sleepStep = WDT_TIMEOUT_SECONDS * 1000UL / 3;
  // No-op unless mqttConfig.json opted into persistentCommandChannel (roadmap #146) - this is the
  // only place in the WiFi profile a device stays powered/idle long enough for it to be worthwhile.
  mqtt.beginPersistentIfEnabled(deviceConfig);
  while (sleepRemaining > 0)
  {
    uint32_t chunk = sleepRemaining < sleepStep ? sleepRemaining : sleepStep;
    delay(chunk);
    mqtt.poll(); // pumps the persistent client's receive loop so a pushed command isn't missed
    esp_task_wdt_reset();
    sleepRemaining -= chunk;
  }
}

#endif // AGRUMY_PROFILE_LORA
