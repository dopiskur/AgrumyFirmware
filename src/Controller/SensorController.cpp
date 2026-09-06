#include <Arduino.h>
#include <ArduinoJson.h>
#include <SPI.h>

#include <Wire.h>
#include <DHT.h> // DHT temp, humidity
#include <DHT_U.h>
#include <BH1750.h> // Light sensor

#include <Adafruit_Sensor.h>
#include <Adafruit_BMP085.h> // BMP180 temp, pressure
#include <Adafruit_BMP280.h>
#include <Adafruit_BME280.h> // BME280 temp, humidity, pressure
#include <Adafruit_CCS811.h> // CCS811 CO2, TVOC
#include <SparkFun_MAX1704x_Fuel_Gauge_Arduino_Library.h> // MAX17048 battery fuel gauge
#include <OneWire.h>
#include <DallasTemperature.h> // DS18B20 soil temperature, single-wire bus

#include <esp_task_wdt.h>

#include "SensorController.h"
#include "DeviceController.h"
#include "ServiceController.h"
#include "ActuatorController.h"
#include "MqttController.h"
#include "../Logic/BatteryLogic.h" // divider math + LiPo voltage->percent curve

static JsonDocument jsonDoc;
static JsonArray sensorDataJsonArray = jsonDoc.to<JsonArray>();
static String dateTime;

static Adafruit_CCS811 ccs811;                               // Co2, Tvoc
// DHT_Unified needs its pin at static-init time, before main.cpp's canonical `deviceConfig` is guaranteed constructed (unspecified cross-TU static-init order) - a locally-scoped default ConfigPin avoids the dependency.
static ConfigPin defaultPins;
static DHT_Unified dht11(defaultPins.DHT, DHT11); // temp, humidity
static DHT_Unified dht22(defaultPins.DHT, DHT22); // temp, humidity
static Adafruit_BMP085 bmp180;                               // temp, pressure
static Adafruit_BMP280 bmp280;                               // temp, pressure
static Adafruit_BME280 bme280;                               // temp, humidity, pressure
BH1750 Bh1750;                                               // light
static SFE_MAX1704X maxlipo;                                  // battery fuel gauge

// OneWire's pin is a constructor argument (not a begin() parameter) and comes from deviceConfig, unknown at static-init time (same reason defaultPins exists above), so these stay null until setupSensor() constructs them at runtime.
static OneWire *oneWireTempSoil;
static DallasTemperature *ds18b20;

static unsigned bmp280status;
static unsigned bmp180status;
static unsigned bme280status;
static unsigned bh1750status;
static bool max17048status;
static bool ds18b20status; // true once at least one DS18B20 answers on the bus


void SensorController::setupSensor()
{
    Serial.println("[Sensor setup]");

    dht11.begin();
    dht22.begin();

    // Only probe a chip this device's config actually selects - begin() on an address nothing answers at produces "i2cWriteReadNonStop returned Error -1" bus-probe noise.
    if (deviceConfig.configSensor.sensorTemp == SensorTypeIds::Bmp180 || deviceConfig.configSensor.sensorBarometer == SensorTypeIds::Bmp180)
    {
        bmp180status = bmp180.begin(0x77);
    }
    if (deviceConfig.configSensor.sensorTemp == SensorTypeIds::Bmp280 || deviceConfig.configSensor.sensorBarometer == SensorTypeIds::Bmp280)
    {
        bmp280status = bmp280.begin(0x76);
    }
    if (deviceConfig.configSensor.sensorTemp == SensorTypeIds::Bme280 || deviceConfig.configSensor.sensorHumid == SensorTypeIds::Bme280 || deviceConfig.configSensor.sensorBarometer == SensorTypeIds::Bme280)
    {
        bme280status = bme280.begin(0x76);
    }
    if (deviceConfig.configSensor.sensorTempSoil == SensorTypeIds::Ds18B20)
    {
        oneWireTempSoil = new OneWire(deviceConfig.configPin.TEMPSOIL);
        ds18b20 = new DallasTemperature(oneWireTempSoil);
        ds18b20->begin();
        ds18b20status = ds18b20->getDeviceCount() > 0;
    }
    if (deviceConfig.configSensor.sensorCo2 == SensorTypeIds::Ccs811 || deviceConfig.configSensor.sensorTvoc == SensorTypeIds::Ccs811)
    {
        ccs811.begin(0x5A); // 0x5B is default, mine is older version
    }
    if (deviceConfig.configSensor.sensorLight == SensorTypeIds::Bh1750)
    {
        bh1750status = Bh1750.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
    }

    // Fixed I2C address 0x36, shares the bus already begun above. Harmless to call when BatterySensorType is None/VoltageDivider - it just never gets read.
    max17048status = maxlipo.begin();

    delay(5000);
}

