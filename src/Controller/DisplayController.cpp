#include "Arduino.h"
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "DisplayController.h"
#include "RelayIO.h"

static const int SCREEN_WIDTH = 128;
static const int SCREEN_HEIGHT = 64;
// -1: no dedicated reset pin wired on this kit's OLED slot, same as every common SSD1306 breakout without one.
static Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

bool DisplayController::begin(const ConfigPin &pin)
{
    if (pin.DISPLAY_I2C_ADDRESS == 0)
    {
        return false;
    }
    // Already begun by RelayIO's own lazy Wire.begin() if a relay was driven first this boot - harmless to call again with the same pins.
    Wire.begin(pin.RELAY_I2C_SDA, pin.RELAY_I2C_SCL);
    ready = oled.begin(SSD1306_SWITCHCAPVCC, pin.DISPLAY_I2C_ADDRESS);
    if (!ready)
    {
        Serial.println("[Display] SSD1306 not detected on I2C bus - screen disabled for this boot");
        return false;
    }
    oled.clearDisplay();
    oled.display();
    lastPageChangeMs = millis();
    return true;
}

void DisplayController::update(const DeviceConfig &config, const SensorData &sensorData, const ActuatorController &controller, time_t lastConfigSyncEpoch, time_t nowEpoch)
{
    if (!ready)
    {
        return;
    }
    cachedConfig = config;
    cachedSensorData = sensorData;
    cachedRelayOn[0] = controller.isRelayOn(RelayFunctionType::Ventilation);
    cachedRelayOn[1] = controller.isRelayOn(RelayFunctionType::Light);
    cachedRelayOn[2] = controller.isRelayOn(RelayFunctionType::Heating);
    cachedRelayOn[3] = controller.isRelayOn(RelayFunctionType::WaterPump);
    cachedLastSyncEpoch = lastConfigSyncEpoch;
    cachedNowEpoch = nowEpoch;
    drawCurrentPage();
}

void DisplayController::tick()
{
    if (!ready)
    {
        return;
    }
    if (millis() - lastPageChangeMs < PAGE_MS)
    {
        return;
    }
    currentPage = (currentPage + 1) % PAGE_COUNT;
    lastPageChangeMs = millis();
    drawCurrentPage();
}

void DisplayController::drawCurrentPage()
{
    switch (currentPage)
    {
    case 0: drawIdentityPage(); break;
    case 1: drawReadingsPage(); break;
    case 2: drawRelaysPage(); break;
    default: drawStatusPage(); break;
    }
}

void DisplayController::drawIdentityPage()
{
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println("Agrumy");
    oled.printf("Device ID: %d\n", cachedConfig.deviceID);
    oled.printf("Kit: %s\n", AGRUMY_KIT);
    oled.printf("FW: %s\n", cachedConfig.firmwareVersion.length() > 0 ? cachedConfig.firmwareVersion.c_str() : "?");
    oled.display();
}

void DisplayController::drawReadingsPage()
{
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println("Sensors");
    // NAN means "absent/disabled this cycle" (same convention as SensorController::buildSensorData) - skip rather than print garbage.
    if (!isnan(cachedSensorData.temperature)) oled.printf("Temp: %.1f C\n", cachedSensorData.temperature);
    if (!isnan(cachedSensorData.humidity)) oled.printf("Humid: %.0f %%\n", cachedSensorData.humidity);
    if (!isnan(cachedSensorData.moisture)) oled.printf("Moist: %.0f %%\n", cachedSensorData.moisture);
    if (!isnan(cachedSensorData.light)) oled.printf("Light: %.0f lx\n", cachedSensorData.light);
    if (!isnan(cachedSensorData.waterLevel)) oled.printf("Water: %.0f\n", cachedSensorData.waterLevel);
    if (!isnan(cachedSensorData.battery)) oled.printf("Batt: %.0f %%\n", cachedSensorData.battery);
    oled.display();
}

void DisplayController::drawRelaysPage()
{
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println("Relays");
    oled.printf("Vent:  %s\n", cachedRelayOn[0] ? "ON" : "off");
    oled.printf("Light: %s\n", cachedRelayOn[1] ? "ON" : "off");
    oled.printf("Heat:  %s\n", cachedRelayOn[2] ? "ON" : "off");
    oled.printf("Pump:  %s\n", cachedRelayOn[3] ? "ON" : "off");
    oled.display();
}

void DisplayController::drawStatusPage()
{
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println("Status");
    if (WiFi.status() == WL_CONNECTED)
    {
        oled.printf("WiFi: %d dBm\n", WiFi.RSSI());
    }
    else
    {
        oled.println("WiFi: disconnected");
    }
    if (cachedLastSyncEpoch <= 0)
    {
        oled.println("Sync: never");
    }
    else
    {
        long ageSeconds = (long)(cachedNowEpoch - cachedLastSyncEpoch);
        if (ageSeconds < 0) ageSeconds = 0;
        oled.printf("Sync: %ldm ago\n", ageSeconds / 60);
    }
    oled.printf("I2C: %s\n", relayI2CFaulted() ? "FAULT" : "OK");
    oled.display();
}
