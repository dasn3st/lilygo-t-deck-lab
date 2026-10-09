#pragma once

#include "../freertosinc.h"

namespace concurrency
{

/**
 * @brief Simple wrapper around FreeRTOS API for implementing a mutex lock
 */
class Lock
{
  public:
    Lock();
    ~Lock();

    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;

    /// Locks the lock.
    //
    // Must not be called from an ISR.
    void lock();

    // Unlocks the lock.
    //
    // Must not be called from an ISR.
    void unlock();

#ifdef HAS_FREE_RTOS
    // Freeze diagnostics: which task holds this lock and since when (FreeRTOS tick
    // ms). Maintained by lock()/unlock(); read by the T-Deck stall detector so a
    // main-loop freeze can name a wedged lock holder (this Lock is a binary
    // semaphore with portMAX_DELAY — one never-released hold stalls every waiter).
    volatile void *owner = nullptr;
    volatile uint32_t lockedAtMs = 0;
#endif

  private:
#ifdef HAS_FREE_RTOS
    SemaphoreHandle_t handle;
#endif
};

} // namespace concurrency
