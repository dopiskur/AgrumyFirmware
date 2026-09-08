#include "Arduino.h"
#include "LittleFS.h"
#include "FS.h"
#include <ArduinoJson.h>
#include <Preferences.h>

#include "StorageController.h"

static const char *REGISTRATION_BACKUP_NAMESPACE = "agrumy";
static const char *REGISTRATION_BACKUP_KEY = "devReg";
static const char *WIFI_BACKUP_NAMESPACE = "agrumywifi";
static const char *WIFI_BACKUP_SSID_KEY = "ssid";
static const char *WIFI_BACKUP_PASS_KEY = "pass";

// Return value tells the caller whether data actually reached disk, not just whether it was logged.
bool StorageController::saveFile(String data, String filename)
{
  String path = "/" + filename;
  String tmpPath = path + ".tmp";

  // format() only after repeated failures (real flash degradation), not one transient I/O hiccup - same pattern as MAX_CONSECUTIVE_AUTH_FAILURES.
  static int consecutiveOpenFailures = 0;
  const int MAX_CONSECUTIVE_OPEN_FAILURES = 3;
  File file = LittleFS.open(tmpPath, "w");
  if (!file)
  {
    consecutiveOpenFailures++;
    Serial.printf("[Device] saveFile: failed to open %s for write (%d/%d consecutive)\n",
                   tmpPath.c_str(), consecutiveOpenFailures, MAX_CONSECUTIVE_OPEN_FAILURES);
    if (consecutiveOpenFailures >= MAX_CONSECUTIVE_OPEN_FAILURES)
    {
      Serial.println("[Device] Too many consecutive open failures, formatting filesystem...");
      LittleFS.format();
      consecutiveOpenFailures = 0;
    }
    return false;
  }
  consecutiveOpenFailures = 0;

  Serial.print("Saving file: ");
  Serial.println(filename);
  size_t written = file.print(data);
  file.close();

  // Only replace the real file once the .tmp write is confirmed complete; a power loss before this point leaves the old file untouched. Deliberately NOT preceded by a separate remove(path), which would reopen the half-written-file window this function exists to close.
  if (written != (size_t)data.length())
  {
    Serial.println("[Device] saveFile: incomplete write (" + String((unsigned)written) + "/" + String(data.length()) + " bytes) to " + tmpPath + " - leaving " + path + " untouched");
    LittleFS.remove(tmpPath);
    return false;
  }

  if (!LittleFS.rename(tmpPath, path))
  {
    Serial.println("[Device] saveFile: rename " + tmpPath + " -> " + path + " failed");
    return false;
  }

  return true;
};

// Returns whether the NEW config actually got saved - a failed backup does not block the real save (nothing to roll back to yet is not fatal), but a failed primary save is.
bool StorageController::saveConfigFile(String newConfigJson)
{
  String currentConfig = loadFile("config.json");
  if (!currentConfig.isEmpty())
  {
    JsonDocument parseCheck;
    if (deserializeJson(parseCheck, currentConfig) == DeserializationError::Ok)
    {
      if (saveFile(currentConfig, "config.json.bak"))
      {
        Serial.println("[Device] Backed up current config.json to config.json.bak");
      }
      else
      {
        Serial.println("[Device] Failed to back up current config.json - continuing anyway");
      }
    }
    else
    {
      Serial.println("[Device] Current config.json failed to parse - not backing it up as a rollback target");
    }
  }

  return saveFile(newConfigJson, "config.json");
}

