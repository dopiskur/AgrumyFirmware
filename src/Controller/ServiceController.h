#ifndef ServiceController_H
#define ServiceController_H
#include "Arduino.h"
#include <ArduinoJson.h>

#include "../Model/DeviceModel.h"
#include "ActuatorController.h"

// Forward declarations
class DeviceController;
class SensorController;

class ServiceController
{
public:
    void checkConfig(String payload); // For Debug only
    ServiceData requestPost(JsonDocument jsonBuffer, ServiceRequest serviceEndpoint);
    ServiceData requestGet(ServiceRequest service);

    void errorReport(EventLog eventlog);

    void apiAuthenticate(DeviceConfig deviceConfig, ServiceRequest serviceRequest, DeviceController& device);
    // deviceConfig is a reference so a received config can be hot-applied in place; returns true when that happened (no reboot), and the caller must then re-copy deviceConfig into its per-module value copies.
    bool apiConfig(DeviceConfig& deviceConfig, ServiceRequest serviceRequest, DeviceController& device);
    ServiceData apiSensorData(DeviceConfig deviceConfig, ServiceRequest serviceRequest);

    // Best-effort, never checked or retried. commandId is included only when >= 0 (alongside EventType="CommandExecuted").
    void pushEvent(ServiceRequest service, String eventType, String message, int commandId = -1);

    // Best-effort, never checked or retried - same convention as pushEvent. dateCreated is computed once by the caller (DeviceController::getDateTime()), not re-derived per entry, so every entry in the same push shares one timestamp.
    void pushControllerData(ServiceRequest service, const ControllerDataChange changes[], int count, const String &dateCreated);

    // Acks the pending command, performs its action, then reports the outcome via pushEvent - except Reboot, which never returns. No-op if config.pendingCommand is not present.
    void processPendingCommand(DeviceConfig& config, ServiceRequest serviceRequest, DeviceController& device);

    // WiFi.scanNetworks() locally, POSTs one Discovery/Report per Agrumy_<mac> AP found - the rest of the scan (every neighboring network's real SSID) never leaves the device.
    void scanAndReportDevices(ServiceRequest serviceRequest);

    // Connects as a client to the discovered device's Agrumy_<mac> AP, POSTs {Username, PIN, SSID, password} to its WiFiManager /wifisave, then reconnects to this device's own network. Returns true only if the target's /wifisave answered 200.
    bool provisionDiscoveredDevice(const String& payloadJson);

    // Trials the new SSID/password (WiFi.persistent(false), so a power loss mid-trial leaves the old network still the one saved to flash), confirms a real config-poll against serviceRequest succeeds - not just an AP link - then persists; reverts to the previously-connected network on any failure. Returns true only once persisted.
    bool switchWifiNetwork(const String& payloadJson, ServiceRequest serviceRequest);

    JsonDocument buildJson();

    // First 4 + last 4 characters visible, rest replaced. Public/static so DeviceController's raw config-JSON debug dump can reuse it.
    static String maskSecret(const String &value);

    // >0 right after apiConfig() returned a 429 ("Wait" - see RelayRateLimitedException server-side): main.cpp's loop() sleeps this many seconds instead of the normal cycle before polling again. Always reset to 0 at the top of apiConfig().
    int waitSeconds = 0;

    // Device-local wall-clock (DeviceController::getEpochSeconds()) of the last config poll that got a real HTTP response (200, with or without a changed body) - 0 means never. Roadmap #133's local display "last sync" page reads this; not set on a 429/error response.
    time_t lastConfigSyncEpoch = 0;

private:
    // Queried on a 401 instead of ever self-wiping from a bare failure count - apiId alone (no apiKey/session) so this reaches a device whose apiKey itself is what's broken.
    bool isHardResetPending(ServiceRequest serviceRequest, const String &apiId);
};

// The one ServiceController instance, defined in main.cpp.
extern ServiceController service;

#endif