void SensorController::reportDHTTemperature(sensors_event_t &event, const char *label)
{
    Serial.print("[Sensor] ");
    Serial.print(label);
    Serial.println(" temperature");
    if (isnan(event.temperature))
    {
        Serial.println("Error reading temperature!");
    }
    else
    {
        Serial.print("Temperature = ");
        Serial.print(event.temperature);
        Serial.println(" C");
        sensorData.temperature=event.temperature;
    }
}

void SensorController::reportDHTHumidity(sensors_event_t &event, const char *label)
{
    Serial.print("[Sensor] ");
    Serial.print(label);
    Serial.println(" humidity");
    if (isnan(event.relative_humidity))
    {
        Serial.println("Error reading humidity!");
    }
    else
    {
        Serial.print("Humidity = ");
        Serial.print(event.relative_humidity);
        Serial.println(" %");
        sensorData.humidity=event.relative_humidity;
    }
}

void SensorController::sensor_DHT11_temp()
{
    sensors_event_t event;
    dht11.temperature().getEvent(&event);
    reportDHTTemperature(event, "DHT11");
}
void SensorController::sensor_DHT11_humid()
{
    sensors_event_t event;
    dht11.humidity().getEvent(&event);
    reportDHTHumidity(event, "DHT11");
}

void SensorController::sensor_DHT22_temp()
{
    delay(500);
    dht22.begin();
    delay(500);
    sensors_event_t event;
    dht22.temperature().getEvent(&event);
    reportDHTTemperature(event, "DHT22");
}
void SensorController::sensor_DHT22_humid()
{
    delay(500);
    dht22.begin();
    delay(500);
    sensors_event_t event;
    dht22.humidity().getEvent(&event);
    reportDHTHumidity(event, "DHT22");
}

void SensorController::reportSensorInitError(const char *label)
{
    Serial.print("[Sensor] ");
    Serial.print(label);
    Serial.println(" error reading data");
    sensorData.eventlog.error = true;
}

void SensorController::reportTemperature(double celsius)
{
    Serial.print("Temperature = ");
    Serial.print(celsius);
    Serial.println(" *C");
    sensorData.temperature = celsius;
}

void SensorController::reportPressure(double pascals)
{
    Serial.print("Pressure = ");
    Serial.print(pascals);
    Serial.println(" Pa");
    sensorData.barometer = pascals;
}

void SensorController::sensor_BMP180_temp()
{
    Serial.println("[Sensor] BMP180 temperature");
    if (!bmp180status) reportSensorInitError("BMP180");
    else reportTemperature(bmp180.readTemperature());
}
void SensorController::sensor_BMP180_pres()
{
    Serial.println("[Sensor] BMP180 pressure");
    if (!bmp180status) reportSensorInitError("BMP180");
    else reportPressure(bmp180.readPressure());
}

void SensorController::sensor_BMP280_temp()
{
    Serial.println("[Sensor] BMP280 temperature");
    if (!bmp280status) reportSensorInitError("BMP280");
    else reportTemperature(bmp280.readTemperature());
}
void SensorController::sensor_BMP280_pres()
{
    Serial.println("[Sensor] BMP280 pressure");
    if (!bmp280status) reportSensorInitError("BMP280");
    else reportPressure(bmp280.readPressure());
}

void SensorController::sensor_BME280_temp()
{
    Serial.println("[Sensor] BME280 temperature");
    if (!bme280status) reportSensorInitError("BME280");
    else reportTemperature(bme280.readTemperature());
}
void SensorController::sensor_BME280_humid()
{
    Serial.println("[Sensor] BME280 humidity");
    if (!bme280status)
    {
        reportSensorInitError("BME280");
        return;
    }
    float humidity = bme280.readHumidity();
    Serial.print("Humidity = ");
    Serial.print(humidity);
    Serial.println(" %");
    sensorData.humidity = humidity;
}
void SensorController::sensor_BME280_pres()
{
    Serial.println("[Sensor] BME280 pressure");
    if (!bme280status) reportSensorInitError("BME280");
    else reportPressure(bme280.readPressure());
}

