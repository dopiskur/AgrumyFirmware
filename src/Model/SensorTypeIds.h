#ifndef SensorTypeIds_H
#define SensorTypeIds_H

// Canonical deviceTypeSensor IDs - must match Agrumy.Shared's api.Models.SensorTypeIds exactly, renumbering desyncs the two independently-versioned repos.
namespace SensorTypeIds
{
    constexpr int Dht11 = 1001;
    constexpr int Dht22 = 1002;
    constexpr int Bmp180 = 1003;
    constexpr int Bmp280 = 1004;
    constexpr int Bme280 = 1005;
    constexpr int Ccs811 = 1006;
    constexpr int Ds18B20 = 1007;
    constexpr int Bh1750 = 1008;
    constexpr int Max17048 = 1009;
    // Roadmap #416 - extended catalog (#415's flash-cost experiment), one ID per physical sensor model.
    constexpr int Max31855 = 1010;
    constexpr int Max31856 = 1011;
    constexpr int Max31865 = 1012;
    constexpr int Mlx90614 = 1013;
    constexpr int Mcp9808 = 1014;
    constexpr int Aht = 1015;
    constexpr int Am2320 = 1016;
    constexpr int Htu21Df = 1017;
    constexpr int Si7021 = 1018;
    constexpr int Sht31 = 1019;
    constexpr int Sht4x = 1020;
    constexpr int Shtc3 = 1021;
    constexpr int Bme680 = 1022;
    constexpr int Dps310 = 1023;
    constexpr int Scd30 = 1024;
    constexpr int Scd4x = 1025;
    constexpr int Mhz19 = 1026;
    constexpr int ChirpSoilMoisture = 1027;
    constexpr int EzoPh = 1028;
    constexpr int AnyleafPh = 1029;
    constexpr int Ads1115Ec = 1030;
    constexpr int Tsl2561 = 1031;
    constexpr int Tsl2591 = 1032;
    constexpr int Si1145 = 1033;
    constexpr int Ltr390 = 1034;
    constexpr int Veml7700 = 1035;
    constexpr int As7341 = 1036;
    constexpr int Hx711 = 1037;
    constexpr int AnalogVoltage = 2001;
    constexpr int AnalogMoisture = 2002;
    constexpr int AnalogWaterLevel = 2003;
}

#endif
