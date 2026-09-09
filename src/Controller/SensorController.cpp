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

// Extended sensor catalog, real wiring of the earlier flash-cost-only experiment's drivers.
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
#include <SensirionI2cScd30.h>
#include <SensirionI2cScd4x.h>
#include <MHZ19.h>
#include <I2CSoilMoistureSensor.h>
#include <Ezo_i2c.h>
#include <Anyleaf.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_TSL2561_U.h>
#include <Adafruit_TSL2591.h>
#include <Adafruit_SI1145.h>
#include <Adafruit_LTR390.h>
#include <Adafruit_VEML7700.h>
#include <Adafruit_AS7341.h>
#include <HX711.h>

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

// SPI chip-select pins come from deviceConfig at setupSensor() time (unknown at static-init, same reason oneWireTempSoil/ds18b20 above are pointers), so the three SPI sensor objects are too.
static Adafruit_MAX31855 *max31855;
static Adafruit_MAX31856 *max31856;
static Adafruit_MAX31865 *max31865;
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
static MHZ19 mhz19(&Serial2); // Stream* is a constructor argument, not a begin() parameter - same reason DS18B20's OneWire is constructed at setupSensor() time, but Serial2 is a fixed global so this can happen at static-init instead
static I2CSoilMoistureSensor chirpSoilMoisture;
static Ezo_board ezoPh(99, "PH");
static PhSensor anyleafPh;
static Adafruit_ADS1115 ads1115;
static Adafruit_TSL2561_Unified tsl2561(TSL2561_ADDR_FLOAT, 12345);
static Adafruit_TSL2591 tsl2591(12346);
static Adafruit_SI1145 si1145;
static Adafruit_LTR390 ltr390;
static Adafruit_VEML7700 veml7700;
static Adafruit_AS7341 as7341;
static HX711 hx711Scale;

