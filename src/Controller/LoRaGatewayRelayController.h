#ifndef LoRaGatewayRelayController_H
#define LoRaGatewayRelayController_H

#include <Arduino.h>
#include "../Model/DeviceModel.h"

class ServiceController;

// Standalone/dual-role WiFi+LoRa gateway. Same RF receive/decode as
// LoRaGatewayBridgeController, but relays each frame directly over THIS device's own WiFi/HTTP
// connection (POST /api/Gateway/RelayUplink) instead of a serial link to a separate,
// mains-powered Agrumy.Gateway process - lets an ordinary sensor/controller device (KC868-A6) or
// a minimal standalone node (Heltec V3/V4) double as a gateway with no extra hardware. Compiled
// in only under AGRUMY_LORA_GATEWAY_CAPABLE; deliberately dumb like LoRaGatewayBridgeController -
// node-address -> device resolution happens server-side (GatewayApiController.RelayUplink).
class LoRaGatewayRelayController
{
public:
    // Attempts radio.begin() on this board's LoRa chip. False means no chip physically
    // present/wired - caller should push DeviceEventType.LoRaHardwareNotDetected and never call poll().
    bool begin();

    // Call every loop() iteration once begin() succeeded and deviceConfig.loRaGatewayEnabled is
    // true - briefly polls the radio for one uplink and relays it via serviceRequest's servicePoint,
    // authenticated with deviceConfig's own PERMANENT apiId/apiKey (RelayUplink uses
    // DeviceAuth.ApiKeyPolicy like Batch, not the shared session apiAuth token loop()'s other calls use).
    void poll(ServiceController &service, ServiceRequest serviceRequest, const DeviceConfig &deviceConfig);

private:
    bool radioReady = false;
};

#endif