void SensorController::sensor_DS18B20_temp()
{
    Serial.println("[Sensor] DS18B20 soil temperature");
    if (!ds18b20status)
    {
        reportSensorInitError("DS18B20");
        return;
    }

    ds18b20->requestTemperatures();
    float celsius = ds18b20->getTempCByIndex(0);
    if (celsius == DEVICE_DISCONNECTED_C)
    {
        reportSensorInitError("DS18B20");
        return;
    }

    Serial.print("Soil temperature = ");
    Serial.print(celsius);
    Serial.println(" *C");
    sensorData.temperatureSoil = celsius;
}

void SensorController::sensor_BH1750_lux()
{
    Serial.println("[Sensor] BH1750 lux");
    // Uses the global Bh1750 already begun in setupSensor() - do not shadow it with a local instance.
    if (!bh1750status)
    {
        reportSensorInitError("BH1750");
        return;
    }

    double lux = Bh1750.readLightLevel();
    Serial.print("Light: ");
    Serial.print(lux);
    Serial.println(" lx");
    Serial.println();

    sensorData.light = lux;
}

// CCS811 needs ~20min to heat up before it has data; available()+readData() is identical for both quantities, only the getter/target field differs per caller.
bool SensorController::tryReadCCS811()
{
    if (!ccs811.available())
    {
        return false;
    }
    if (ccs811.readData())
    {
        Serial.println("[Sensor] CCS811 waiting for sensor to heat up, no data");
        return false;
    }
    return true;
}

void SensorController::sensor_CCS811_co2()
{
    Serial.println("[Sensor] CCS811 Co2");
    if (tryReadCCS811())
    {
        Serial.print("CO2: ");
        Serial.println(ccs811.geteCO2());
        sensorData.co2 = ccs811.geteCO2();
    }
}
void SensorController::sensor_CCS811_tvoc()
{
    Serial.println("[Sensor] CCS811 Tvoc");
    if (tryReadCCS811())
    {
        Serial.print("ppm, TVOC: ");
        Serial.println(ccs811.getTVOC());
        sensorData.tvoc = ccs811.getTVOC();
    }
}

void SensorController::sensor_analog_voltage() // SensorTypeIds::AnalogVoltage, VoltageDivider
{
    Serial.println("[Sensor battery - voltage divider]");
    device.powerRailSecondary(true);

    // analogReadMilliVolts(), not raw analogRead(): the ESP32 Arduino core already runs it through esp-idf's adc_cali API (eFuse-based per-chip calibration).
    uint32_t measuredMilliVolts = analogReadMilliVolts(deviceConfig.configPin.BATTERY_ADC);
    double batteryVoltage = computeDividerBatteryVoltage(
        measuredMilliVolts / 1000.0,
        deviceConfig.configSensor.batteryDividerR1,
        deviceConfig.configSensor.batteryDividerR2);
    int percent = computeBatteryPercentFromVoltage(batteryVoltage);

    device.powerRailSecondary(false);

    Serial.print("Battery voltage: ");
    Serial.print(batteryVoltage);
    Serial.print("V (");
    Serial.print(percent);
    Serial.println("%)");

    sensorData.battery = percent;
}

void SensorController::sensor_battery_max17048() // SensorTypeIds::Max17048
{
    Serial.println("[Sensor battery - MAX17048]");
    if (!max17048status)
    {
        Serial.println("MAX17048 not detected on I2C bus - skipping battery reading");
        return;
    }

    // getSOC() is the fuel gauge's own coulomb-counting percentage - no voltage curve needed.
    float percent = maxlipo.getSOC();
    sensorData.battery = (int)constrain(percent, 0.0f, 100.0f);
}

void SensorController::sensor_analog_moist()
{
    
    Serial.println("[Sensor moisture]");
    device.powerRailSecondary(true);

    int moisture = analogRead(deviceConfig.configPin.MOIST);

    Serial.print("Analog: ");
    Serial.println(moisture);

    int soilWet = 1200;
    int soilDry = 3000;
    if (moisture < soilWet)
    {
        Serial.println("Status: Soil is too wet");
    }
    else if (moisture >= soilWet && moisture < soilDry)
    {
        Serial.println("Status: Soil is moist");
    }
    else
    {
        Serial.println("Status: Soil is too dry");
    }

    if (moisture != 0)
    {
        // soilWet (low raw ADC) is 100% wet, soilDry (high raw ADC) is 0% - constrain first so a reading past either calibration bound still clamps to 0-100 instead of extrapolating past it.
        sensorData.moisture = map(constrain(moisture, soilWet, soilDry), soilWet, soilDry, 100, 0);
    }
    else
    {
        Serial.println("Moisture sensor not present");
        service.pushEvent(serviceRequest, "SensorMissing", "Moisture sensor reading 0 - likely disconnected");
    }

    device.powerRailSecondary(false);
    Serial.println();
}
void SensorController::sensor_Wind()
{
}