static bool max31855status, max31856status, max31865status, mlx90614status, mcp9808status;
static bool ahtStatus, am2320Status, htu21dfStatus, si7021Status, sht31Status, sht4xStatus, shtc3Status, bme680Status, dps310Status;
static bool scd30Status, scd4xStatus, mhz19Status;
static bool chirpStatus;
static bool ezoPhStatus, anyleafPhStatus, ads1115Status;
static bool tsl2561Status, tsl2591Status, si1145Status, ltr390Status, veml7700Status, as7341Status;
static bool hx711Status;


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

    // Extended sensor catalog, same "only probe what this device's config actually selects" gating as above.
    if (deviceConfig.configSensor.sensorTemp == SensorTypeIds::Max31855)
    {
        max31855 = new Adafruit_MAX31855(deviceConfig.configPin.MAX31855_CS);
        max31855status = max31855->begin();
    }
    if (deviceConfig.configSensor.sensorTemp == SensorTypeIds::Max31856)
    {
        max31856 = new Adafruit_MAX31856(deviceConfig.configPin.MAX31856_CS);
        max31856status = max31856->begin();
        if (max31856status) max31856->setThermocoupleType(MAX31856_TCTYPE_K);
    }
    if (deviceConfig.configSensor.sensorTemp == SensorTypeIds::Max31865)
    {
        max31865 = new Adafruit_MAX31865(deviceConfig.configPin.MAX31865_CS);
        max31865status = max31865->begin(MAX31865_2WIRE);
    }
    if (deviceConfig.configSensor.sensorTemp == SensorTypeIds::Mlx90614)
    {
        mlx90614status = mlx90614.begin();
    }
    if (deviceConfig.configSensor.sensorTemp == SensorTypeIds::Mcp9808)
    {
        mcp9808status = mcp9808.begin(0x18);
    }
    if (deviceConfig.configSensor.sensorTemp == SensorTypeIds::Aht || deviceConfig.configSensor.sensorHumid == SensorTypeIds::Aht)
    {
        ahtStatus = aht.begin();
    }
    if (deviceConfig.configSensor.sensorTemp == SensorTypeIds::Am2320 || deviceConfig.configSensor.sensorHumid == SensorTypeIds::Am2320)
    {
        am2320Status = am2320.begin();
    }
    if (deviceConfig.configSensor.sensorTemp == SensorTypeIds::Htu21Df || deviceConfig.configSensor.sensorHumid == SensorTypeIds::Htu21Df)
    {
        htu21dfStatus = htu21df.begin();
    }
    if (deviceConfig.configSensor.sensorTemp == SensorTypeIds::Si7021 || deviceConfig.configSensor.sensorHumid == SensorTypeIds::Si7021)
    {
        si7021Status = si7021.begin();
    }
    if (deviceConfig.configSensor.sensorTemp == SensorTypeIds::Sht31 || deviceConfig.configSensor.sensorHumid == SensorTypeIds::Sht31)
    {
        sht31Status = sht31.begin(0x44);
    }
    if (deviceConfig.configSensor.sensorTemp == SensorTypeIds::Sht4x || deviceConfig.configSensor.sensorHumid == SensorTypeIds::Sht4x)
    {
        sht4xStatus = sht4x.begin();
    }
    if (deviceConfig.configSensor.sensorTemp == SensorTypeIds::Shtc3 || deviceConfig.configSensor.sensorHumid == SensorTypeIds::Shtc3)
    {
        shtc3Status = shtc3.begin();
    }
    if (deviceConfig.configSensor.sensorTemp == SensorTypeIds::Bme680 || deviceConfig.configSensor.sensorHumid == SensorTypeIds::Bme680 || deviceConfig.configSensor.sensorBarometer == SensorTypeIds::Bme680)
    {
        bme680Status = bme680.begin(0x77);
    }
    if (deviceConfig.configSensor.sensorTemp == SensorTypeIds::Dps310 || deviceConfig.configSensor.sensorBarometer == SensorTypeIds::Dps310)
    {
        dps310Status = dps310.begin_I2C();
    }
    if (deviceConfig.configSensor.sensorCo2 == SensorTypeIds::Scd30)
    {
        scd30.begin(Wire, SCD30_I2C_ADDR_61);
        scd30Status = scd30.startPeriodicMeasurement(0) == 0;
    }
    if (deviceConfig.configSensor.sensorCo2 == SensorTypeIds::Scd4x)
    {
        scd4x.begin(Wire, SCD41_I2C_ADDR_62);
        scd4xStatus = scd4x.startPeriodicMeasurement() == 0;
    }
    if (deviceConfig.configSensor.sensorCo2 == SensorTypeIds::Mhz19)
    {
        // No dedicated config pins for this UART (see ConfigPin) - default ESP32 hardware UART2 pins (RX16/TX17).
        Serial2.begin(9600);
        mhz19.setAutoCalibration(false);
        mhz19Status = true;
    }
    if (deviceConfig.configSensor.sensorMoist == SensorTypeIds::ChirpSoilMoisture)
    {
        chirpSoilMoisture.begin();
        chirpStatus = true; // library exposes no begin() success signal - matches its own upstream examples
    }
    if (deviceConfig.configSensor.sensorPH == SensorTypeIds::EzoPh)
    {
        ezoPhStatus = true; // Ezo_board has no begin()/init status - it only starts answering once send_read_cmd() actually gets a reply
    }
    if (deviceConfig.configSensor.sensorPH == SensorTypeIds::AnyleafPh)
    {
        anyleafPhStatus = true;
    }
    if (deviceConfig.configSensor.sensorEc == SensorTypeIds::Ads1115Ec)
    {
        ads1115Status = ads1115.begin();
    }
    if (deviceConfig.configSensor.sensorLight == SensorTypeIds::Tsl2561)
    {
        tsl2561Status = tsl2561.begin();
    }
    if (deviceConfig.configSensor.sensorLight == SensorTypeIds::Tsl2591)
    {
        tsl2591Status = tsl2591.begin();
    }
    if (deviceConfig.configSensor.sensorLight == SensorTypeIds::Si1145)
    {
        si1145Status = si1145.begin();
    }
    if (deviceConfig.configSensor.sensorLight == SensorTypeIds::Ltr390)
    {
        ltr390Status = ltr390.begin();
    }
    if (deviceConfig.configSensor.sensorLight == SensorTypeIds::Veml7700)
    {
        veml7700Status = veml7700.begin();
    }
    if (deviceConfig.configSensor.sensorLight == SensorTypeIds::As7341)
    {
        as7341Status = as7341.begin();
    }
    if (deviceConfig.configSensor.sensorWeight == SensorTypeIds::Hx711)
    {
        hx711Scale.begin(deviceConfig.configPin.HX711_DOUT, deviceConfig.configPin.HX711_SCK);
        hx711Status = hx711Scale.wait_ready_timeout(1000);
        if (hx711Status)
        {
            hx711Scale.set_scale(deviceConfig.configSensor.weightCalibrationFactor);
            hx711Scale.set_offset(deviceConfig.configSensor.weightTareOffset);
        }
    }

    delay(5000);
}

namespace
{
    struct I2CCandidate
    {
        uint8_t address;
        int sensorTypeId;
    };

