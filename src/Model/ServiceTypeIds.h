#ifndef ServiceTypeIds_H
#define ServiceTypeIds_H

// Canonical deviceTypeService IDs (deviceTypeServiceID / registration serviceType) - must match Agrumy.Shared's DeviceServiceTypeIds exactly, same cross-repo convention as SensorTypeIds.
namespace ServiceTypeIds
{
    constexpr int Http = 0;
    constexpr int Https = 1;
    constexpr int Mqtt = 2;
}

#endif
