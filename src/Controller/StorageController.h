#ifndef StorageController_H
#define StorageController_H
#include "Arduino.h"

class StorageController
{
public:
    // Atomic write: writes to a .tmp copy, only replacing the real file once that write is confirmed complete, so a power loss mid-write leaves the old file untouched; returns false (data NOT persisted) if the write or final rename failed.
    static bool saveFile(String data, String filename);
    static String loadFile(String filename);

    // Bounded verification (not a fixed delay) around saveFile()/loadFile() call sites.
    static bool waitForFileCommitted(String filename, unsigned long timeoutMs = 1000);
    static String loadFileRetry(String filename, int maxAttempts = 5, unsigned long retryDelayMs = 100);

    // Backs up the current config.json to config.json.bak (only if it exists and parses) before atomically replacing it. Returns whether the NEW config.json save succeeded (backup failure alone does not count against it).
    static bool saveConfigFile(String newConfigJson);

    // One ready-to-POST SensorData JSON array per file under /buffer/, zero-padded so lexicographic order == chronological; false means the payload was deliberately discarded (partition >= 70% full), via an atomic tmp+rename write.
    static bool bufferSensorDataToDisk(String payloadJson);

    // Lowest-numbered (oldest) queued file's name (e.g. "buffer/00001.json"), or "" when the queue is empty.
    static String oldestBufferedSensorFile();

    static void removeBufferedFile(String filename);

    // NVS (Preferences), a genuinely separate flash partition from LittleFS - a LittleFS-specific corruption never takes out both copies of the one file the device needs to know who it is.
    static bool saveRegistrationBackup(String data);
    // "" means no backup was ever written, or the read itself failed - same "empty means absent" convention as loadFile().
    static String loadRegistrationBackup();
};

#endif