    // Address as this codebase's own drivers actually probe it (fixed or default) - not every alt address a chip's datasheet allows, only what setupSensor() above already begins with.
    const I2CCandidate i2cCandidates[] = {
        {0x10, SensorTypeIds::Veml7700},
        {0x18, SensorTypeIds::Mcp9808},
        {0x20, SensorTypeIds::ChirpSoilMoisture},
        {0x23, SensorTypeIds::Bh1750},
        {0x29, SensorTypeIds::Tsl2591},
        {0x36, SensorTypeIds::Max17048},
        {0x38, SensorTypeIds::Aht},
        {0x39, SensorTypeIds::Tsl2561},
        {0x39, SensorTypeIds::As7341},
        {0x40, SensorTypeIds::Htu21Df},
        {0x40, SensorTypeIds::Si7021},
        {0x44, SensorTypeIds::Sht31},
        {0x44, SensorTypeIds::Sht4x},
        {0x48, SensorTypeIds::Ads1115Ec},
        {0x53, SensorTypeIds::Ltr390},
        {0x5A, SensorTypeIds::Ccs811},
        {0x5A, SensorTypeIds::Mlx90614},
        {0x5C, SensorTypeIds::Am2320},
        {0x60, SensorTypeIds::Si1145},
        {0x61, SensorTypeIds::Scd30},
        {0x62, SensorTypeIds::Scd4x},
        {0x70, SensorTypeIds::Shtc3},
        {0x76, SensorTypeIds::Bmp280},
        {0x76, SensorTypeIds::Bme280},
        {0x76, SensorTypeIds::Bme680},
        {0x76, SensorTypeIds::Dps310},
        {0x77, SensorTypeIds::Bmp180},
        {0x77, SensorTypeIds::Bme680},
        {0x77, SensorTypeIds::Dps310},
    };

    // Only called for a candidate whose table address already matches what's on the bus - narrows which begin() overload actually confirms the chip, not just guesses at it.
    bool probeI2CCandidate(int sensorTypeId, uint8_t addr)
    {
        switch (sensorTypeId)
        {
        case SensorTypeIds::Bmp280: return bmp280.begin(addr);
        case SensorTypeIds::Bme280: return bme280.begin(addr);
        case SensorTypeIds::Bme680: return bme680.begin(addr);
        case SensorTypeIds::Dps310: return dps310.begin_I2C(addr);
        case SensorTypeIds::Bmp180: return bmp180.begin(); // fixed 0x77, begin() takes an oversampling mode, not an address
        case SensorTypeIds::Ccs811: return ccs811.begin(addr);
        case SensorTypeIds::Mlx90614: return mlx90614.begin(addr);
        case SensorTypeIds::Mcp9808: return mcp9808.begin(addr);
        case SensorTypeIds::Aht: return aht.begin(); // fixed 0x38, no address overload
        case SensorTypeIds::Am2320: return am2320.begin(); // fixed 0x5C
        case SensorTypeIds::Htu21Df: return htu21df.begin(); // fixed 0x40
        case SensorTypeIds::Si7021: return si7021.begin(); // fixed 0x40, same address as Htu21Df above
        case SensorTypeIds::Sht31: return sht31.begin(addr);
        case SensorTypeIds::Sht4x: return sht4x.begin(); // fixed 0x44
        case SensorTypeIds::Shtc3: return shtc3.begin(); // fixed 0x70
        case SensorTypeIds::Scd30: { scd30.begin(Wire, addr); return scd30.startPeriodicMeasurement(0) == 0; }
        case SensorTypeIds::Scd4x: { scd4x.begin(Wire, addr); return scd4x.startPeriodicMeasurement() == 0; }
        case SensorTypeIds::ChirpSoilMoisture: chirpSoilMoisture.begin(); return true; // fixed at the address its constructor was given, no init-success signal
        case SensorTypeIds::Ads1115Ec: return ads1115.begin(addr);
        case SensorTypeIds::Tsl2561: return tsl2561.begin(); // address fixed at construction (TSL2561_ADDR_FLOAT)
        case SensorTypeIds::Tsl2591: return tsl2591.begin(addr);
        case SensorTypeIds::Si1145: return si1145.begin(addr);
        case SensorTypeIds::Ltr390: return ltr390.begin(); // fixed 0x53
        case SensorTypeIds::Veml7700: return veml7700.begin(); // fixed 0x10
        case SensorTypeIds::As7341: return as7341.begin(addr);
        case SensorTypeIds::Max17048: return maxlipo.begin(); // fixed 0x36
        default: return false;
        }
    }
}

namespace
{
    struct SensorReadEntry
    {
        SensorMetricSlot slot;
        int sensorTypeId;
        void (SensorController::*read)();
    };
}

