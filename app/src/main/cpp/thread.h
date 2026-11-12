#pragma once
/* Guest threads: one host pthread and one GuestCtx per Horizon thread. */

#include <pthread.h>
#include <stdint.h>

#include "guest.h"

typedef struct KThread {
    GuestCtx *ctx;
    pthread_t pt;
    volatile int finished;
    int started;
    uint32_t handle;
    uint64_t id;
    uint64_t entry, arg, stack_top, tlr;
    int linux_tid;
    volatile int pause_req, paused;
    int ctx_valid;
    uint8_t ctx_buf[0x320]; /* ThreadContext as GetThreadContext3 returns it */
} KThread;

extern __thread GuestCtx *t_ctx; /* the guest context running on this host thread */

int kthread_finished(void *t);
uint64_t tlr_alloc(void);
KThread *kthread_register_main(GuestCtx *ctx, uint64_t tlr, uint32_t *handle);
uint32_t kthread_create(uint64_t entry, uint64_t arg, uint64_t stack_top, uint32_t *handle);
uint32_t kthread_start(uint32_t handle);
__attribute__((noreturn)) void kthread_exit_current(GuestCtx *ctx);
KThread *kthread_current(void);
uint32_t kthread_set_activity(uint32_t handle, uint32_t activity);
uint32_t kthread_get_context(uint32_t handle, void *out);
void kthread_check_pause(void); /* called as the guest resumes from a kernel call */
void som_guest_pause_all(int on); /* background pause: park every guest thread (on=1) / let them go (on=0) */
void som_suspend_handler(int sig, void *info, void *uc, int in_guest);

