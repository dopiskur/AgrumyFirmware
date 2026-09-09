#include "Arduino.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include "mbedtls/sha256.h" // hardware-accelerated on ESP32

#include "OtaController.h"

// Same CA bundle ServiceController::requestPost() validates against; re-declared here for the OTA download.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_data_cert_x509_crt_bundle_bin_start");

// 32 raw bytes -> 64 lowercase hex chars + NUL.
static void sha256ToHex(const unsigned char digest[32], char out[65])
{
  static const char *hexDigits = "0123456789abcdef";
  for (int i = 0; i < 32; i++)
  {
    out[i * 2] = hexDigits[(digest[i] >> 4) & 0x0F];
    out[i * 2 + 1] = hexDigits[digest[i] & 0x0F];
  }
  out[64] = '\0';
}

// The authority (host[:port]) segment of a URL, for an EXACT host comparison, not substring matching.
static String extractHost(const String &url)
{
  int schemeEnd = url.indexOf("://");
  if (schemeEnd < 0)
  {
    return String();
  }
  int hostStart = schemeEnd + 3;
  int hostEnd = url.indexOf('/', hostStart);
  return hostEnd < 0 ? url.substring(hostStart) : url.substring(hostStart, hostEnd);
}

bool OtaController::update(String url, bool isHttps, const String &servicePublicKey, const String &servicePoint, const String &expectedSha256)
{
  Serial.println("[Firmware] Starting OTA update from: " + url);

  // Both required, no soft-skip - refusing outright is safer than flashing an unverified image.
  if (!isHttps)
  {
    Serial.println("[Firmware] Refusing OTA over HTTP - only HTTPS is allowed");
    return false;
  }
  if (expectedSha256.length() != 64)
  {
    Serial.println("[Firmware] Refusing OTA without a valid SHA-256 (expected 64 hex chars)");
    return false;
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("[Firmware] No WiFi, aborting OTA");
    return false;
  }

  HTTPClient http;
  WiFiClientSecure secureClient;

  if (isHttps)
  {
    // The download host decides which trust to use: our own API validates against a pinned servicePublicKey (if set), any other host (GitHub release, Custom repository) against the embedded CA bundle - a pinned cert can never match those.
    bool ownServer = servicePublicKey.length() > 0 &&
                     servicePoint.length() > 0 &&
                     extractHost(url).equalsIgnoreCase(servicePoint);
    if (ownServer)
    {
      secureClient.setCACert(servicePublicKey.c_str());
    }
    else
    {
      secureClient.setCACert(nullptr);
      secureClient.setCACertBundle(rootca_crt_bundle_start);
    }
    http.begin(secureClient, url);
  }
  else
  {
    http.begin(url);
  }
  // A GitHub release asset URL answers 302 to objects.githubusercontent.com - without this the GET returns the redirect itself (HTTP 302, no body) and OTA "fails".
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK)
  {
    Serial.printf("[Firmware] Download failed, HTTP code: %d\n", httpCode);
    http.end();
    return false;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0)
  {
    Serial.printf("[Firmware] Invalid content length: %d\n", contentLength);
    http.end();
    return false;
  }

  if (!Update.begin(contentLength))
  {
    Serial.printf("[Firmware] Update.begin failed (need %d bytes free in OTA partition): %s\n",
                  contentLength, Update.errorString());
    http.end();
    return false;
  }

  // Hashed WHILE streaming so the check covers exactly the bytes handed to Update.write().
  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts(&shaCtx, 0); // 0 = SHA-256, not the truncated SHA-224 variant

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  size_t remaining = (size_t)contentLength;
  while (remaining > 0 && http.connected())
  {
    esp_task_wdt_reset(); // runs on ServiceController's network task now - a whole image can take far longer than one WDT period, so this must be fed per chunk, not just once per request like everything else on that task
    size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
    size_t got = stream->readBytes(buf, chunk);
    if (got == 0)
    {
      break; // stalled/closed before all bytes arrived - reported as a short write below
    }
    if (Update.write(buf, got) != got)
    {
      Serial.printf("[Firmware] Update.write failed: %s\n", Update.errorString());
      Update.abort();
      http.end();
      mbedtls_sha256_free(&shaCtx);
      return false;
    }
    mbedtls_sha256_update(&shaCtx, buf, got);
    remaining -= got;
  }

  if (remaining > 0)
  {
    Serial.printf("[Firmware] Wrote %u/%d bytes\n", (unsigned)(contentLength - remaining), contentLength);
    Update.abort();
    http.end();
    mbedtls_sha256_free(&shaCtx);
    return false;
  }

  {
    unsigned char digest[32];
    mbedtls_sha256_finish(&shaCtx, digest);
    mbedtls_sha256_free(&shaCtx);
    char actualHex[65];
    sha256ToHex(digest, actualHex);
    if (!expectedSha256.equalsIgnoreCase(actualHex))
    {
      Serial.printf("[Firmware] SHA-256 mismatch - expected %s, got %s. Aborting before flash is finalized.\n",
                    expectedSha256.c_str(), actualHex);
      Update.abort();
      http.end();
      return false;
    }
    Serial.println("[Firmware] SHA-256 verified");
  }

  if (!Update.end() || !Update.isFinished())
  {
    Serial.printf("[Firmware] Update did not finish: %s\n", Update.errorString());
    http.end();
    return false;
  }

  http.end();
  Serial.println("[Firmware] Update written and verified");
  return true;
}