// Table lives inside this member function (not file-scope) - a member-function-pointer literal to a private sensor_* method is only accessible from within a member function of this class, not a free function elsewhere in the file.
void SensorController::dispatchSensorRead(SensorMetricSlot slot, int sensorTypeId)
{
    // One row per (slot, sensorTypeId) - replaces the ~14 separate switch-per-config-slot blocks buildSensorData() used to have.
    static const SensorReadEntry sensorReadTable[] = {
        {SensorMetricSlot::Battery, SensorTypeIds::Max17048, &SensorController::sensor_battery_max17048},
        {SensorMetricSlot::Battery, SensorTypeIds::AnalogVoltage, &SensorController::sensor_analog_voltage},

        {SensorMetricSlot::Temp, SensorTypeIds::Dht11, &SensorController::sensor_DHT11_temp},
        {SensorMetricSlot::Temp, SensorTypeIds::Dht22, &SensorController::sensor_DHT22_temp},
        {SensorMetricSlot::Temp, SensorTypeIds::Bmp180, &SensorController::sensor_BMP180_temp},
        {SensorMetricSlot::Temp, SensorTypeIds::Bmp280, &SensorController::sensor_BMP280_temp},
        {SensorMetricSlot::Temp, SensorTypeIds::Bme280, &SensorController::sensor_BME280_temp},
        {SensorMetricSlot::Temp, SensorTypeIds::Max31855, &SensorController::sensor_MAX31855_temp},
        {SensorMetricSlot::Temp, SensorTypeIds::Max31856, &SensorController::sensor_MAX31856_temp},
        {SensorMetricSlot::Temp, SensorTypeIds::Max31865, &SensorController::sensor_MAX31865_temp},
        {SensorMetricSlot::Temp, SensorTypeIds::Mlx90614, &SensorController::sensor_MLX90614_temp},
        {SensorMetricSlot::Temp, SensorTypeIds::Mcp9808, &SensorController::sensor_MCP9808_temp},
        {SensorMetricSlot::Temp, SensorTypeIds::Aht, &SensorController::sensor_AHT_temp},
        {SensorMetricSlot::Temp, SensorTypeIds::Am2320, &SensorController::sensor_AM2320_temp},
        {SensorMetricSlot::Temp, SensorTypeIds::Htu21Df, &SensorController::sensor_HTU21DF_temp},
        {SensorMetricSlot::Temp, SensorTypeIds::Si7021, &SensorController::sensor_SI7021_temp},
        {SensorMetricSlot::Temp, SensorTypeIds::Sht31, &SensorController::sensor_SHT31_temp},
        {SensorMetricSlot::Temp, SensorTypeIds::Sht4x, &SensorController::sensor_SHT4x_temp},
        {SensorMetricSlot::Temp, SensorTypeIds::Shtc3, &SensorController::sensor_SHTC3_temp},
        {SensorMetricSlot::Temp, SensorTypeIds::Bme680, &SensorController::sensor_BME680_temp},
        {SensorMetricSlot::Temp, SensorTypeIds::Dps310, &SensorController::sensor_DPS310_temp},

        {SensorMetricSlot::TempSoil, SensorTypeIds::Ds18B20, &SensorController::sensor_DS18B20_temp},

        {SensorMetricSlot::Humid, SensorTypeIds::Dht11, &SensorController::sensor_DHT11_humid},
        {SensorMetricSlot::Humid, SensorTypeIds::Dht22, &SensorController::sensor_DHT22_humid},
        {SensorMetricSlot::Humid, SensorTypeIds::Bme280, &SensorController::sensor_BME280_humid},
        {SensorMetricSlot::Humid, SensorTypeIds::Aht, &SensorController::sensor_AHT_humid},
        {SensorMetricSlot::Humid, SensorTypeIds::Am2320, &SensorController::sensor_AM2320_humid},
        {SensorMetricSlot::Humid, SensorTypeIds::Htu21Df, &SensorController::sensor_HTU21DF_humid},
        {SensorMetricSlot::Humid, SensorTypeIds::Si7021, &SensorController::sensor_SI7021_humid},
        {SensorMetricSlot::Humid, SensorTypeIds::Sht31, &SensorController::sensor_SHT31_humid},
        {SensorMetricSlot::Humid, SensorTypeIds::Sht4x, &SensorController::sensor_SHT4x_humid},
        {SensorMetricSlot::Humid, SensorTypeIds::Shtc3, &SensorController::sensor_SHTC3_humid},
        {SensorMetricSlot::Humid, SensorTypeIds::Bme680, &SensorController::sensor_BME680_humid},

        {SensorMetricSlot::Moist, SensorTypeIds::AnalogMoisture, &SensorController::sensor_analog_moist},
        {SensorMetricSlot::Moist, SensorTypeIds::ChirpSoilMoisture, &SensorController::sensor_Chirp_moist},

        {SensorMetricSlot::Light, SensorTypeIds::Bh1750, &SensorController::sensor_BH1750_lux},
        {SensorMetricSlot::Light, SensorTypeIds::Tsl2561, &SensorController::sensor_TSL2561_lux},
        {SensorMetricSlot::Light, SensorTypeIds::Tsl2591, &SensorController::sensor_TSL2591_lux},
        {SensorMetricSlot::Light, SensorTypeIds::Si1145, &SensorController::sensor_SI1145_lux},
        {SensorMetricSlot::Light, SensorTypeIds::Ltr390, &SensorController::sensor_LTR390_lux},
        {SensorMetricSlot::Light, SensorTypeIds::Veml7700, &SensorController::sensor_VEML7700_lux},
        {SensorMetricSlot::Light, SensorTypeIds::As7341, &SensorController::sensor_AS7341_lux},

        {SensorMetricSlot::Co2, SensorTypeIds::Ccs811, &SensorController::sensor_CCS811_co2},
        {SensorMetricSlot::Co2, SensorTypeIds::Scd30, &SensorController::sensor_SCD30_co2},
        {SensorMetricSlot::Co2, SensorTypeIds::Scd4x, &SensorController::sensor_SCD4x_co2},
        {SensorMetricSlot::Co2, SensorTypeIds::Mhz19, &SensorController::sensor_MHZ19_co2},

        {SensorMetricSlot::Tvoc, SensorTypeIds::Ccs811, &SensorController::sensor_CCS811_tvoc},

        {SensorMetricSlot::Barometer, SensorTypeIds::Bmp180, &SensorController::sensor_BMP180_pres},
        {SensorMetricSlot::Barometer, SensorTypeIds::Bmp280, &SensorController::sensor_BMP280_pres},
        {SensorMetricSlot::Barometer, SensorTypeIds::Bme280, &SensorController::sensor_BME280_pres},
        {SensorMetricSlot::Barometer, SensorTypeIds::Bme680, &SensorController::sensor_BME680_pres},
        {SensorMetricSlot::Barometer, SensorTypeIds::Dps310, &SensorController::sensor_DPS310_pres},

        {SensorMetricSlot::Ph, SensorTypeIds::EzoPh, &SensorController::sensor_EzoPH_ph},
        {SensorMetricSlot::Ph, SensorTypeIds::AnyleafPh, &SensorController::sensor_AnyleafPH_ph},

        {SensorMetricSlot::Ec, SensorTypeIds::Ads1115Ec, &SensorController::sensor_ADS1115_ec},

        {SensorMetricSlot::Weight, SensorTypeIds::Hx711, &SensorController::sensor_HX711_weight},

        {SensorMetricSlot::WaterLevel, SensorTypeIds::AnalogWaterLevel, &SensorController::sensor_analog_waterLevel},
    };

    for (const auto &entry : sensorReadTable)
    {
        if (entry.slot == slot && entry.sensorTypeId == sensorTypeId)
        {
            (this->*entry.read)();
            return;
        }
    }
}

