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

    // Roadmap #416 - extended catalog, real wiring of #415's flash-cost-only drivers.
    void sensor_MAX31855_temp();   // SensorTypeIds::Max31855
    void sensor_MAX31856_temp();   // SensorTypeIds::Max31856
    void sensor_MAX31865_temp();   // SensorTypeIds::Max31865
    void sensor_MLX90614_temp();   // SensorTypeIds::Mlx90614
    void sensor_MCP9808_temp();    // SensorTypeIds::Mcp9808
    void sensor_AHT_temp();        // SensorTypeIds::Aht
    void sensor_AHT_humid();       // SensorTypeIds::Aht
    void sensor_AM2320_temp();     // SensorTypeIds::Am2320
    void sensor_AM2320_humid();    // SensorTypeIds::Am2320
    void sensor_HTU21DF_temp();    // SensorTypeIds::Htu21Df
    void sensor_HTU21DF_humid();   // SensorTypeIds::Htu21Df
    void sensor_SI7021_temp();     // SensorTypeIds::Si7021
    void sensor_SI7021_humid();    // SensorTypeIds::Si7021
    void sensor_SHT31_temp();      // SensorTypeIds::Sht31
    void sensor_SHT31_humid();     // SensorTypeIds::Sht31
    void sensor_SHT4x_temp();      // SensorTypeIds::Sht4x
    void sensor_SHT4x_humid();     // SensorTypeIds::Sht4x
    void sensor_SHTC3_temp();      // SensorTypeIds::Shtc3
    void sensor_SHTC3_humid();     // SensorTypeIds::Shtc3
    void sensor_BME680_temp();     // SensorTypeIds::Bme680
    void sensor_BME680_humid();    // SensorTypeIds::Bme680
    void sensor_BME680_pres();     // SensorTypeIds::Bme680
    void sensor_DPS310_temp();     // SensorTypeIds::Dps310
    void sensor_DPS310_pres();     // SensorTypeIds::Dps310
    void sensor_SCD30_co2();       // SensorTypeIds::Scd30
    void sensor_SCD4x_co2();       // SensorTypeIds::Scd4x
    void sensor_MHZ19_co2();       // SensorTypeIds::Mhz19
    void sensor_Chirp_moist();     // SensorTypeIds::ChirpSoilMoisture
    void sensor_EzoPH_ph();        // SensorTypeIds::EzoPh
    void sensor_AnyleafPH_ph();    // SensorTypeIds::AnyleafPh
    void sensor_ADS1115_ec();      // SensorTypeIds::Ads1115Ec
    void sensor_TSL2561_lux();     // SensorTypeIds::Tsl2561
    void sensor_TSL2591_lux();     // SensorTypeIds::Tsl2591
    void sensor_SI1145_lux();      // SensorTypeIds::Si1145
    void sensor_LTR390_lux();      // SensorTypeIds::Ltr390
    void sensor_VEML7700_lux();    // SensorTypeIds::Veml7700
    void sensor_AS7341_lux();      // SensorTypeIds::As7341
    void sensor_HX711_weight();    // SensorTypeIds::Hx711

    // Shared print/store tail for the #416 temp/humidity/pressure sensors above - same role as reportTemperature()/reportPressure(), separate so an unavailable reading (isnan) is never stored as a real 0.
    void reportHumidity(double percent);
    void reportEc(double milliSiemensPerCm);
    void reportWeight(double units);

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