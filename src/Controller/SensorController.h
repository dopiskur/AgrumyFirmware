#ifndef SensorController_H
#define SensorController_H

#include "Arduino.h"
#include "ArduinoJson.h"
#include <Adafruit_Sensor.h> // sensors_event_t, used by the DHT report helpers below

#include "../Model/DeviceModel.h"
#include "../Model/SensorTypeIds.h"

// Forward declarations instead of includes
class DeviceController;
class ServiceController;

class SensorController
{

private:
    SensorData sensorData;

    void sensor_DHT11_temp();          // SensorTypeIds::Dht11
    void sensor_DHT11_humid();          // SensorTypeIds::Dht11
    void sensor_DHT22_temp();          // SensorTypeIds::Dht22
    void sensor_DHT22_humid();          // SensorTypeIds::Dht22
    void sensor_BMP180_temp();         // SensorTypeIds::Bmp180
    void sensor_BMP180_pres();         // SensorTypeIds::Bmp180
    void sensor_BMP280_temp();         // SensorTypeIds::Bmp280
    void sensor_BMP280_pres();         // SensorTypeIds::Bmp280
    void sensor_BME280_temp();         // SensorTypeIds::Bme280
    void sensor_BME280_humid();         // SensorTypeIds::Bme280
    void sensor_BME280_pres();         // SensorTypeIds::Bme280
    void sensor_CCS811_co2();         // SensorTypeIds::Ccs811
    void sensor_CCS811_tvoc();         // SensorTypeIds::Ccs811
    void sensor_DS18B20_temp();        // SensorTypeIds::Ds18B20
    void sensor_BH1750_lux();         // SensorTypeIds::Bh1750

    void sensor_Wind();
    void sensor_analog_voltage(); // SensorTypeIds::AnalogVoltage, VoltageDivider
    void sensor_battery_max17048(); // SensorTypeIds::Max17048
    void sensor_analog_moist();   // SensorTypeIds::AnalogMoisture
    void sensor_liquid_PH(); // unavailable
    void sensor_analog_waterLevel(); // SensorTypeIds::AnalogWaterLevel
    void sensor_rainLevel(); // unavailable

    // Shared print/store tail for the DHT11/DHT22, BMP180/BMP280 and CCS811 co2/tvoc pairs - only the read call differs per library.
    void reportDHTTemperature(sensors_event_t &event, const char *label);
    void reportDHTHumidity(sensors_event_t &event, const char *label);
    void reportSensorInitError(const char *label);
    void reportTemperature(double celsius);
    void reportPressure(double pascals);
    bool tryReadCCS811(); // available()+readData(), false on either miss or heat-up wait

    // Drains /buffer oldest-first, deleting each file only after its own 2xx. Returns false if it broke off mid-queue (connection dropped again).
    bool flushBufferedSensorData();


public:
    void setupSensor();

    // serviceRequest stays a per-module member (unlike deviceConfig): this one always targets the sensor-data/event endpoints.
    ServiceRequest serviceRequest;

    void buildSensorData(DeviceConfig deviceConfig);
    void buildSensorDataPayload();
    void pushSensorData(JsonDocument payload);

    // Roadmap #133's local display reads the last cycle's readings through this instead of duplicating buildSensorData()'s own storage.
    const SensorData &getSensorData() const { return sensorData; }
};

// The one SensorController instance, defined in main.cpp.
extern SensorController sensor;

#endif