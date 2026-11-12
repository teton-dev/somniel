#pragma once
/* Horizon synchronisation primitives on top of one big lock and one condition
 * variable. Simple and correct first; per-address queues can replace the
 * broadcast later if the profile asks for it. */

#include <stdint.h>

#include "kobj.h"

typedef struct KEvent {
    int signaled;
    int refs;
} KEvent;

void sync_init(void);
void sync_wake_all(void);

uint32_t sync_arbitrate_lock(uint32_t owner, uint64_t addr, uint32_t tag);
uint32_t sync_arbitrate_unlock(uint64_t addr);
uint32_t sync_wait_cv(uint64_t addr, uint64_t key, uint32_t tag, int64_t timeout_ns);
void sync_signal_cv(uint64_t key, int32_t count);
uint32_t sync_wait_for_address(uint64_t addr, uint32_t type, int32_t value, int64_t timeout_ns);
uint32_t sync_signal_to_address(uint64_t addr, uint32_t type, int32_t value, int32_t count);
uint32_t sync_wait_synchronization(const uint32_t *handles, int32_t n, int64_t timeout_ns, int32_t *index);

/* events: a KEvent is shared by its writable and readable handles */
KEvent *kevent_new(void);
void kevent_unref(KEvent *e);
void kevent_signal(KEvent *e);
void kevent_clear(KEvent *e);
uint32_t kevent_create_handles(uint32_t *writable, uint32_t *readable);
