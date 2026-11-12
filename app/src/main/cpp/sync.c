#include "sync.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "thread.h"
#include "debug_log.h"

#define WAITERS_BIT 0x40000000u

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_condattr_t g_cattr; /* CLOCK_MONOTONIC */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/* Every waiting thread sleeps on its own condition variable and is woken alone. (One shared variable with a broadcast
 * per wake was the first version: each mutex handoff woke all ~40 sleeping guest threads, which then queued on g_mu
 * to find the wake was not theirs. Under a contended guest mutex, as while a fight loads, that cost seconds.) */
typedef struct Waiter {
    struct Waiter *next;
    uint64_t key;
    uint32_t tag;
    int done;
    pthread_cond_t cv;
} Waiter;

static Waiter *g_lock_q, *g_cv_q, *g_addr_q, *g_sync_q; /* g_sync_q: threads in WaitSynchronization */

static void init_once(void) {
    pthread_condattr_init(&g_cattr);
    pthread_condattr_setclock(&g_cattr, CLOCK_MONOTONIC);
}

void sync_init(void) { pthread_once(&g_once, init_once); }

static void waiter_init(Waiter *w, uint64_t key, uint32_t tag) {
    w->next = NULL, w->key = key, w->tag = tag, w->done = 0;
    pthread_cond_init(&w->cv, &g_cattr);
}

/* with g_mu held */
static void wake(Waiter *w) {
    w->done = 1;
    pthread_cond_signal(&w->cv);
}

/* an event was signalled or a thread finished: the WaitSynchronization callers look again (they stay queued) */
static void wake_sync_waiters(void) {
    for (Waiter *w = g_sync_q; w; w = w->next) wake(w);
}

void sync_wake_all(void) {
    sync_init();
    pthread_mutex_lock(&g_mu);
    wake_sync_waiters();
    pthread_mutex_unlock(&g_mu);
}

/* ---- helpers, all with g_mu held ---- */

typedef struct Deadline {
    int infinite;
    struct timespec ts;
} Deadline;

static Deadline deadline_from(int64_t timeout_ns) {
    Deadline d = {0};
    if (timeout_ns < 0) {
        d.infinite = 1;
    } else {
        clock_gettime(CLOCK_MONOTONIC, &d.ts);
        int64_t ns = d.ts.tv_nsec + timeout_ns % 1000000000ll;
        d.ts.tv_sec += timeout_ns / 1000000000ll + ns / 1000000000ll;
        d.ts.tv_nsec = ns % 1000000000ll;
    }
    return d;
}

/* Returns nonzero when the deadline has passed. */
static int wait_cv(Waiter *w, const Deadline *d) {
    if (d->infinite) {
        pthread_cond_wait(&w->cv, &g_mu);
        return 0;
    }
    return pthread_cond_timedwait(&w->cv, &g_mu, &d->ts) == ETIMEDOUT;
}

static void enqueue(Waiter **q, Waiter *w) {
    w->next = NULL;
    while (*q) q = &(*q)->next;
    *q = w;
}

static void remove_waiter(Waiter **q, Waiter *w) {
    for (; *q; q = &(*q)->next)
        if (*q == w) {
            *q = w->next;
            return;
        }
}

static Waiter *first_for(Waiter *q, uint64_t key) {
    for (; q; q = q->next)
        if (q->key == key) return q;
    return NULL;
}

static unsigned count_for(Waiter *q, uint64_t key) {
    unsigned n = 0;
    for (; q; q = q->next) n += q->key == key;
    return n;
}

