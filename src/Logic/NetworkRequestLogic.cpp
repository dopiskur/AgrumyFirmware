#include "NetworkRequestLogic.h"

bool networkRequestReleaseShouldFree(std::atomic<int>& releaseCount)
{
    return releaseCount.fetch_add(1) == 1;
}

bool networkRequestTryEnqueue(const std::function<bool()>& trySend)
{
    return trySend();
}