String StorageController::loadFile(String filename)
{
  String path = "/" + filename;
  File file = LittleFS.open(path, "r");

  if (!file || file.isDirectory())
  {
    Serial.println("[Device] loadFile: cannot open " + path);
    return String(); // empty => caller treats the file as absent
  }

  Serial.print("Reading file: ");
  Serial.println(filename);

  // Refuse outright rather than silently truncating, so an oversized file reads as "too large", not "corrupt config".
  const size_t MAX_FILE_SIZE = 16384;
  size_t fileSize = file.size();
  if (fileSize > MAX_FILE_SIZE)
  {
    Serial.printf("[Device] loadFile: %s is %u bytes, exceeds the %u byte cap - refusing to load\n",
                   path.c_str(), (unsigned)fileSize, (unsigned)MAX_FILE_SIZE);
    file.close();
    return String();
  }

  // Reads exactly fileSize bytes, not available()-driven - a corrupt LittleFS size field could otherwise spin this loop forever.
  String data;
  data.reserve(fileSize + 1);
  while (data.length() < fileSize)
  {
    int c = file.read();
    if (c < 0)
    {
      break;
    }
    data += (char)c;
  }
  file.close();
  return data;
};

// saveFile()'s LittleFS.rename() completes synchronously, so this bounded poll normally returns on the very first check and only spends real time if that assumption is ever wrong.
bool StorageController::waitForFileCommitted(String filename, unsigned long timeoutMs)
{
  String path = "/" + filename;
  unsigned long start = millis();
  while (!LittleFS.exists(path))
  {
    if (millis() - start >= timeoutMs)
    {
      Serial.println("[Device] waitForFileCommitted: " + path + " still not visible after " + String(timeoutMs) + "ms");
      return false;
    }
    delay(50);
  }
  return true;
}

// Re-reads only when the previous attempt came back empty. A file that legitimately doesn't exist yet still reads empty on every attempt and returns after maxAttempts.
String StorageController::loadFileRetry(String filename, int maxAttempts, unsigned long retryDelayMs)
{
  String data = loadFile(filename);
  for (int attempt = 1; data.isEmpty() && attempt < maxAttempts; attempt++)
  {
    delay(retryDelayMs);
    data = loadFile(filename);
  }
  return data;
}

// The 70% cap is checked BEFORE every write - a write that just pushed usage over the line is caught by the next spill re-running this same check, no separate post-write state needed.
bool StorageController::bufferSensorDataToDisk(String payloadJson)
{
  size_t total = LittleFS.totalBytes();
  size_t used = LittleFS.usedBytes();
  if (total == 0 || used * 100 >= total * 70)
  {
    Serial.printf("[Device] Sensor buffer DISCARDED: LittleFS %u/%u bytes (>= 70%% full) - deliberate data loss by design\n", (unsigned)used, (unsigned)total);
    return false;
  }

  // Lazy one-time init per boot: continue numbering after the highest survivor from before the reboot, so chronological order holds across power cycles.
  static int nextIndex = -1;
  if (nextIndex < 0)
  {
    LittleFS.mkdir("/buffer"); // no-op if it already exists
    nextIndex = 1;
    File dir = LittleFS.open("/buffer");
    File entry;
    while (dir && (entry = dir.openNextFile()))
    {
      int n = String(entry.name()).toInt(); // "00042.json" -> 42; non-numeric -> 0, harmless
      entry.close();
      if (n >= nextIndex)
      {
        nextIndex = n + 1;
      }
    }
  }

  char name[24];
  snprintf(name, sizeof(name), "buffer/%05d.json", nextIndex);
  nextIndex++;
  // nextIndex is still incremented above even on failure, so a later successful spill doesn't reuse this file name.
  if (!saveFile(payloadJson, name))
  {
    Serial.printf("[Device] Sensor buffer spill to /%s FAILED - readings not persisted\n", name);
    return false;
  }

  Serial.printf("[Device] Sensor buffer spilled to /%s - LittleFS now %u/%u bytes\n", name, (unsigned)LittleFS.usedBytes(), (unsigned)total);
  return true;
}

