#ifndef EpochPlausibility_H
#define EpochPlausibility_H

#include <ctime>

// 2023-11-14 UTC, safely before any real deployment - distinguishes a genuine epoch from the 0
// (or near-0) value NTPClient reports before its first successful sync. Standalone (no Arduino.h
// dependency) so Logic/*.cpp can #include this and still build under the native PlatformIO test env.
constexpr time_t MIN_PLAUSIBLE_EPOCH = 1700000000;

#endif