void SensorController::sensor_liquid_PH()
{
}

void SensorController::sensor_analog_waterLevel()
{
    Serial.println("[Sensor water level]");
    device.powerRailSecondary(true);

    int waterTank = analogRead(deviceConfig.configPin.WaterTank);

    Serial.print("Analog: ");
    Serial.println(waterTank);

    sensorData.waterLevel = waterTank;

    device.powerRailSecondary(false);
}

void SensorController::sensor_rainLevel()
{
}

void SensorController::buildSensorDataPayload()
{
    Serial.println(sensorData.temperature);
    Serial.println(sensorData.humidity);
    Serial.println(sensorData.barometer);
    Serial.println(sensorData.co2);
    Serial.println(sensorData.tvoc);
    Serial.println(sensorData.light);

    JsonDocument jsonSensorData;

    jsonSensorData["deviceID"]=deviceConfig.deviceID;
    jsonSensorData["tenantID"]=deviceConfig.tenantID;
    jsonSensorData["deviceUnitID"]=deviceConfig.deviceUnitID;
    jsonSensorData["deviceUnitZoneID"]=deviceConfig.deviceUnitZoneID;


    // JsonVariant() serializes as JSON null - isnan(...) is this struct's "no reading this cycle" state (see DeviceModel.h SensorData).
    jsonSensorData["temperature"]=!isnan(sensorData.temperature)? sensorData.temperature:  JsonVariant();
    jsonSensorData["soilTemperature"]=!isnan(sensorData.temperatureSoil)? sensorData.temperatureSoil:  JsonVariant();
    jsonSensorData["humidity"]=!isnan(sensorData.humidity)? sensorData.humidity:  JsonVariant();
    jsonSensorData["battery"]=!isnan(sensorData.battery)? sensorData.battery:  JsonVariant();
    jsonSensorData["moisture"]=!isnan(sensorData.moisture)? sensorData.moisture:  JsonVariant();
    jsonSensorData["light"]=!isnan(sensorData.light)? sensorData.light:  JsonVariant();
    jsonSensorData["co2"]=!isnan(sensorData.co2)? sensorData.co2:  JsonVariant();
    jsonSensorData["tvoc"]=!isnan(sensorData.tvoc)? sensorData.tvoc:  JsonVariant();
    jsonSensorData["barometer"]=!isnan(sensorData.barometer)? sensorData.barometer:  JsonVariant();
    jsonSensorData["liquidPH"]=!isnan(sensorData.liquidPH)? sensorData.liquidPH:  JsonVariant();
    jsonSensorData["rainLevel"]=!isnan(sensorData.rainLevel)? sensorData.rainLevel:  JsonVariant();
    jsonSensorData["waterLevel"]=!isnan(sensorData.waterLevel)? sensorData.waterLevel:  JsonVariant();
    jsonSensorData["wind"]=!isnan(sensorData.wind)? sensorData.wind:  JsonVariant();
    // Computed once - calling getDateTime() twice in one expression let the two calls straddle a second boundary and disagree.
    String dateCreated = device.getDateTime();
    jsonSensorData["dateCreated"]=(dateCreated)!=""? dateCreated:  JsonVariant(); // timestamp for buffering

    sensorDataJsonArray.add(jsonSensorData); // buffer if the service point is unavailable

    // Additive channel alongside the HTTPS buffer above - best-effort, never buffered/retried.
    mqtt.publishSensorData(deviceConfig, jsonSensorData);

    String sensorDataDebug;
    serializeJsonPretty(jsonSensorData,sensorDataDebug);

    Serial.println("[Sensor] Buffered sensorData:");
    Serial.println(sensorDataDebug);

    pushSensorData(sensorDataJsonArray); 
}