String SensorController::detectSensors()
{
    JsonDocument doc;
    JsonArray addresses = doc["Addresses"].to<JsonArray>();

    for (uint16_t addr = 0x08; addr <= 0x77; addr++)
    {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() != 0)
        {
            continue; // nothing answered at this address
        }

        JsonArray matchedCandidates;
        for (const auto &candidate : i2cCandidates)
        {
            if (candidate.address != addr || !probeI2CCandidate(candidate.sensorTypeId, addr))
            {
                continue;
            }
            if (matchedCandidates.isNull())
            {
                JsonObject entry = addresses.add<JsonObject>();
                entry["Address"] = addr;
                matchedCandidates = entry["Candidates"].to<JsonArray>();
            }
            matchedCandidates.add(candidate.sensorTypeId);
        }
    }

    String result;
    serializeJson(doc, result);
    return result;
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

// Same store-and-log tail as reportTemperature()/reportPressure(), for the extended catalog's new sensor drivers below.
void SensorController::reportHumidity(double percent)
{
    Serial.print("Humidity = ");
    Serial.print(percent);
    Serial.println(" %");
    sensorData.humidity = percent;
}

void SensorController::reportEc(double milliSiemensPerCm)
{
    Serial.print("EC = ");
    Serial.print(milliSiemensPerCm);
    Serial.println(" mS/cm");
    sensorData.ec = milliSiemensPerCm;
}

