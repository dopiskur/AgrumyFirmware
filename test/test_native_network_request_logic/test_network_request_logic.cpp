#include <unity.h>
#include "../../src/Logic/NetworkRequestLogic.h"

void setUp(void) {}
void tearDown(void) {}

// Ordering A: the network task finishes first (gives the semaphore, then releases) and the facade's own
// wait later returns normally - first release() must not free, second must.
void test_Release_TaskFirstThenFacade(void)
{
    std::atomic<int> released{0};
    TEST_ASSERT_FALSE(networkRequestReleaseShouldFree(released)); // task's release
    TEST_ASSERT_TRUE(networkRequestReleaseShouldFree(released));  // facade's release
}

// Ordering B: the facade's wait times out and releases first, while the task is still working; the task
// releases later once it finishes - same two outcomes, reversed order.
void test_Release_FacadeFirstThenTask(void)
{
    std::atomic<int> released{0};
    TEST_ASSERT_FALSE(networkRequestReleaseShouldFree(released)); // facade's release (timeout)
    TEST_ASSERT_TRUE(networkRequestReleaseShouldFree(released));  // task's release
}

void test_Enqueue_QueueHasRoom_Succeeds(void)
{
    TEST_ASSERT_TRUE(networkRequestTryEnqueue([]() { return true; }));
}

void test_Enqueue_QueueFull_FailsWithoutBlocking(void)
{
    TEST_ASSERT_FALSE(networkRequestTryEnqueue([]() { return false; }));
}

// Models the real queue's fixed length (4, see ServiceController.cpp's NETWORK_QUEUE_LENGTH): the 5th
// enqueue attempt with 4 already pending must fail immediately, not block.
void test_Enqueue_FifthAttemptWithFourPending_Fails(void)
{
    int pending = 0;
    auto trySend = [&pending]() {
        if (pending >= 4)
        {
            return false;
        }
        pending++;
        return true;
    };

    TEST_ASSERT_TRUE(networkRequestTryEnqueue(trySend));
    TEST_ASSERT_TRUE(networkRequestTryEnqueue(trySend));
    TEST_ASSERT_TRUE(networkRequestTryEnqueue(trySend));
    TEST_ASSERT_TRUE(networkRequestTryEnqueue(trySend));
    TEST_ASSERT_FALSE(networkRequestTryEnqueue(trySend));
    TEST_ASSERT_EQUAL_INT(4, pending);
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_Release_TaskFirstThenFacade);
    RUN_TEST(test_Release_FacadeFirstThenTask);
    RUN_TEST(test_Enqueue_QueueHasRoom_Succeeds);
    RUN_TEST(test_Enqueue_QueueFull_FailsWithoutBlocking);
    RUN_TEST(test_Enqueue_FifthAttemptWithFourPending_Fails);
    return UNITY_END();
}
