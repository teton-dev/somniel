#pragma once
#include <pthread.h>
#include <sched.h>
#include <time.h>

typedef pthread_rwlock_t PlatMutex;
#define PLAT_MUTEX_INIT PTHREAD_RWLOCK_INITIALIZER
static inline void plat_mutex_lock(PlatMutex *m) { pthread_rwlock_wrlock(m); }
static inline void plat_mutex_unlock(PlatMutex *m) { pthread_rwlock_unlock(m); }
static inline void plat_mutex_lock_shared(PlatMutex *m) { pthread_rwlock_rdlock(m); }
static inline void plat_mutex_unlock_shared(PlatMutex *m) { pthread_rwlock_unlock(m); }
static inline void plat_sleep_ms(unsigned ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}
static inline void plat_yield(void) { sched_yield(); }
