#ifndef DeviceController_H
#define DeviceController_H
#include "Arduino.h"
#include "ArduinoJson.h"

#include "../Model/DeviceModel.h"

// Forward declarations instead of includes
class ServiceController;
class SensorController;

// DeviceController.cpp is a thin facade: file I/O lives in StorageController, power-rail/sleep/reboot/reset in PowerController, OTA download+flash in OtaController, config-field parsing in ConfigParser.
class DeviceController
{

public:
    void setupController();

    String getDateTime();

    // NTP-derived wall-clock seconds, synced at boot and periodically thereafter via maybeResyncTime() - ActuatorController's grid-aligned interval formula needs this instead of millis(), which resets on every reboot (and overflows after ~49.7 days even without one).
    time_t getEpochSeconds();

    // Call every loop() cycle - resyncs NTP once ~24h has passed since the last successful sync (corrects crystal drift), or retries every ~60s if a sync has never succeeded yet (no internet at boot). Never lets a failed attempt clobber the last known-good time.
    void maybeResyncTime();

    // Roadmap #381 - seeds getEpochSeconds() from the server's own clock (DeviceConfig.ServerUtcEpoch) for an install with no internet access, so NTP can never succeed; no-op once real NTP has synced, since that always takes priority.
    void applyServerEpochFallback(time_t serverUtcEpoch);

    // Mosfet activation
    void powerRailPrimary(bool state);
    void powerRailSecondary(bool state);

    String macAddr();

    // LittleFS-backed, atomic write (temp+rename) - a mid-write power loss leaves the file untouched or fully replaced, never half-written; returns false if the write/rename didn't complete.
    bool saveFile(String data, String filename);
    String loadFile(String filename);

    // Bounded verification (not a fixed delay) around saveFile()/loadFile() call sites.
    bool waitForFileCommitted(String filename, unsigned long timeoutMs = 1000);
    String loadFileRetry(String filename, int maxAttempts = 5, unsigned long retryDelayMs = 100);

    // Use this instead of loadFileRetry("deviceRegistration.json") directly - falls back to (and repairs from) an NVS backup copy if the primary LittleFS file is unreadable, instead of treating a locally-corrupted primary as "never registered".
    String loadRegistrationWithFallback();

    // Backs up config.json to config.json.bak (if it exists and parses) before atomically replacing it, pairing with consumeRollbackTrigger() below; returns whether the new config.json save succeeded.
    bool saveConfigFile(String newConfigJson);

    // Call ONLY from ServiceController::apiConfig()'s "new config received, about to reboot" branch - feeds the crash-loop counter consumeRollbackTrigger() reads.
    void notePendingConfigReboot(unsigned long uptimeMs);

    // Call once from setup(); true means 3 config-triggered reboots in a row each happened within 60s of their own boot, so the caller should load config.json.bak instead of config.json.
    bool consumeRollbackTrigger();

    // Set on every config-triggered reboot; call once from setup(). Must be ignored when consumeRollbackTrigger() also returns true - that boot is applying the OLD backup, not the new config this flag was set for.
    bool consumeConfigAppliedPending();

    // Reads and clears any pending core dump the ESP-IDF panic handler wrote on the LAST crash. Call once from setup(). Empty string means no crash since the partition was last cleared.
    String consumeCrashSummary();

    // Records which loop phase main.cpp is currently in, RTC-persisted so a WDT/crash reboot can report where it happened - see consumeCrashSummary().
    void setLastPhase(int phase);

    void initializeDevice(); // sets up the WiFi AP

    // Web-flasher hook - waits up to timeoutMs for one '{"type":"agrumyProvision",...}' JSON line on Serial (see firmware-provisioning.js's post-flash re-open of the same port); on a match writes deviceRegistration.json + NVS backup and persists WiFi credentials via WiFi.begin(), same as initializeDevice()'s captive-portal outcome but without ever opening its own AP. False (no line arrived in time, or it didn't parse/match) leaves everything untouched - the caller falls through to initializeDevice() as before.
    bool tryReadSerialProvisioning(unsigned long timeoutMs = 5000);

    void registerDevice(String configRegistration);
    void initializeWifi();

    // Parses configJson into target in place (never returns/copies a DeviceConfig - that struct is tens of KB and must never sit on the stack); target should already hold whatever values omitted JSON keys should fall back to. Returns false when parsing/validation failed (target.eventlog carries the reason) - caller decides whether to commit target to the live config.
    bool loadConfig(const String& configJson, DeviceConfig& target);

    String serviceType(int deviceServiceTypeID, bool& isHttps); // maps deviceServiceTypeID to http/https/mqtt

    String rtc();
    String lcd();
    String camera();

    void sleep();
    void sleep(int overrideSleepSeconds); // same deviceConfig.sleepDeep gate, but a caller-given duration instead of deviceConfig.sleepSeconds - used for a server-requested "Wait" window
    // Downloads+flashes a .bin, returns true on success (caller reboots). Requires isHttps=true and a real 64-char hex expectedSha256 - see OtaController::update.
    bool firmwareUpdate(String url, bool isHttps, String expectedSha256);
    void reboot();
    void reset();
    void button(); // short press runs query, long press erase device

    // One ready-to-POST SensorData JSON array per file under /buffer/, zero-padded so lexicographic order == chronological; false means the payload was deliberately discarded (partition >= 70% full), otherwise an atomic tmp+rename write never leaves a half-written file.
    bool bufferSensorDataToDisk(String payloadJson);

    // Lowest-numbered (oldest) queued file's name (e.g. "buffer/00001.json"), or "" when the queue is empty.
    String oldestBufferedSensorFile();

    void removeBufferedFile(String filename);

private:
};

// The one DeviceController instance, defined in main.cpp.
extern DeviceController device;

#endif
