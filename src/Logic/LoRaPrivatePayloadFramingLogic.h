#ifndef LoRaPrivatePayloadFramingLogic_H
#define LoRaPrivatePayloadFramingLogic_H

#include <string>
#include <cstdint>

// Pure, native-testable byte layout for an encrypted LoRa private-protocol uplink payload (roadmap
// #395 finding 3) - matches AgrumyService's api.LoRa.LoRaPrivatePayloadCrypto wire format exactly:
// [counter:8 bytes big-endian][ciphertext][tag:16 bytes]. The actual AES-256-GCM encryption is
// mbedtls (Arduino/ESP32-only, see Controller/LoRaPrivateController.cpp), not native-testable -
// this file only builds the buffer around whatever ciphertext/tag that call already produced.

std::string encodeLoRaPrivateCipherFrame(uint64_t counter, const std::string &ciphertext, const uint8_t tag[16]);

#endif
