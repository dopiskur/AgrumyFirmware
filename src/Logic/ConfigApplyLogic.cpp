#include "ConfigApplyLogic.h"

bool applyEpochFallbackIfCommitted(bool configOk, long serverUtcEpoch, const std::function<void(long)>& applyFallback)
{
    if (!configOk || serverUtcEpoch <= 0)
    {
        return false;
    }
    applyFallback(serverUtcEpoch);
    return true;
}
