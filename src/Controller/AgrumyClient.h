#ifndef AgrumyClient_H
#define AgrumyClient_H
#include "Arduino.h"
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../Model/DeviceModel.h"

// Forward declaration instead of an include
class DeviceController;

// OtaController::update()'s parameters, bundled so a firmware update rides the same network task queue as requestPost/requestGet.
struct OtaParams
{
    String url;
    bool isHttps = false;
    String servicePublicKey;
    String servicePoint;
    String expectedSha256;
};

// Roadmap #457 (EPIC A3) - the transport/session layer split out of ServiceController: endpoints, the
// persistent network task, session-token (apiAuth) lifecycle, and 401/429 retry policy. ServiceController
// keeps the application layer (ConfigApplier/CommandExecutor - apiConfig/processPendingCommand/pushEvent
// and friends), calling into this class for every actual network round trip instead of doing its own I/O.
class AgrumyClient
{
public:
    // Creates the one persistent network task; call exactly once from setup(), before anything can call requestPost/requestGet/firmwareUpdate.
    static void beginNetworkTask();
    static TaskHandle_t networkTaskHandle();

    // First 4 + last 4 characters visible, rest replaced.
    static String maskSecret(const String &value);

    ServiceData requestPost(const JsonDocument& jsonBuffer, ServiceRequest serviceEndpoint);
    ServiceData requestGet(ServiceRequest service);

    // OtaController::update on the network task; true only once the image is downloaded and verified (caller reboots).
    bool firmwareUpdate(const OtaParams& params);

    // MqttController::publishSync/connectPersistentSync on the network task - same "TLS handshake never on loopTask" discipline as requestPost, so an MQTT TLS session and an HTTPS one never run concurrently and fight over heap.
    bool mqttPublish(const String& topic, const String& payload);
    bool mqttConnectPersistent(int tenantID, int deviceID);

    // Sets/clears the session token (apiAuth) used by every requestPost/requestGet's Authorization header. A repeated 401 checks isHardResetPending (an admin-set flag, apiId-only) before giving up - it never wipes the device on its own.
    void apiAuthenticate(const DeviceConfig& deviceConfig, ServiceRequest serviceRequest, DeviceController& device);

    // Actual HTTP(S) logic, run ONLY on the persistent network task - see AgrumyClient.cpp's networkTaskLoop. Public only so that free function can call it; not part of the intended external API.
    ServiceData requestPostSync(const JsonDocument& jsonBuffer, ServiceRequest service);
    ServiceData requestGetSync(ServiceRequest service);

private:
    // Bare "true"/"false" JSON body, no session/apiKey required server-side - the query-string apiId is the only thing that endpoint trusts.
    bool isHardResetPending(ServiceRequest serviceRequest, const String &apiId);
};

// The one AgrumyClient instance, defined in AgrumyClient.cpp.
extern AgrumyClient agrumyClient;

#endif