// One full RAM buffer's worth per file (~400 bytes/reading, so this spills roughly every 20 failed cycles; ~170 files fit under the 70% partition cap).
static const size_t SENSOR_BUFFER_SPILL_BYTES = 8192;

bool SensorController::flushBufferedSensorData()
{
    String filename = device.oldestBufferedSensorFile();
    if (filename.isEmpty())
    {
        return true; // nothing queued - the common case, one directory scan and out
    }

    serviceRequest.endpoint = serviceEndpoint.apiSensorDataPost;
    serviceRequest.header.apiId = deviceConfig.apiId;

    while (!filename.isEmpty())
    {
        String payloadJson = device.loadFile(filename);

        JsonDocument payload;
        if (payloadJson.isEmpty() || deserializeJson(payload, payloadJson) != DeserializationError::Ok)
        {
            // A poison entry would wedge the whole queue forever - drop it, keep draining.
            Serial.println("[Sensor] Buffered file /" + filename + " unreadable - dropping it");
            device.removeBufferedFile(filename);
        }
        else
        {
            ServiceData result = service.requestPost(payload, serviceRequest);
            if (result.eventlog.errorCode == 401)
            {
                // One re-auth retry, same as apiConfig()'s 401 handling.
                Serial.println("[Sensor] Flush /" + filename + " got 401 - re-authenticating once");
                service.apiAuthenticate(deviceConfig, serviceRequest, device);
                result = service.requestPost(payload, serviceRequest);
            }

            if (result.eventlog.error)
            {
                Serial.println("[Sensor] Flush stopped at /" + filename + " - connection lost again, remaining files stay queued");
                return false;
            }

            Serial.println("[Sensor] Flushed /" + filename);
            device.removeBufferedFile(filename); // per-file delete, only after ITS OWN 2xx
        }

        // Each file is a full HTTP round-trip (TLS handshake included) - a deep backlog would outlast the 90s task WDT without feeding it per file.
        esp_task_wdt_reset();

        filename = device.oldestBufferedSensorFile();
    }
    return true;
}

void SensorController::pushSensorData(JsonDocument payload){

    serviceRequest.endpoint = serviceEndpoint.apiSensorDataPost;
    serviceRequest.header.apiId = deviceConfig.apiId;

    // pushEvent() takes its ServiceRequest argument BY VALUE, so mutating its local copy's .endpoint to apiEvent never disturbs serviceRequest.endpoint for the sensor-data POST right after this.
    String safetyEventMessage;
    if (controller.consumeSafetyLimitEvent(safetyEventMessage))
    {
        service.pushEvent(serviceRequest, "SafetyLimitTripped", safetyEventMessage);
    }
    String hardwareFaultMessage;
    if (controller.consumeHardwareFaultEvent(hardwareFaultMessage))
    {
        service.pushEvent(serviceRequest, "I2CFault", hardwareFaultMessage);
    }
    String sensorStaleMessage;
    if (controller.consumeSensorStaleEvent(sensorStaleMessage))
    {
        service.pushEvent(serviceRequest, "SensorStale", sensorStaleMessage);
    }

    // Disk backlog goes first, oldest file first, so the server receives rows in chronological order; a flush that broke off means the connection is down again, so skip the doomed live attempt.
    bool sent = false;
    if (flushBufferedSensorData())
    {
        sent = !service.requestPost(payload, serviceRequest).eventlog.error; // 2xx - requestPost marks 200/201 as success
    }

    if (sent)
    {
        Serial.println("[Sensor] SensorData uploaded, resetting sensorData buffer");
        sensorDataJsonArray = jsonDoc.to<JsonArray>();
        return;
    }

    // Failed send: readings stay in the RAM array, capped - at SENSOR_BUFFER_SPILL_BYTES it spills to one /buffer file and RAM restarts empty instead of growing unbounded.
    size_t pending = measureJson(sensorDataJsonArray);
    Serial.printf("[Sensor] SensorData send failed - %u bytes pending in RAM buffer\n", (unsigned)pending);
    if (pending >= SENSOR_BUFFER_SPILL_BYTES)
    {
        String spill;
        serializeJson(sensorDataJsonArray, spill);
        if (!device.bufferSensorDataToDisk(spill))
        {
            // Covers both a full partition (deliberate data loss) and a write/rename failure - same event either way, StorageController's own log has the specific cause.
            service.pushEvent(serviceRequest, "BufferDiscarded", "Sensor buffer could not be persisted to LittleFS, dropped " + String((unsigned)pending) + " bytes of sensor data");
        }
        sensorDataJsonArray = jsonDoc.to<JsonArray>();
    }
}

