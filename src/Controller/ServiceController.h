#ifndef ServiceController_H
#define ServiceController_H
#include "Arduino.h"
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../Model/DeviceModel.h"
#include "ActuatorController.h"
#include "AgrumyClient.h"

// Forward declarations
class DeviceController;
class SensorController;

// ServiceController is now the application layer (ConfigApplier/
// CommandExecutor: apiConfig, processPendingCommand, pushEvent and friends); every actual network
// round trip is forwarded to AgrumyClient (see AgrumyClient.h), the transport/session layer split out
// of this class. The forwarding methods below (requestPost/requestGet/firmwareUpdate/mqttPublish/
// mqttConnectPersistent/apiAuthenticate/maskSecret/beginNetworkTask/networkTaskHandle) exist so every
// existing external caller (DeviceController, MqttController, ConfigParser, main.cpp) keeps calling
// `service.xxx(...)` unchanged.
class ServiceController
{
public:
    // Forwards to AgrumyClient::beginNetworkTask - call exactly once from setup(), before anything can call requestPost/requestGet/firmwareUpdate.
    static void beginNetworkTask();
    static TaskHandle_t networkTaskHandle();

    void checkConfig(String payload); // For Debug only
    ServiceData requestPost(const JsonDocument& jsonBuffer, ServiceRequest serviceEndpoint);
    ServiceData requestGet(ServiceRequest service);

    // Forwards to AgrumyClient::firmwareUpdate; true only once the image is downloaded and verified (caller reboots).
    bool firmwareUpdate(const OtaParams& params);

    // Forwards to AgrumyClient::mqttPublish/mqttConnectPersistent.
    bool mqttPublish(const String& topic, const String& payload);
    bool mqttConnectPersistent(int tenantID, int deviceID);

    void errorReport(EventLog eventlog);

    // Forwards to AgrumyClient::apiAuthenticate.
    void apiAuthenticate(const DeviceConfig& deviceConfig, ServiceRequest serviceRequest, DeviceController& device);
    // deviceConfig is a reference so a received config can be hot-applied in place; returns true when that happened (no reboot), and the caller must then re-copy deviceConfig into its per-module value copies.
    // gpsLatitude/gpsLongitude: NAN (default) when this board has no GPS fix to report (see main.cpp's AGRUMY_GPS_ENABLED gate) - omitted from the payload rather than sent as 0,0.
    bool apiConfig(DeviceConfig& deviceConfig, ServiceRequest serviceRequest, DeviceController& device, double gpsLatitude = NAN, double gpsLongitude = NAN);

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

    // Device-local wall-clock (DeviceController::getEpochSeconds()) of the last config poll that got a real HTTP response (200, with or without a changed body) - 0 means never. The local display "last sync" page reads this; not set on a 429/error response.
    time_t lastConfigSyncEpoch = 0;
};

// The one ServiceController instance, defined in main.cpp.
extern ServiceController service;

#endif