String StorageController::oldestBufferedSensorFile()
{
  // /buffer only ever gets created by spillSensorDataToBuffer() on a failed send - open()'ing it
  // before that ever happened logs a noisy vfs_api error on every device that's never failed a send.
  if (!LittleFS.exists("/buffer"))
  {
    return String();
  }

  File dir = LittleFS.open("/buffer");
  if (!dir || !dir.isDirectory())
  {
    return String();
  }

  String best;
  File entry;
  while ((entry = dir.openNextFile()))
  {
    String name = entry.name();
    entry.close();
    // Defensive: strips a possible "/buffer/" prefix from name() so "buffer/" + name can't double up.
    if (name.startsWith("/buffer/"))
    {
      name = name.substring(8);
    }
    if (!name.endsWith(".json")) // skips orphaned .tmp files from an interrupted atomic write
    {
      continue;
    }
    if (best.isEmpty() || name.compareTo(best) < 0)
    {
      best = name;
    }
  }
  return best.isEmpty() ? String() : "buffer/" + best;
}

// Same shape as bufferSensorDataToDisk()/oldestBufferedSensorFile(), separate /relaybuffer directory (roadmap #396(7)) - a relayed LoRa uplink is a different payload/endpoint than this device's own SensorData, mixing them into /buffer would misroute relayed rows through the sensor-data flush path.
bool StorageController::bufferRelayUplinkToDisk(String payloadJson)
{
  size_t total = LittleFS.totalBytes();
  size_t used = LittleFS.usedBytes();
  if (total == 0 || used * 100 >= total * 70)
  {
    Serial.printf("[Device] Relay buffer DISCARDED: LittleFS %u/%u bytes (>= 70%% full) - deliberate data loss by design\n", (unsigned)used, (unsigned)total);
    return false;
  }

  static int nextIndex = -1;
  if (nextIndex < 0)
  {
    LittleFS.mkdir("/relaybuffer");
    nextIndex = 1;
    File dir = LittleFS.open("/relaybuffer");
    File entry;
    while (dir && (entry = dir.openNextFile()))
    {
      int n = String(entry.name()).toInt();
      entry.close();
      if (n >= nextIndex)
      {
        nextIndex = n + 1;
      }
    }
  }

  char name[28];
  snprintf(name, sizeof(name), "relaybuffer/%05d.json", nextIndex);
  nextIndex++;
  if (!saveFile(payloadJson, name))
  {
    Serial.printf("[Device] Relay buffer spill to /%s FAILED - uplink not persisted\n", name);
    return false;
  }

  Serial.printf("[Device] Relay buffer spilled to /%s - LittleFS now %u/%u bytes\n", name, (unsigned)LittleFS.usedBytes(), (unsigned)total);
  return true;
}

String StorageController::oldestBufferedRelayFile()
{
  if (!LittleFS.exists("/relaybuffer"))
  {
    return String();
  }

  File dir = LittleFS.open("/relaybuffer");
  if (!dir || !dir.isDirectory())
  {
    return String();
  }

  String best;
  File entry;
  while ((entry = dir.openNextFile()))
  {
    String name = entry.name();
    entry.close();
    if (name.startsWith("/relaybuffer/"))
    {
      name = name.substring(13);
    }
    if (!name.endsWith(".json"))
    {
      continue;
    }
    if (best.isEmpty() || name.compareTo(best) < 0)
    {
      best = name;
    }
  }
  return best.isEmpty() ? String() : "relaybuffer/" + best;
}

// Same shape as bufferRelayUplinkToDisk() above, own /lorabuffer directory and .dat extension since the content is a binary blob, not JSON.
bool StorageController::bufferLoRaUplinkToDisk(String payload)
{
  size_t total = LittleFS.totalBytes();
  size_t used = LittleFS.usedBytes();
  if (total == 0 || used * 100 >= total * 70)
  {
    Serial.printf("[Device] LoRa uplink buffer DISCARDED: LittleFS %u/%u bytes (>= 70%% full) - deliberate data loss by design\n", (unsigned)used, (unsigned)total);
    return false;
  }

  static int nextIndex = -1;
  if (nextIndex < 0)
  {
    LittleFS.mkdir("/lorabuffer");
    nextIndex = 1;
    File dir = LittleFS.open("/lorabuffer");
    File entry;
    while (dir && (entry = dir.openNextFile()))
    {
      int n = String(entry.name()).toInt();
      entry.close();
      if (n >= nextIndex)
      {
        nextIndex = n + 1;
      }
    }
  }

  char name[28];
  snprintf(name, sizeof(name), "lorabuffer/%05d.dat", nextIndex);
  nextIndex++;
  if (!saveFile(payload, name))
  {
    Serial.printf("[Device] LoRa uplink buffer spill to /%s FAILED - frames not persisted\n", name);
    return false;
  }

  Serial.printf("[Device] LoRa uplink buffer spilled to /%s - LittleFS now %u/%u bytes\n", name, (unsigned)LittleFS.usedBytes(), (unsigned)total);
  return true;
}