static inline uint32_t load32(uint64_t addr) { return __atomic_load_n((uint32_t *)addr, __ATOMIC_SEQ_CST); }
static inline void store32(uint64_t addr, uint32_t v) { __atomic_store_n((uint32_t *)addr, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t addr, uint32_t expect, uint32_t v) {
    return __atomic_compare_exchange_n((uint32_t *)addr, &expect, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* Hand a mutex to its first waiter, or free it. */
static void handoff(uint64_t addr) {
    Waiter *w = first_for(g_lock_q, addr);
    if (!w) {
        store32(addr, 0);
        return;
    }
    remove_waiter(&g_lock_q, w);
    int more = first_for(g_lock_q, addr) != NULL;
    store32(addr, w->tag | (more ? WAITERS_BIT : 0));
    wake(w);
}

/* ---- mutexes and condition variables ---- */

uint32_t sync_arbitrate_lock(uint32_t owner, uint64_t addr, uint32_t tag) {
    sync_init();
    pthread_mutex_lock(&g_mu);
    if (load32(addr) != (owner | WAITERS_BIT)) {
        pthread_mutex_unlock(&g_mu);
        return RES_OK;
    }
    Waiter w;
    waiter_init(&w, addr, tag);
    enqueue(&g_lock_q, &w);
    while (!w.done) pthread_cond_wait(&w.cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    return RES_OK;
}

uint32_t sync_arbitrate_unlock(uint64_t addr) {
    sync_init();
    pthread_mutex_lock(&g_mu);
    handoff(addr);
    pthread_mutex_unlock(&g_mu);
    return RES_OK;
}

uint32_t sync_wait_cv(uint64_t addr, uint64_t key, uint32_t tag, int64_t timeout_ns) {
    sync_init();
    /* NOTE: returning spurious wakeups from infinite waits (every 100 ms) was tried and made every boot stall at
     * ~64 submits: some guest wait does not re-check its predicate. Keep waits exact. */
    int forced = 0;
    Deadline d = deadline_from(timeout_ns);
    pthread_mutex_lock(&g_mu);
    /* Register as a waiter and mark the condition variable BEFORE releasing the mutex: the SDK's Signal() reads the
     * key word without any lock and skips the svc when it is 0, so a signaller that grabs the mutex in the gap
     * between the release and the mark would lose the wakeup (seen as random boot deadlocks). */
    Waiter w;
    waiter_init(&w, key, tag);
    enqueue(&g_cv_q, &w);
    store32(key, 1);
    handoff(addr);
    int timed_out = 0;
    while (!w.done) {
        if (timeout_ns == 0 || wait_cv(&w, &d)) {
            if (!w.done) {
                remove_waiter(&g_cv_q, &w);
                if (!first_for(g_cv_q, key)) store32(key, 0);
                timed_out = !forced; /* a forced wakeup takes the mutex back like a signal */
            }
            break;
        }
    }

    /* take the mutex back; on a timeout the real kernel returns without it and the SDK locks again itself */
    for (; !timed_out;) {
        uint32_t v = load32(addr);
        if (v == 0) {
            if (cas32(addr, 0, tag)) break;
            continue;
        }
        if (!(v & WAITERS_BIT) && !cas32(addr, v, v | WAITERS_BIT)) continue;
        Waiter lw;
        waiter_init(&lw, addr, tag);
        enqueue(&g_lock_q, &lw);
        while (!lw.done) pthread_cond_wait(&lw.cv, &g_mu);
        break;
    }
    pthread_mutex_unlock(&g_mu);
    return timed_out ? RES_TIMEOUT : RES_OK;
}

void sync_signal_cv(uint64_t key, int32_t count) {
    sync_init();
    pthread_mutex_lock(&g_mu);
    int32_t woken = 0;
    for (Waiter **q = &g_cv_q; *q && (count <= 0 || woken < count);) {
        Waiter *w = *q;
        if (w->key == key) {
            *q = w->next;
            wake(w);
            woken++;
        } else {
            q = &w->next;
        }
    }
    if (!first_for(g_cv_q, key)) store32(key, 0);
    pthread_mutex_unlock(&g_mu);
}

/* diagnostics: every queued waiter with the current value of its key / mutex word */
void sync_debug_dump(void) {
    pthread_mutex_lock(&g_mu);
    for (Waiter *w = g_cv_q; w; w = w->next)
        som_log("stall: cv waiter key 0x%llx tag 0x%x done %d key word %u", (unsigned long long)w->key, w->tag, w->done,
                load32(w->key));
    for (Waiter *w = g_lock_q; w; w = w->next)
        som_log("stall: lock waiter addr 0x%llx tag 0x%x done %d word 0x%x", (unsigned long long)w->key, w->tag, w->done,
                load32(w->key));
    pthread_mutex_unlock(&g_mu);
}

/* ---- address arbiter ---- */

uint32_t sync_wait_for_address(uint64_t addr, uint32_t type, int32_t value, int64_t timeout_ns) {
    sync_init();
    Deadline d = deadline_from(timeout_ns);
    pthread_mutex_lock(&g_mu);
    int32_t v = (int32_t)load32(addr);
    if (type == 0 && !(v < value)) goto bad;
    if (type == 1) {
        for (;;) { /* atomic against guest userspace atomics */
            if (!(v < value)) goto bad;
            if (cas32(addr, (uint32_t)v, (uint32_t)(v - 1))) break;
            v = (int32_t)load32(addr);
        }
    }
    if (type == 2 && v != value) goto bad;
    if (type > 2) {
        pthread_mutex_unlock(&g_mu);
        return RES_INVALID_ENUM;
    }
    if (timeout_ns == 0) {
        pthread_mutex_unlock(&g_mu);
        return RES_TIMEOUT;
    }
    Waiter w;
    waiter_init(&w, addr, 0);
    enqueue(&g_addr_q, &w);
    uint32_t res = RES_OK;
    while (!w.done) {
        if (wait_cv(&w, &d)) {
            if (!w.done) {
                remove_waiter(&g_addr_q, &w);
                res = RES_TIMEOUT;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return res;
bad:
    pthread_mutex_unlock(&g_mu);
    return RES_INVALID_STATE;
}

uint32_t sync_signal_to_address(uint64_t addr, uint32_t type, int32_t value, int32_t count) {
    sync_init();
    pthread_mutex_lock(&g_mu);
    int32_t v = (int32_t)load32(addr);
    if (type == 1) {
        if (v != value || !cas32(addr, (uint32_t)value, (uint32_t)(value + 1))) goto bad;
    } else if (type == 2) {
        if (v != value) goto bad;
        unsigned n = count_for(g_addr_q, addr);
        int32_t nv;
        if (count <= 0) nv = n > 0 ? value - 2 : value + 1;
        else if (n == 0) nv = value + 1;
        else if ((int32_t)n <= count) nv = value - 1;
        else nv = value;
        if (nv != value && !cas32(addr, (uint32_t)value, (uint32_t)nv)) goto bad;
    } else if (type != 0) {
        pthread_mutex_unlock(&g_mu);
        return RES_INVALID_ENUM;
    }
    int32_t woken = 0;
    for (Waiter **q = &g_addr_q; *q && (count <= 0 || woken < count);) {
        Waiter *w = *q;
        if (w->key == addr) {
            *q = w->next;
            wake(w);
            woken++;
        } else {
            q = &w->next;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return RES_OK;
bad:
    pthread_mutex_unlock(&g_mu);
    return RES_INVALID_STATE;
}

/* ---- events ---- */

KEvent *kevent_new(void) {
    KEvent *e = calloc(1, sizeof *e);
    if (e) e->refs = 1;
    return e;
}

void kevent_unref(KEvent *e) {
    if (__atomic_sub_fetch(&e->refs, 1, __ATOMIC_SEQ_CST) == 0) free(e);
}

void kevent_signal(KEvent *e) {
    sync_init();
    pthread_mutex_lock(&g_mu);
    e->signaled = 1;
    wake_sync_waiters();
    pthread_mutex_unlock(&g_mu);
}

void kevent_clear(KEvent *e) {
    sync_init();
    pthread_mutex_lock(&g_mu);
    e->signaled = 0;
    pthread_mutex_unlock(&g_mu);
}

uint32_t kevent_create_handles(uint32_t *writable, uint32_t *readable) {
    KEvent *e = kevent_new();
    if (!e) return RES_OUT_OF_MEMORY;
    e->refs = 2;
    *writable = kobj_new(KO_EVENT_W, e);
    *readable = kobj_new(KO_EVENT_R, e);
    if (!*writable || !*readable) return RES_OUT_OF_MEMORY;
    return RES_OK;
}

/* ---- WaitSynchronization ---- */

uint32_t sync_wait_synchronization(const uint32_t *handles, int32_t n, int64_t timeout_ns, int32_t *index) {
    sync_init();
    Deadline d = deadline_from(timeout_ns);
    pthread_mutex_lock(&g_mu);
    Waiter w;
    waiter_init(&w, 0, 0);
    enqueue(&g_sync_q, &w);
    uint32_t res;
    for (;;) {
        w.done = 0;
        for (int32_t i = 0; i < n; i++) {
            int kind = kobj_kind(handles[i]);
            void *o = kobj_get(handles[i], kind);
            int ready;
            if (kind == KO_EVENT_R || kind == KO_EVENT_W) ready = ((KEvent *)o)->signaled;
            else if (kind == KO_THREAD) ready = kthread_finished(o);
            else {
                res = RES_INVALID_HANDLE;
                goto out;
            }
            if (ready) {
                *index = i;
                res = RES_OK;
                goto out;
            }
        }
        /* a wake since the scan (done) means: look again; a deadline that passed ends the wait */
        if (timeout_ns == 0 || (wait_cv(&w, &d) && !w.done)) {
            res = RES_TIMEOUT;
            goto out;
        }
    }
out:
    remove_waiter(&g_sync_q, &w);
    pthread_mutex_unlock(&g_mu);
    return res;
}