void SensorController::reportWeight(double units)
{
    Serial.print("Weight = ");
    Serial.println(units);
    sensorData.weight = units;
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

// Extended catalog read functions, one per (chip, quantity) pair, same convention as BME280/BMP280 above.
void SensorController::sensor_MAX31855_temp()
{
    Serial.println("[Sensor] MAX31855 temperature");
    if (!max31855status) { reportSensorInitError("MAX31855"); return; }
    double celsius = max31855->readCelsius();
    if (isnan(celsius)) { reportSensorInitError("MAX31855"); return; }
    reportTemperature(celsius);
}

void SensorController::sensor_MAX31856_temp()
{
    Serial.println("[Sensor] MAX31856 temperature");
    if (!max31856status) { reportSensorInitError("MAX31856"); return; }
    reportTemperature(max31856->readThermocoupleTemperature());
}

void SensorController::sensor_MAX31865_temp()
{
    Serial.println("[Sensor] MAX31865 temperature");
    if (!max31865status) { reportSensorInitError("MAX31865"); return; }
    // PT100/430ohm reference - Adafruit board default, not verified against a real install (see ConfigPin comment).
    reportTemperature(max31865->temperature(100.0, 430.0));
}

void SensorController::sensor_MLX90614_temp()
{
    Serial.println("[Sensor] MLX90614 temperature");
    if (!mlx90614status) { reportSensorInitError("MLX90614"); return; }
    reportTemperature(mlx90614.readObjectTempC());
}

void SensorController::sensor_MCP9808_temp()
{
    Serial.println("[Sensor] MCP9808 temperature");
    if (!mcp9808status) { reportSensorInitError("MCP9808"); return; }
    reportTemperature(mcp9808.readTempC());
}

void SensorController::sensor_AHT_temp()
{
    Serial.println("[Sensor] AHT temperature");
    if (!ahtStatus) { reportSensorInitError("AHT"); return; }
    sensors_event_t humidityEvent, tempEvent;
    aht.getEvent(&humidityEvent, &tempEvent);
    reportTemperature(tempEvent.temperature);
}
void SensorController::sensor_AHT_humid()
{
    Serial.println("[Sensor] AHT humidity");
    if (!ahtStatus) { reportSensorInitError("AHT"); return; }
    sensors_event_t humidityEvent, tempEvent;
    aht.getEvent(&humidityEvent, &tempEvent);
    reportHumidity(humidityEvent.relative_humidity);
}

void SensorController::sensor_AM2320_temp()
{
    Serial.println("[Sensor] AM2320 temperature");
    if (!am2320Status) { reportSensorInitError("AM2320"); return; }
    reportTemperature(am2320.readTemperature());
}
void SensorController::sensor_AM2320_humid()
{
    Serial.println("[Sensor] AM2320 humidity");
    if (!am2320Status) { reportSensorInitError("AM2320"); return; }
    reportHumidity(am2320.readHumidity());
}

void SensorController::sensor_HTU21DF_temp()
{
    Serial.println("[Sensor] HTU21DF temperature");
    if (!htu21dfStatus) { reportSensorInitError("HTU21DF"); return; }
    reportTemperature(htu21df.readTemperature());
}
void SensorController::sensor_HTU21DF_humid()
{
    Serial.println("[Sensor] HTU21DF humidity");
    if (!htu21dfStatus) { reportSensorInitError("HTU21DF"); return; }
    reportHumidity(htu21df.readHumidity());
}

void SensorController::sensor_SI7021_temp()
{
    Serial.println("[Sensor] Si7021 temperature");
    if (!si7021Status) { reportSensorInitError("Si7021"); return; }
    reportTemperature(si7021.readTemperature());
}
void SensorController::sensor_SI7021_humid()
{
    Serial.println("[Sensor] Si7021 humidity");
    if (!si7021Status) { reportSensorInitError("Si7021"); return; }
    reportHumidity(si7021.readHumidity());
}

void SensorController::sensor_SHT31_temp()
{
    Serial.println("[Sensor] SHT31 temperature");
    if (!sht31Status) { reportSensorInitError("SHT31"); return; }
    reportTemperature(sht31.readTemperature());
}
void SensorController::sensor_SHT31_humid()
{
    Serial.println("[Sensor] SHT31 humidity");
    if (!sht31Status) { reportSensorInitError("SHT31"); return; }
    reportHumidity(sht31.readHumidity());
}

void SensorController::sensor_SHT4x_temp()
{
    Serial.println("[Sensor] SHT4x temperature");
    if (!sht4xStatus) { reportSensorInitError("SHT4x"); return; }
    sensors_event_t humidityEvent, tempEvent;
    sht4x.getEvent(&humidityEvent, &tempEvent);
    reportTemperature(tempEvent.temperature);
}
void SensorController::sensor_SHT4x_humid()
{
    Serial.println("[Sensor] SHT4x humidity");
    if (!sht4xStatus) { reportSensorInitError("SHT4x"); return; }
    sensors_event_t humidityEvent, tempEvent;
    sht4x.getEvent(&humidityEvent, &tempEvent);
    reportHumidity(humidityEvent.relative_humidity);
}

void SensorController::sensor_SHTC3_temp()
{
    Serial.println("[Sensor] SHTC3 temperature");
    if (!shtc3Status) { reportSensorInitError("SHTC3"); return; }
    sensors_event_t humidityEvent, tempEvent;
    shtc3.getEvent(&humidityEvent, &tempEvent);
    reportTemperature(tempEvent.temperature);
}
void SensorController::sensor_SHTC3_humid()
{
    Serial.println("[Sensor] SHTC3 humidity");
    if (!shtc3Status) { reportSensorInitError("SHTC3"); return; }
    sensors_event_t humidityEvent, tempEvent;
    shtc3.getEvent(&humidityEvent, &tempEvent);
    reportHumidity(humidityEvent.relative_humidity);
}

void SensorController::sensor_BME680_temp()
{
    Serial.println("[Sensor] BME680 temperature");
    if (!bme680Status || !bme680.performReading()) { reportSensorInitError("BME680"); return; }
    reportTemperature(bme680.temperature);
}
void SensorController::sensor_BME680_humid()
{
    Serial.println("[Sensor] BME680 humidity");
    if (!bme680Status || !bme680.performReading()) { reportSensorInitError("BME680"); return; }
    reportHumidity(bme680.humidity);
}
void SensorController::sensor_BME680_pres()
{
    Serial.println("[Sensor] BME680 pressure");
    if (!bme680Status || !bme680.performReading()) { reportSensorInitError("BME680"); return; }
    reportPressure(bme680.pressure);
}

void SensorController::sensor_DPS310_temp()
{
    Serial.println("[Sensor] DPS310 temperature");
    sensors_event_t tempEvent, pressureEvent;
    if (!dps310Status || !dps310.getEvents(&tempEvent, &pressureEvent)) { reportSensorInitError("DPS310"); return; }
    reportTemperature(tempEvent.temperature);
}
void SensorController::sensor_DPS310_pres()
{
    Serial.println("[Sensor] DPS310 pressure");
    sensors_event_t tempEvent, pressureEvent;
    if (!dps310Status || !dps310.getEvents(&tempEvent, &pressureEvent)) { reportSensorInitError("DPS310"); return; }
    reportPressure(pressureEvent.pressure * 100.0); // library reports hPa, reportPressure()/sensorData.barometer is Pa everywhere else
}

void SensorController::sensor_SCD30_co2()
{
    Serial.println("[Sensor] SCD30 CO2");
    if (!scd30Status) { reportSensorInitError("SCD30"); return; }
    float co2, temp, hum;
    if (scd30.readMeasurementData(co2, temp, hum) != 0) { reportSensorInitError("SCD30"); return; }
    Serial.println(co2);
    sensorData.co2 = co2;
}

void SensorController::sensor_SCD4x_co2()
{
    Serial.println("[Sensor] SCD4x CO2");
    if (!scd4xStatus) { reportSensorInitError("SCD4x"); return; }
    uint16_t co2;
    float temp, hum;
    if (scd4x.readMeasurement(co2, temp, hum) != 0) { reportSensorInitError("SCD4x"); return; }
    Serial.println(co2);
    sensorData.co2 = co2;
}

void SensorController::sensor_MHZ19_co2()
{
    Serial.println("[Sensor] MH-Z19 CO2");
    if (!mhz19Status) { reportSensorInitError("MHZ19"); return; }
    mhz19.retrieveData();
    int co2 = mhz19.getCO2();
    Serial.println(co2);
    sensorData.co2 = co2;
}

void SensorController::sensor_Chirp_moist()
{
    Serial.println("[Sensor] Chirp soil moisture");
    if (!chirpStatus) { reportSensorInitError("Chirp"); return; }
    unsigned int capacitance = chirpSoilMoisture.getCapacitance();
    Serial.print("Capacitance: ");
    Serial.println(capacitance);
    // Typical Chirp dry/wet raw capacitance range - not calibrated against a specific real install (same caveat as sensor_analog_moist's soilWet/soilDry).
    const unsigned int chirpDry = 300, chirpWet = 700;
    sensorData.moisture = map(constrain((int)capacitance, (int)chirpDry, (int)chirpWet), chirpDry, chirpWet, 0, 100);
}

void SensorController::sensor_EzoPH_ph()
{
    Serial.println("[Sensor] Atlas EZO pH");
    if (!ezoPhStatus) { reportSensorInitError("EzoPH"); return; }
    ezoPh.send_read_cmd();
    delay(900); // EZO boards need ~900ms to complete a reading before the result can be collected
    ezoPh.receive_read_cmd();
    float ph = ezoPh.get_last_received_reading();
    Serial.println(ph);
    sensorData.liquidPH = ph;
}

void SensorController::sensor_AnyleafPH_ph()
{
    Serial.println("[Sensor] Anyleaf pH");
    if (!anyleafPhStatus) { reportSensorInitError("AnyleafPH"); return; }
    float ph = anyleafPh.read();
    Serial.println(ph);
    sensorData.liquidPH = ph;
}

void SensorController::sensor_ADS1115_ec()
{
    Serial.println("[Sensor] ADS1115 EC");
    if (!ads1115Status) { reportSensorInitError("ADS1115"); return; }
    int16_t raw = ads1115.readADC_SingleEnded(0);
    double millivolts = ads1115.computeVolts(raw) * 1000.0;
    reportEc(millivolts * deviceConfig.configSensor.ecCalibrationSlope + deviceConfig.configSensor.ecCalibrationOffset);
}

void SensorController::sensor_TSL2561_lux()
{
    Serial.println("[Sensor] TSL2561 lux");
    if (!tsl2561Status) { reportSensorInitError("TSL2561"); return; }
    sensors_event_t event;
    tsl2561.getEvent(&event);
    if (!event.light) { reportSensorInitError("TSL2561"); return; }
    sensorData.light = event.light;
}

void SensorController::sensor_TSL2591_lux()
{
    Serial.println("[Sensor] TSL2591 lux");
    if (!tsl2591Status) { reportSensorInitError("TSL2591"); return; }
    sensorData.light = tsl2591.getLuminosity(TSL2591_VISIBLE);
}

void SensorController::sensor_SI1145_lux()
{
    Serial.println("[Sensor] SI1145 lux");
    if (!si1145Status) { reportSensorInitError("SI1145"); return; }
    sensorData.light = si1145.readVisible();
}

void SensorController::sensor_LTR390_lux()
{
    Serial.println("[Sensor] LTR390 ambient light");
    if (!ltr390Status) { reportSensorInitError("LTR390"); return; }
    ltr390.setMode(LTR390_MODE_ALS);
    if (!ltr390.newDataAvailable()) { reportSensorInitError("LTR390"); return; }
    sensorData.light = ltr390.readALS();
}

void SensorController::sensor_VEML7700_lux()
{
    Serial.println("[Sensor] VEML7700 lux");
    if (!veml7700Status) { reportSensorInitError("VEML7700"); return; }
    sensorData.light = veml7700.readLux();
}

void SensorController::sensor_AS7341_lux()
{
    Serial.println("[Sensor] AS7341 clear channel");
    if (!as7341Status || !as7341.readAllChannels()) { reportSensorInitError("AS7341"); return; }
    // No lux conversion in the Adafruit library - raw clear-channel count used as-is, same as the earlier flash-cost experiment's own read.
    sensorData.light = as7341.getChannel(AS7341_CHANNEL_CLEAR);
}

void SensorController::sensor_HX711_weight()
{
    Serial.println("[Sensor] HX711 weight");
    if (!hx711Status || !hx711Scale.is_ready()) { reportSensorInitError("HX711"); return; }
    reportWeight(hx711Scale.get_units(10));
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
    jsonSensorData["deviceFarmUnitID"]=deviceConfig.deviceFarmUnitID;
    jsonSensorData["deviceFarmUnitZoneID"]=deviceConfig.deviceFarmUnitZoneID;


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
    jsonSensorData["ec"]=!isnan(sensorData.ec)? sensorData.ec:  JsonVariant();
    jsonSensorData["weight"]=!isnan(sensorData.weight)? sensorData.weight:  JsonVariant();
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

    ControllerDataChange controllerDataChanges[MAX_REPORTED_FUNCTIONS];
    int controllerDataChangeCount = controller.consumeControllerDataChanges(controllerDataChanges);
    if (controllerDataChangeCount > 0)
    {
        // Own timestamp, not sensorData's - this push can trail SensorData's by however long flushBufferedSensorData() above took, so reusing its dateCreated would misreport when the relay change actually happened.
        service.pushControllerData(serviceRequest, controllerDataChanges, controllerDataChangeCount, device.getDateTime());
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

void SensorController::buildSensorData(const DeviceConfig& deviceConfig)
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
    sensorData.ec=NAN;
    sensorData.weight=NAN;

    // Table-driven dispatch (sensorReadTable above) - one call per config slot, replacing what used to be ~14 separate switch blocks. sensorRainLevel/sensorWind have no real model yet (see DeviceModel.h) so dispatchSensorRead simply finds no match for either, same as the old switches' unreachable default.
    dispatchSensorRead(SensorMetricSlot::Battery, deviceConfig.configSensor.sensorBattery);
    dispatchSensorRead(SensorMetricSlot::Temp, deviceConfig.configSensor.sensorTemp);
    dispatchSensorRead(SensorMetricSlot::TempSoil, deviceConfig.configSensor.sensorTempSoil);
    dispatchSensorRead(SensorMetricSlot::Humid, deviceConfig.configSensor.sensorHumid);
    dispatchSensorRead(SensorMetricSlot::Moist, deviceConfig.configSensor.sensorMoist);
    dispatchSensorRead(SensorMetricSlot::Light, deviceConfig.configSensor.sensorLight);
    dispatchSensorRead(SensorMetricSlot::Co2, deviceConfig.configSensor.sensorCo2);
    dispatchSensorRead(SensorMetricSlot::Tvoc, deviceConfig.configSensor.sensorTvoc);
    dispatchSensorRead(SensorMetricSlot::Barometer, deviceConfig.configSensor.sensorBarometer);
    dispatchSensorRead(SensorMetricSlot::Ph, deviceConfig.configSensor.sensorPH);
    dispatchSensorRead(SensorMetricSlot::Ec, deviceConfig.configSensor.sensorEc);
    dispatchSensorRead(SensorMetricSlot::Weight, deviceConfig.configSensor.sensorWeight);
    dispatchSensorRead(SensorMetricSlot::WaterLevel, deviceConfig.configSensor.sensorWaterLevel);

    buildSensorDataPayload();

    if(deviceConfig.deviceControllerEnabled){
        controller.initController(sensorData, device.getEpochSeconds());
    }

}