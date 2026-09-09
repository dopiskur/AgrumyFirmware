#ifndef SensorWarmupLogic_H
#define SensorWarmupLogic_H

#include <stddef.h>

// Longest datasheet time-to-first-valid-sample among the selected deviceTypeSensor IDs; 0 when every selected driver is ready as soon as begin() returns.
unsigned long sensorWarmupMs(const int* selectedSensorTypeIds, size_t count);

#endif
