#ifndef LoRaPrivateSessionLogic_H
#define LoRaPrivateSessionLogic_H

#include <cstdint>
#include <string>

// Pure byte-layout helpers for the LoRa private-protocol v2 wire format (roadmap #468) - no crypto,
// no Arduino/mbedtls dependency, so these are native-testable against contracts/lora-private-v2.vectors.json
// the same way LoRaPrivatePayloadFramingLogic.cpp is for v1. Actual HKDF/AES-GCM stay in
// LoRaPrivateController.cpp (ESP32/mbedtls-only) and are verified against the same vectors on the
// AgrumyService (C#) side plus real hardware, per the roadmap's "Hardversko verificiranje" section.

/// v2 GCM nonce = bootNonce(8) || counter(4, big-endian), exactly 12 bytes.
void buildLoRaPrivateNonceV2(const uint8_t bootNonce[8], uint32_t counter, uint8_t nonceOut[12]);

/// v2 uplink wire frame = 0x02 || bootNonce(8) || counter(4 BE) || ciphertext || tag(16).
std::string encodeLoRaPrivateCipherFrameV2(const uint8_t bootNonce[8], uint32_t counter, const std::string &ciphertext, const uint8_t tag[16]);

/// True if a 13-byte downlink payload is the gateway's v2 ack for this exact (bootNonce, counter) - 0x02 || bootNonce(8) || counter(4 BE).
bool isCounterAckV2(const std::string &payload, const uint8_t bootNonce[8], uint32_t counter);

#endif
