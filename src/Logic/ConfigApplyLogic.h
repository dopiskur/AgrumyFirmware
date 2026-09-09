#ifndef ConfigApplyLogic_H
#define ConfigApplyLogic_H

#include <functional>

// Calls applyFallback exactly once only when the config was committed AND carries an epoch (>0) - loading alone must never touch the clock, so DeviceController::loadConfig no longer does; see ServiceController::apiConfig and main.cpp's boot load.
bool applyEpochFallbackIfCommitted(bool configOk, long serverUtcEpoch, const std::function<void(long)>& applyFallback);

#endif
