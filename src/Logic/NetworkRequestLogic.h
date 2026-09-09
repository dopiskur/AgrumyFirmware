#ifndef NetworkRequestLogic_H
#define NetworkRequestLogic_H

#include <atomic>
#include <functional>

// True for whichever of the two parties (network task, waiting facade) releases a NetworkRequest second - that one frees it, in either order.
bool networkRequestReleaseShouldFree(std::atomic<int>& releaseCount);

// trySend is injected (real: short-timeout xQueueSend; tests: a fixed-capacity fake) so queue-full is testable without FreeRTOS; false means never wait on the semaphore.
bool networkRequestTryEnqueue(const std::function<bool()>& trySend);

#endif
