#include "Arduino.h"
#include "WiFi.h"
#include "HTTPClient.h"
#include <esp_task_wdt.h>
#include <WiFiClientSecure.h>
#include "AgrumyClient.h"
#include "InboxController.h"
#include "DeviceController.h"
#include "ServiceController.h"
#include "MqttController.h"
#include "OtaController.h"
#include "../Logic/HttpDateLogic.h"
#include "../Logic/NetworkRequestLogic.h"

#include <ArduinoJson.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

AgrumyClient agrumyClient;

static const uint32_t NETWORK_TASK_STACK_SIZE = 16384; // measured peak ~7.6KB (OTA), ~2.6KB (CA-bundle config polls, HWM 13804B after 8 TLS calls) - every KB here is one mbedTLS's ~45KB handshake cannot use; heap-backed, esp32dev's dram0_0_seg cannot hold it statically
static TaskHandle_t networkTask = nullptr;

static const UBaseType_t NETWORK_QUEUE_LENGTH = 4;
static QueueHandle_t networkQueue = nullptr;

static const uint32_t NETWORK_REQUEST_TIMEOUT_MS = 2 * HTTPCLIENT_DEFAULT_TCP_TIMEOUT + 10000; // HTTPClient's own connect+read timeouts plus a queueing margin
static const uint32_t NETWORK_ENQUEUE_TIMEOUT_MS = 200; // a full queue must fail fast, not wait anywhere near the request timeout

// Heap-allocated: a facade timeout leaves the task still working on it, so whoever releases second frees it (Logic/NetworkRequestLogic.h).
struct NetworkRequest
{
    enum Kind { Post, Get, Ota, MqttPublish, MqttConnect } kind;
    ServiceRequest service;
    const JsonDocument *payload = nullptr; // Post only - points into the caller's own stack frame, valid for as long as requestPost() itself blocks waiting on done
    OtaParams ota; // Ota only
    ServiceData result; // Post/Get only
    bool ok = false; // Ota/MqttPublish/MqttConnect only - each Sync method's own bool result
    String mqttTopic; // MqttPublish only
    String mqttPayload; // MqttPublish only
    int mqttTenantID = 0; // MqttConnect only
    int mqttDeviceID = 0; // MqttConnect only
    SemaphoreHandle_t done = nullptr;
    std::atomic<int> released{0};
};

static void releaseNetworkRequest(NetworkRequest *req)
{
    if (networkRequestReleaseShouldFree(req->released))
    {
        vSemaphoreDelete(req->done);
        delete req;
    }
}

static WiFiClientSecure networkSecureClient; // owned by the network task alone, shared by requestPostSync/requestGetSync - nothing outside the task touches it

static void networkTaskLoop(void *)
{
    for (;;)
    {
        NetworkRequest *req = nullptr;
        if (xQueueReceive(networkQueue, &req, portMAX_DELAY) != pdTRUE || req == nullptr)
        {
            continue;
        }

        esp_task_wdt_reset(); // covers every Post/Get, including one buffered-replay file per iteration

        switch (req->kind)
        {
        case NetworkRequest::Post:
            req->result = agrumyClient.requestPostSync(*req->payload, req->service);
            break;
        case NetworkRequest::Get:
            req->result = agrumyClient.requestGetSync(req->service);
            break;
        case NetworkRequest::Ota:
            req->ok = OtaController::update(req->ota.url, req->ota.isHttps, req->ota.servicePublicKey, req->ota.servicePoint, req->ota.expectedSha256);
            break;
        case NetworkRequest::MqttPublish:
            req->ok = mqtt.publishSync(req->mqttTopic, req->mqttPayload);
            break;
        case NetworkRequest::MqttConnect:
            req->ok = mqtt.connectPersistentSync(req->mqttTenantID, req->mqttDeviceID);
            break;
        }

        xSemaphoreGive(req->done);
        releaseNetworkRequest(req); // never touch req below this line - the facade may already be freeing it concurrently
    }
}

