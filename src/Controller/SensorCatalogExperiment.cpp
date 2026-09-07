// experiment/sensor-catalog-flash-test: flash-cost measurement only, not a real feature - every sensor below
// gets a minimal begin()+read() so the linker can't dead-strip it, no config/reporting wiring. kc868-a6 only,
// see platformio.ini lib_deps/build_src_filter. Do not merge to master.
#ifdef AGRUMY_KIT_KC868_A6

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>

// --- Temperature ---
#include <Adafruit_MAX31855.h>
#include <Adafruit_MAX31856.h>
#include <Adafruit_MAX31865.h>
#include <Adafruit_MLX90614.h>
#include <Adafruit_MCP9808.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_AM2320.h>
#include <Adafruit_HTU21DF.h>
#include <Adafruit_Si7021.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_SHT4x.h>
#include <Adafruit_SHTC3.h>
#include <Adafruit_BME680.h>
#include <Adafruit_DPS310.h>

// --- CO2 ---
#include <SensirionI2cScd30.h>
#include <SensirionI2cScd4x.h>
#include <MHZ19.h>

// --- Soil moisture ---
#include <I2CSoilMoistureSensor.h>

// --- pH / EC ---
#include <Ezo_i2c.h>
#include <Anyleaf.h>
#include <Adafruit_ADS1X15.h>

// --- Light ---
#include <Adafruit_TSL2561_U.h>
#include <Adafruit_TSL2591.h>
#include <Adafruit_SI1145.h>
#include <Adafruit_LTR390.h>
#include <Adafruit_VEML7700.h>
#include <Adafruit_AS7341.h>

// --- Flow / Mass ---
#include <HX711.h>

// Dedicated SPI chip-select pins, unused GPIOs picked only so each object constructs - never wired to real hardware.
static const int8_t CS_MAX31855 = 32;
static const int8_t CS_MAX31856 = 33;
static const int8_t CS_MAX31865 = 25;

static Adafruit_MAX31855 max31855(CS_MAX31855);
static Adafruit_MAX31856 max31856(CS_MAX31856);
static Adafruit_MAX31865 max31865(CS_MAX31865);
static Adafruit_MLX90614 mlx90614;
static Adafruit_MCP9808 mcp9808;
static Adafruit_AHTX0 aht;
static Adafruit_AM2320 am2320;
static Adafruit_HTU21DF htu21df;
static Adafruit_Si7021 si7021;
static Adafruit_SHT31 sht31;
static Adafruit_SHT4x sht4x;
static Adafruit_SHTC3 shtc3;
static Adafruit_BME680 bme680;
static Adafruit_DPS310 dps310;

static SensirionI2cScd30 scd30;
static SensirionI2cScd4x scd4x;
static MHZ19 mhz19(&Serial2);

static I2CSoilMoistureSensor i2cSoilMoisture;

static Ezo_board ezoPh(99, "PH");
static PhSensor anyleafPh;
static Adafruit_ADS1115 ads1115;

static Adafruit_TSL2561_Unified tsl2561(TSL2561_ADDR_FLOAT, 12345);
static Adafruit_TSL2591 tsl2591(12346);
static Adafruit_SI1145 si1145;
static Adafruit_LTR390 ltr390;
static Adafruit_VEML7700 veml7700;
static Adafruit_AS7341 as7341;

static const uint8_t HX711_DOUT = 34;
static const uint8_t HX711_SCK = 35;
static HX711 hx711Scale;

// One begin()+read() per driver, values discarded - only exists to make the linker keep every library in the image.
void runSensorCatalogExperiment()
{
    Serial.println("[SensorCatalogExperiment] begin");

    max31855.begin();
    Serial.println(max31855.readCelsius());

    max31856.begin();
    max31856.setThermocoupleType(MAX31856_TCTYPE_K);
    Serial.println(max31856.readThermocoupleTemperature());

    max31865.begin(MAX31865_2WIRE);
    Serial.println(max31865.temperature(100.0, 430.0));

    mlx90614.begin();
    Serial.println(mlx90614.readObjectTempC());

    mcp9808.begin(0x18);
    Serial.println(mcp9808.readTempC());

    aht.begin();
    sensors_event_t ahtHumidity, ahtTemp;
    aht.getEvent(&ahtHumidity, &ahtTemp);
    Serial.println(ahtTemp.temperature);

    am2320.begin();
    Serial.println(am2320.readTemperature());

    htu21df.begin();
    Serial.println(htu21df.readTemperature());

    si7021.begin();
    Serial.println(si7021.readTemperature());

    sht31.begin(0x44);
    Serial.println(sht31.readTemperature());

    sht4x.begin();
    sensors_event_t sht4Humidity, sht4Temp;
    sht4x.getEvent(&sht4Humidity, &sht4Temp);
    Serial.println(sht4Temp.temperature);

    shtc3.begin();
    sensors_event_t shtc3Humidity, shtc3Temp;
    shtc3.getEvent(&shtc3Humidity, &shtc3Temp);
    Serial.println(shtc3Temp.temperature);

    bme680.begin(0x77);
    bme680.performReading();
    Serial.println(bme680.temperature);

    dps310.begin_I2C();
    sensors_event_t dpsTemp, dpsPressure;
    dps310.getEvents(&dpsTemp, &dpsPressure);
    Serial.println(dpsPressure.pressure);

    scd30.begin(Wire, SCD30_I2C_ADDR_61);
    scd30.startPeriodicMeasurement(0);
    float scd30Co2, scd30Temp, scd30Hum;
    scd30.readMeasurementData(scd30Co2, scd30Temp, scd30Hum);
    Serial.println(scd30Co2);

    scd4x.begin(Wire, SCD41_I2C_ADDR_62);
    scd4x.startPeriodicMeasurement();
    uint16_t scd4xCo2;
    float scd4xTemp, scd4xHum;
    scd4x.readMeasurement(scd4xCo2, scd4xTemp, scd4xHum);
    Serial.println(scd4xCo2);

    mhz19.setAutoCalibration(false);
    mhz19.retrieveData();
    Serial.println(mhz19.getCO2());

    i2cSoilMoisture.begin();
    Serial.println(i2cSoilMoisture.getCapacitance());

    ezoPh.send_read_cmd();
    Serial.println(ezoPh.get_last_received_reading());

    Serial.println(anyleafPh.read());

    ads1115.begin();
    Serial.println(ads1115.readADC_SingleEnded(0));

    tsl2561.begin();
    sensors_event_t tslEvent;
    tsl2561.getEvent(&tslEvent);
    Serial.println(tslEvent.light);

    tsl2591.begin();
    Serial.println(tsl2591.getLuminosity(TSL2591_VISIBLE));

    si1145.begin();
    Serial.println(si1145.readVisible());

    ltr390.begin();
    Serial.println(ltr390.readUVS());

    veml7700.begin();
    Serial.println(veml7700.readLux());

    as7341.begin();
    as7341.readAllChannels();
    Serial.println(as7341.getChannel(AS7341_CHANNEL_CLEAR));

    hx711Scale.begin(HX711_DOUT, HX711_SCK);
    Serial.println(hx711Scale.read());

    Serial.println("[SensorCatalogExperiment] end");
}

#endif // AGRUMY_KIT_KC868_A6
