#ifndef LoRaGatewayBridgeController_H
#define LoRaGatewayBridgeController_H

#include <Arduino.h>

/// The LoRa private-protocol Gateway's radio-frontend board (mains-powered, always-on ESP32+SX126x) -
/// bridges raw RadioLib LoRa frames (Logic/LoRaPrivateFrameLogic) to/from Agrumy.Gateway over USB
/// serial (Logic/AgrumySerialFrameLogic). Mostly dumb (no addressing/mapping decisions - all routing
/// logic lives server-side in api.Gateway.LoRaPrivate.LoRaPrivateProtocolUplinkService, same
/// "gateway is a transparent forwarder" principle as Profile A), with one exception: it immediately
/// acks a received uplink's plaintext counter prefix back over LoRa itself, without waiting on the
/// serial/host round trip, since that has to land inside the node's ~1s RX1 window. Two-board
/// over-the-air uplink, the serial link to a real Agrumy.Gateway process, and this ack round trip
/// have all been confirmed on real Heltec WiFi LoRa 32 V3 hardware (2026-09-06/08).
class LoRaGatewayBridgeController
{
public:
    bool begin();

    /// Call every loop() iteration - polls the radio briefly for an uplink and drains any pending
    /// serial bytes for a downlink, forwarding each direction as it finds one.
    void runOnce();

private:
    void pollRadioForUplink();
    void pollSerialForDownlink();

    uint16_t gatewayAddress = 0;
    float frequencyMHz = 868.0;
    uint8_t spreadingFactor = 9;
    float bandwidthKHz = 125.0;
    uint8_t codingRate = 7;
    int8_t txPowerDbm = 22;
    bool configLoaded = false;

    // Accumulates incoming serial bytes across loop() iterations until a full AgrumySerialFrame downlink is available.
    static const size_t SERIAL_BUFFER_CAPACITY = 512;
    uint8_t serialBuffer[SERIAL_BUFFER_CAPACITY];
    size_t serialBufferLength = 0;
};

#endif