void AgrumyClient::beginNetworkTask()
{
    networkQueue = xQueueCreate(NETWORK_QUEUE_LENGTH, sizeof(NetworkRequest *));
    xTaskCreate(networkTaskLoop, "network", NETWORK_TASK_STACK_SIZE, nullptr, 1, &networkTask);
}

TaskHandle_t AgrumyClient::networkTaskHandle()
{
    return networkTask;
}

// Root CA bundle embedded via platformio.ini board_build.embed_files.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_data_cert_x509_crt_bundle_bin_start");

// Deliberately matches no catalog board, so a build that bypassed platformio.ini's define is never OTA'd.
#ifndef AGRUMY_BOARD
#define AGRUMY_BOARD "unknown"
#endif

// requestPost()'s Authorization header is built from this file-static, not ServiceRequest.header.
static String apiAuth;

// Masks the middle (first 4 + last 4 visible) so serial debugging can spot an obviously wrong value without ever printing a copy-pasteable secret.
String AgrumyClient::maskSecret(const String &value)
{
    if (value.length() == 0)
    {
        return value;
    }
    if (value.length() <= 8)
    {
        return "****"; // too short to show 4+4 without exposing the whole thing
    }
    return value.substring(0, 4) + "****...****" + value.substring(value.length() - 4);
}

static ServiceData buildQueueFullError()
{
    ServiceData result;
    result.eventlog.error = true;
    result.eventlog.errorCode = 1001;
    copyStr(result.eventlog.errorData, "Network task queue full");
    return result;
}

static ServiceData buildTimeoutError()
{
    ServiceData result;
    result.eventlog.error = true;
    result.eventlog.errorCode = 1002;
    copyStr(result.eventlog.errorData, "Network task did not respond in time");
    return result;
}

ServiceData AgrumyClient::requestPost(const JsonDocument& jsonBuffer, ServiceRequest service)
{
    NetworkRequest *req = new NetworkRequest();
    req->kind = NetworkRequest::Post;
    req->service = service;
    req->payload = &jsonBuffer;
    req->done = xSemaphoreCreateBinary();

    bool enqueued = networkRequestTryEnqueue([req]() {
        return xQueueSend(networkQueue, &req, pdMS_TO_TICKS(NETWORK_ENQUEUE_TIMEOUT_MS)) == pdTRUE;
    });
    if (!enqueued)
    {
        Serial.println("[AgrumyClient] requestPost: network task queue full");
        vSemaphoreDelete(req->done);
        delete req;
        return buildQueueFullError();
    }

    if (xSemaphoreTake(req->done, pdMS_TO_TICKS(NETWORK_REQUEST_TIMEOUT_MS)) != pdTRUE)
    {
        Serial.println("[AgrumyClient] requestPost: network task did not respond in time");
        releaseNetworkRequest(req);
        return buildTimeoutError();
    }

    ServiceData result = req->result;
    releaseNetworkRequest(req);
    return result;
}

