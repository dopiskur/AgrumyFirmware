#ifndef NetworkRequestLogic_H
#define NetworkRequestLogic_H

#include <atomic>
#include <functional>

// Ownership handshake for ServiceController's NetworkRequest, shared between the persistent network task
// and the facade (requestPost/requestGet/firmwareUpdate) waiting on it - a facade timeout can leave the
// task still working on the (heap-allocated) request, so whichever party calls this SECOND is the one that
// must free it; the first call just marks its own side done. Order-agnostic by design: works the same
// whether the task finishes before the facade's wait times out, or the reverse.
bool networkRequestReleaseShouldFree(std::atomic<int>& releaseCount);

// trySend is injected (real code: a short-timeout xQueueSend wrapped as a bool; native tests: a fake
// modeling a fixed-capacity queue) so the queue-full decision is testable without linking FreeRTOS. False
// means the caller must build an immediate error result and release the request itself - nothing will ever
// signal its semaphore, since the task never saw it.
bool networkRequestTryEnqueue(const std::function<bool()>& trySend);

#endif
