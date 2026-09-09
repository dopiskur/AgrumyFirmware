#include "SensorWarmupLogic.h"
#include "../Model/SensorTypeIds.h"

namespace
{
    struct DriverWarmup
    {
        int sensorTypeId;
        unsigned long ms;
    };

    // Drivers absent here are ready as soon as begin() returns (I2C temperature/pressure chips, ADC, DS18B20 whose library blocks on conversion itself).
    constexpr DriverWarmup DRIVER_WARMUPS[] = {
        { SensorTypeIds::Dht11, 2000 },
        { SensorTypeIds::Dht22, 2000 },
        { SensorTypeIds::Am2320, 2000 },
        { SensorTypeIds::Ccs811, 1000 },
        { SensorTypeIds::Bh1750, 200 },
        { SensorTypeIds::Bme680, 1000 },
        { SensorTypeIds::Scd30, 2000 },
        { SensorTypeIds::Scd4x, 5000 },
        { SensorTypeIds::Mhz19, 5000 },
        { SensorTypeIds::Tsl2561, 500 },
        { SensorTypeIds::Tsl2591, 700 },
        { SensorTypeIds::Ltr390, 200 },
        { SensorTypeIds::Veml7700, 200 },
        { SensorTypeIds::As7341, 500 },
        { SensorTypeIds::EzoPh, 1000 },
    };
}

unsigned long sensorWarmupMs(const int* selectedSensorTypeIds, size_t count)
{
    unsigned long longest = 0;
    for (size_t i = 0; i < count; i++)
    {
        for (const DriverWarmup& w : DRIVER_WARMUPS)
        {
            if (w.sensorTypeId == selectedSensorTypeIds[i] && w.ms > longest)
                longest = w.ms;
        }
    }
    return longest;
}