void SensorController::buildSensorData(DeviceConfig deviceConfig)
{
    sensorData.battery=NAN;
    sensorData.temperature=NAN;
    sensorData.temperatureSoil=NAN;
    sensorData.humidity=NAN;
    sensorData.moisture=NAN;
    sensorData.light=NAN;
    sensorData.co2=NAN;
    sensorData.tvoc=NAN;
    sensorData.barometer=NAN;
    sensorData.liquidPH=NAN;
    sensorData.rainLevel=NAN;
    sensorData.waterLevel=NAN;
    sensorData.wind=NAN;

    switch (deviceConfig.configSensor.sensorBattery)
    {
    case SensorTypeIds::Max17048:
        sensor_battery_max17048();
        break;
    case SensorTypeIds::AnalogVoltage:
        sensor_analog_voltage();
        break;
    default:
        break;
    }

    switch (deviceConfig.configSensor.sensorTemp)
    {
    case SensorTypeIds::Dht11:
        sensor_DHT11_temp();
        break;
    case SensorTypeIds::Dht22:
        sensor_DHT22_temp();
        break;
    case SensorTypeIds::Bmp180:
        sensor_BMP180_temp();
        break;
    case SensorTypeIds::Bmp280:
        sensor_BMP280_temp();
        break;
    case SensorTypeIds::Bme280:
        sensor_BME280_temp();
        break;
    default:
        break;
    }

    switch (deviceConfig.configSensor.sensorTempSoil)
    {
    case SensorTypeIds::Ds18B20:
        sensor_DS18B20_temp();
        break;

    default:
        break;
    }

    switch (deviceConfig.configSensor.sensorHumid)
    {
    case SensorTypeIds::Dht11:
        sensor_DHT11_humid();
        break;

    default:
        break;
    }
    switch (deviceConfig.configSensor.sensorHumid)
    {
    case SensorTypeIds::Dht22:
        sensor_DHT22_humid();
        break;

    default:
        break;
    }
    switch (deviceConfig.configSensor.sensorHumid)
    {
    case SensorTypeIds::Bme280:
        sensor_BME280_humid();
        break;

    default:
        break;
    }

    switch (deviceConfig.configSensor.sensorMoist)
    {
    case SensorTypeIds::AnalogMoisture:
        sensor_analog_moist();
        break;

    default:
        break;
    }

    switch (deviceConfig.configSensor.sensorLight)
    {
    case SensorTypeIds::Bh1750:
        sensor_BH1750_lux();
        break;

    default:
        break;
    }

    switch (deviceConfig.configSensor.sensorCo2)
    {
    case SensorTypeIds::Ccs811:
        sensor_CCS811_co2();
        break;

    default:
        break;
    }

    switch (deviceConfig.configSensor.sensorTvoc)
    {
    case SensorTypeIds::Ccs811:
        sensor_CCS811_tvoc();
        break;

    default:
        break;
    }

    switch (deviceConfig.configSensor.sensorBarometer)
    {
    case SensorTypeIds::Bmp180:
        sensor_BMP180_pres();
        break;

    default:
        break;
    }
    switch (deviceConfig.configSensor.sensorBarometer)
    {
    case SensorTypeIds::Bmp280:
        sensor_BMP280_pres();
        break;

    default:
        break;
    }
    switch (deviceConfig.configSensor.sensorBarometer)
    {
    case SensorTypeIds::Bme280:
        sensor_BME280_pres();
        break;

    default:
        break;
    }

    switch (deviceConfig.configSensor.sensorPH)
    {
    case 0:
        sensor_liquid_PH();
        break;

    default:
        break;
    }

    switch (deviceConfig.configSensor.sensorWaterLevel)
    {
    case SensorTypeIds::AnalogWaterLevel:
        sensor_analog_waterLevel();
        break;

    default:
        break;
    }

    switch (deviceConfig.configSensor.sensorRainLevel)
    {
    case 0:
        sensor_rainLevel();
        break;

    default:
        break;
    }

    switch (deviceConfig.configSensor.sensorWind)
    {
    case 0:
        sensor_Wind();
        break;

    default:
        break;
    }

    buildSensorDataPayload();

    if(deviceConfig.deviceControllerEnabled){
        controller.initController(sensorData, device.getEpochSeconds());
    }

}