ServiceData AgrumyClient::requestPostSync(const JsonDocument& jsonBuffer, ServiceRequest service)
{
    ServiceData serviceData;
    String jsonRequest;

    // Compact, not pretty - this string is the actual wire payload (http.POST(jsonRequest) below), never logged,
    // so indentation/newlines only cost extra bytes over the air and extra peak heap right before the TLS handshake.
    serializeJson(jsonBuffer, jsonRequest);

    if ((WiFi.status() == WL_CONNECTED))
    {
        HTTPClient http;
        String serviceURL = service.url();
        Serial.println("[AgrumyClient] POST: " + serviceURL);
        Serial.println("[AgrumyClient] apiId: " + service.header.apiId); // identifier, not a secret
        Serial.println("[AgrumyClient] apiKey: " + maskSecret(service.header.apiKey));
        Serial.println("[AgrumyClient] authKey: " + maskSecret(apiAuth));

        if (service.isHttps)
        {
            Serial.printf("[Diag] pre-TLS FreeHeap=%u MaxAllocHeap=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
            if (deviceConfig.servicePublicKey[0] != 0)
            {
                // Self-hosted deployment: operator pinned a (often self-signed) cert via the admin UI, so pin exactly that.
                networkSecureClient.setCACert(deviceConfig.servicePublicKey);
            }
            else
            {
                // Publicly-trusted cert: validate against the embedded CA bundle (CA rotation doesn't force a re-flash) - networkSecureClient persists across calls, so clear any prior call's CA cert first (setCACertBundle() doesn't).
                networkSecureClient.setCACert(nullptr);
                networkSecureClient.setCACertBundle(rootca_crt_bundle_start);
            }
            http.begin(networkSecureClient, serviceURL);
        }
        else
        {
            // Plain HTTP transitional path while http:// service points still exist.
            http.begin(serviceURL);
        }
        http.addHeader("Content-Type", "application/json");
        http.addHeader("apiId", service.header.apiId);
        http.addHeader("apiKey", service.header.apiKey);
        http.addHeader("Authorization", apiAuth);
        // Only headers named here reach header() below - HTTPClient doesn't retain arbitrary response headers by default.
        static const char *collectedHeaders[] = {"Retry-After", "Date"};
        http.collectHeaders(collectedHeaders, 2);

        int httpCode = http.POST(jsonRequest);

        // httpCode is negative on error
        if (httpCode > 0)
        {
            Serial.print("[HTTP] Code: ");
            Serial.println(httpCode);
            serviceData.eventlog.errorCode = httpCode;
            // Every real response carries the server's clock, not just a full config body - lets apiConfig() feed the NTP fallback on a bare heartbeat 200 too.
            serviceData.dateHeaderEpoch = httpDateToEpochSeconds(http.header("Date").c_str());

            if (httpCode == 200 || httpCode == 201)
            {
                serviceData.eventlog.error = false;
                serviceData.payload = http.getString();
            }
            else
            {
                serviceData.eventlog.error = true;
                if (httpCode == 429)
                {
                    String retryAfter = http.header("Retry-After");
                    if (retryAfter.length() > 0)
                    {
                        serviceData.retryAfterSeconds = retryAfter.toInt();
                    }
                }
            }
        }
        else
        {
            Serial.printf("[HTTP] Failed, error: %s\n", http.errorToString(httpCode).c_str());
            Serial.println("[HTTP] Error: Bad request.");
            Serial.println(http.getString());
            serviceData.eventlog.error = true;
            serviceData.eventlog.errorCode = httpCode;
        }

        http.end();
        // No keep-alive left the connection already dead - stop() releases networkSecureClient's mbedTLS context instead of leaving it half-torn-down until the next call reuses it.
        if (service.isHttps && !networkSecureClient.connected())
        {
            networkSecureClient.stop();
        }
    }
    else
    {
        serviceData.eventlog.errorCode = 1000;
        copyStr(serviceData.eventlog.errorData, "Wifi not available");
        // Guard against recursing into pushEvent() -> requestPost() -> "still no WiFi" -> pushEvent() forever. ::service is ServiceController's own global instance, not this method's local `service` parameter (a ServiceRequest) - pushEvent lives on the application layer, not here.
        if (service.endpoint != serviceEndpoint.apiEvent)
        {
            ::service.pushEvent(service, "NoInternet", "WiFi not connected");
        }
    }
    return serviceData;
}

ServiceData AgrumyClient::requestGet(ServiceRequest service)
{
    NetworkRequest *req = new NetworkRequest();
    req->kind = NetworkRequest::Get;
    req->service = service;
    req->done = xSemaphoreCreateBinary();

    bool enqueued = networkRequestTryEnqueue([req]() {
        return xQueueSend(networkQueue, &req, pdMS_TO_TICKS(NETWORK_ENQUEUE_TIMEOUT_MS)) == pdTRUE;
    });
    if (!enqueued)
    {
        Serial.println("[AgrumyClient] requestGet: network task queue full");
        vSemaphoreDelete(req->done);
        delete req;
        return buildQueueFullError();
    }

    if (xSemaphoreTake(req->done, pdMS_TO_TICKS(NETWORK_REQUEST_TIMEOUT_MS)) != pdTRUE)
    {
        Serial.println("[AgrumyClient] requestGet: network task did not respond in time");
        releaseNetworkRequest(req);
        return buildTimeoutError();
    }

    ServiceData result = req->result;
    releaseNetworkRequest(req);
    return result;
}

ServiceData AgrumyClient::requestGetSync(ServiceRequest service)
{
    ServiceData serviceData;

    if ((WiFi.status() == WL_CONNECTED))
    {
        HTTPClient http;
        String serviceURL = service.url();
        Serial.println("[AgrumyClient] GET: " + serviceURL);

        if (service.isHttps)
        {
            if (deviceConfig.servicePublicKey[0] != 0)
            {
                networkSecureClient.setCACert(deviceConfig.servicePublicKey);
            }
            else
            {
                networkSecureClient.setCACert(nullptr);
                networkSecureClient.setCACertBundle(rootca_crt_bundle_start);
            }
            http.begin(networkSecureClient, serviceURL);
        }
        else
        {
            http.begin(serviceURL);
        }
        // apiId/apiKey/Authorization ride along like every other request - harmless when the target endpoint (e.g. HardResetPending) ignores them and authenticates by query param instead.
        http.addHeader("apiId", service.header.apiId);
        http.addHeader("apiKey", service.header.apiKey);
        http.addHeader("Authorization", apiAuth);

        int httpCode = http.GET();
        if (httpCode > 0)
        {
            Serial.print("[HTTP] Code: ");
            Serial.println(httpCode);
            serviceData.eventlog.errorCode = httpCode;
            if (httpCode == 200)
            {
                serviceData.eventlog.error = false;
                serviceData.payload = http.getString();
            }
            else
            {
                serviceData.eventlog.error = true;
            }
        }
        else
        {
            Serial.printf("[HTTP] Failed, error: %s\n", http.errorToString(httpCode).c_str());
            serviceData.eventlog.error = true;
            serviceData.eventlog.errorCode = httpCode;
        }
        http.end();
        if (service.isHttps && !networkSecureClient.connected())
        {
            networkSecureClient.stop();
        }
    }
    else
    {
        serviceData.eventlog.errorCode = 1000;
        copyStr(serviceData.eventlog.errorData, "Wifi not available");
    }
    return serviceData;
}

bool AgrumyClient::firmwareUpdate(const OtaParams& params)
{
    NetworkRequest *req = new NetworkRequest();
    req->kind = NetworkRequest::Ota;
    req->ota = params;
    req->done = xSemaphoreCreateBinary();

    bool enqueued = networkRequestTryEnqueue([req]() {
        return xQueueSend(networkQueue, &req, pdMS_TO_TICKS(NETWORK_ENQUEUE_TIMEOUT_MS)) == pdTRUE;
    });
    if (!enqueued)
    {
        Serial.println("[AgrumyClient] firmwareUpdate: network task queue full");
        vSemaphoreDelete(req->done);
        delete req;
        return false;
    }

    // Polled, not portMAX_DELAY: the calling loopTask is watchdog-watched too and a multi-minute download must not starve it.
    while (xSemaphoreTake(req->done, pdMS_TO_TICKS(5000)) != pdTRUE)
    {
        esp_task_wdt_reset();
    }
    bool ok = req->ok;
    releaseNetworkRequest(req);
    return ok;
}

bool AgrumyClient::mqttPublish(const String& topic, const String& payload)
{
    NetworkRequest *req = new NetworkRequest();
    req->kind = NetworkRequest::MqttPublish;
    req->mqttTopic = topic;
    req->mqttPayload = payload;
    req->done = xSemaphoreCreateBinary();

    bool enqueued = networkRequestTryEnqueue([req]() {
        return xQueueSend(networkQueue, &req, pdMS_TO_TICKS(NETWORK_ENQUEUE_TIMEOUT_MS)) == pdTRUE;
    });
    if (!enqueued)
    {
        Serial.println("[AgrumyClient] mqttPublish: network task queue full");
        vSemaphoreDelete(req->done);
        delete req;
        return false;
    }

    if (xSemaphoreTake(req->done, pdMS_TO_TICKS(NETWORK_REQUEST_TIMEOUT_MS)) != pdTRUE)
    {
        Serial.println("[AgrumyClient] mqttPublish: network task did not respond in time");
        releaseNetworkRequest(req);
        return false;
    }

    bool ok = req->ok;
    releaseNetworkRequest(req);
    return ok;
}

bool AgrumyClient::mqttConnectPersistent(int tenantID, int deviceID)
{
    NetworkRequest *req = new NetworkRequest();
    req->kind = NetworkRequest::MqttConnect;
    req->mqttTenantID = tenantID;
    req->mqttDeviceID = deviceID;
    req->done = xSemaphoreCreateBinary();

    bool enqueued = networkRequestTryEnqueue([req]() {
        return xQueueSend(networkQueue, &req, pdMS_TO_TICKS(NETWORK_ENQUEUE_TIMEOUT_MS)) == pdTRUE;
    });
    if (!enqueued)
    {
        Serial.println("[AgrumyClient] mqttConnectPersistent: network task queue full");
        vSemaphoreDelete(req->done);
        delete req;
        return false;
    }

    if (xSemaphoreTake(req->done, pdMS_TO_TICKS(NETWORK_REQUEST_TIMEOUT_MS)) != pdTRUE)
    {
        Serial.println("[AgrumyClient] mqttConnectPersistent: network task did not respond in time");
        releaseNetworkRequest(req);
        return false;
    }

    bool ok = req->ok;
    releaseNetworkRequest(req);
    return ok;
}

// Bare "true"/"false" JSON body, no session/apiKey required server-side - the query-string apiId is the only thing that endpoint trusts.
bool AgrumyClient::isHardResetPending(ServiceRequest serviceRequest, const String &apiId)
{
    serviceRequest.endpoint = String(serviceEndpoint.apiHardResetPending) + "?apiId=" + apiId;
    ServiceData serviceData = requestGet(serviceRequest);
    return !serviceData.eventlog.error && serviceData.payload.indexOf("true") >= 0;
}

void AgrumyClient::apiAuthenticate(const DeviceConfig& deviceConfig, ServiceRequest serviceRequest, DeviceController& device)
{
    Serial.println("[AgrumyClient] apiAuthentication: ");
    serviceRequest.endpoint = serviceEndpoint.apiAuthenticate;
    serviceRequest.header.apiId = deviceConfig.apiId;
    serviceRequest.header.apiKey = deviceConfig.apiKey;

    ServiceData serviceData;
    JsonDocument payload;
    serviceData = requestPost(payload, serviceRequest);

    // A repeated 401 NEVER wipes the device on its own anymore (a transient server-side outage - cache, DB restore, a bad deploy - used to nuke the whole fleet simultaneously). The only path to a factory reset now is an admin explicitly setting the hard-reset flag from the Web console, checked here via apiId alone since a broken apiKey is exactly the scenario this exists for.
    if(serviceData.eventlog.errorCode==401){
        Serial.println("[AgrumyClient] Device failed authentication - checking whether an admin requested a hard reset");
        if (isHardResetPending(serviceRequest, deviceConfig.apiId))
        {
            inbox.handleHardReset("HardResetPending after a 401", device); // never returns
        }
        return; // no valid payload to parse below on a 401 - avoid setting apiAuth from an error body
    }

    DeserializationError error = deserializeJson(payload, serviceData.payload);
    if (error)
    {
        // A truncated/corrupt body must not leave a blank-but-"successful" apiAuth; clear it so the next 401 reflects real state and feeds the failure counter.
        Serial.print("[AgrumyClient] apiAuthenticate: deserializeJson failed: ");
        Serial.println(error.c_str());
        apiAuth = "";
        return;
    }

    String output = payload["apiAuth"];
    apiAuth = output;
    Serial.println("[AgrumyClient] apiAuthentication authKey: " + maskSecret(apiAuth));
}