String StorageController::oldestBufferedLoRaUplinkFile()
{
  if (!LittleFS.exists("/lorabuffer"))
  {
    return String();
  }

  File dir = LittleFS.open("/lorabuffer");
  if (!dir || !dir.isDirectory())
  {
    return String();
  }

  String best;
  File entry;
  while ((entry = dir.openNextFile()))
  {
    String name = entry.name();
    entry.close();
    if (name.startsWith("/lorabuffer/"))
    {
      name = name.substring(12);
    }
    if (!name.endsWith(".dat"))
    {
      continue;
    }
    if (best.isEmpty() || name.compareTo(best) < 0)
    {
      best = name;
    }
  }
  return best.isEmpty() ? String() : "lorabuffer/" + best;
}

void StorageController::removeBufferedFile(String filename)
{
  LittleFS.remove("/" + filename);
}

bool StorageController::saveRegistrationBackup(String data)
{
  Preferences prefs;
  if (!prefs.begin(REGISTRATION_BACKUP_NAMESPACE, false))
  {
    Serial.println("[Device] saveRegistrationBackup: NVS open (read-write) failed");
    return false;
  }
  size_t written = prefs.putString(REGISTRATION_BACKUP_KEY, data);
  prefs.end();
  if (written != data.length())
  {
    Serial.println("[Device] saveRegistrationBackup: incomplete NVS write");
    return false;
  }
  return true;
}

String StorageController::loadRegistrationBackup()
{
  Preferences prefs;
  if (!prefs.begin(REGISTRATION_BACKUP_NAMESPACE, true))
  {
    return String(); // namespace never created yet - no backup exists
  }
  String data = prefs.getString(REGISTRATION_BACKUP_KEY, "");
  prefs.end();
  return data;
}

// Roadmap #396(8) - WiFi.SSID()/WiFi.psk() report the CURRENTLY CONNECTED STA credentials, not the last-known-good ones; empty whenever the device happens to not be connected at the moment a rollback needs them. This NVS copy is the actual source of truth for "what network was this device last successfully on", written only from a state DeviceController::initializeWifi() already knows is verified-good.
bool StorageController::saveWifiCredentialsBackup(String ssid, String password)
{
  Preferences prefs;
  if (!prefs.begin(WIFI_BACKUP_NAMESPACE, false))
  {
    Serial.println("[Device] saveWifiCredentialsBackup: NVS open (read-write) failed");
    return false;
  }
  bool ok = prefs.putString(WIFI_BACKUP_SSID_KEY, ssid) == (size_t)ssid.length()
      && prefs.putString(WIFI_BACKUP_PASS_KEY, password) == (size_t)password.length();
  prefs.end();
  return ok;
}

void StorageController::loadWifiCredentialsBackup(String &ssid, String &password)
{
  Preferences prefs;
  if (!prefs.begin(WIFI_BACKUP_NAMESPACE, true))
  {
    ssid = "";
    password = "";
    return; // namespace never created yet - no backup exists
  }
  ssid = prefs.getString(WIFI_BACKUP_SSID_KEY, "");
  password = prefs.getString(WIFI_BACKUP_PASS_KEY, "");
  prefs.end();
}
