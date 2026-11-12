#define _GNU_SOURCE /* cpu_set_t */
#include "thread.h"

#include <asm/sigcontext.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "debug_log.h"
#include "kobj.h"
#include "sync.h"

#define TLR_BASE 0x0000003010000000ull
#define HOST_STACK (1u << 20)

__thread GuestCtx *t_ctx;
static __thread KThread *t_thread;

static uint64_t g_next_tlr = TLR_BASE;
static uint64_t g_next_id = 1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

int kthread_finished(void *t) { return ((KThread *)t)->finished; }

/* Experiment switch: a file "onecore" in the data dir pins every guest thread to one CPU (the recomp's model), to tell
 * parallelism races from missing features. */
extern const char *som_data_dir(void);
static void maybe_pin(void) {
    static int on = -1;
    if (on < 0) {
        char path[600];
        snprintf(path, sizeof path, "%s/onecore", som_data_dir());
        on = access(path, F_OK) == 0;
        if (on) som_log("onecore: pinning guest threads to CPU 7");
    }
    if (on) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(7, &set);
        sched_setaffinity(0, sizeof set, &set);
    }
}
int g_guest_tid[128]; /* guest thread id -> linux tid, for per-thread CPU time in the long-frame report */
static KThread *g_threads[128]; /* same index: every guest thread, for the app-wide background pause */
volatile int g_bg_pause; /* app is in the background: every guest thread parks (som_guest_pause_all) */
#define WANT_PAUSE(t) ((t)->pause_req || g_bg_pause)

KThread *kthread_current(void) { return t_thread; }

uint64_t tlr_alloc(void) {
    pthread_mutex_lock(&g_lock);
    uint64_t a = g_next_tlr;
    g_next_tlr += 0x1000;
    pthread_mutex_unlock(&g_lock);
    void *p = mmap((void *)a, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if ((uint64_t)p != a) {
        som_log("tlr_alloc: mmap failed: %s", strerror(errno));
        return 0;
    }
    guest_region_set(a, 0x1000, MT_TLS, PERM_R | PERM_W);
    return a;
}

KThread *kthread_register_main(GuestCtx *ctx, uint64_t tlr, uint32_t *handle) {
    KThread *t = calloc(1, sizeof *t);
    t->ctx = ctx;
    t->tlr = tlr;
    t->started = 1;
    t->linux_tid = (int)syscall(SYS_gettid);
    maybe_pin();
    t->id = __atomic_fetch_add(&g_next_id, 1, __ATOMIC_SEQ_CST);
    g_guest_tid[t->id & 127] = t->linux_tid;
    g_threads[t->id & 127] = t;
    t->handle = kobj_new(KO_THREAD, t);
    ctx->tlr = tlr;
    t_ctx = ctx;
    t_thread = t;
    *handle = t->handle;
    return t;
}

static void *thread_main(void *arg) {
    KThread *t = arg;
    t_ctx = t->ctx;
    t_thread = t;
    t->linux_tid = (int)syscall(SYS_gettid);
    g_guest_tid[t->id & 127] = t->linux_tid;
    g_threads[t->id & 127] = t;
    maybe_pin();
    size_t alt_size = 64 * 1024;
    stack_t ss = {.ss_sp = malloc(alt_size), .ss_size = alt_size, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    som_log("guest thread %llu: start entry 0x%llx sp 0x%llx arg 0x%llx", (unsigned long long)t->id,
            (unsigned long long)t->entry, (unsigned long long)t->stack_top, (unsigned long long)t->arg);
    som_enter_guest(t->ctx, t->entry, t->stack_top, t->arg, 0);
    som_log("guest thread %llu: finished", (unsigned long long)t->id);
    t->finished = 1;
    sync_wake_all();
    return NULL;
}

uint32_t kthread_create(uint64_t entry, uint64_t arg, uint64_t stack_top, uint32_t *handle) {
    KThread *t = calloc(1, sizeof *t);
    if (!t) return RES_OUT_OF_MEMORY;
    t->ctx = guest_ctx_alloc();
    t->tlr = tlr_alloc();
    if (!t->ctx || !t->tlr) {
        free(t);
        return RES_OUT_OF_MEMORY;
    }
    int ctx_id = t->ctx->id;
    memset(t->ctx, 0, sizeof *t->ctx);
    t->ctx->id = ctx_id;
    t->ctx->dispatch = som_svc_dispatch;
    t->ctx->tlr = t->tlr;
    t->entry = entry;
    t->arg = arg;
    t->stack_top = stack_top;
    t->id = __atomic_fetch_add(&g_next_id, 1, __ATOMIC_SEQ_CST);
    t->handle = kobj_new(KO_THREAD, t);
    if (!t->handle) {
        free(t);
        return RES_OUT_OF_MEMORY;
    }
    *handle = t->handle;
    return RES_OK;
}

uint32_t kthread_start(uint32_t handle) {
    KThread *t = kobj_get(handle, KO_THREAD);
    if (!t) return RES_INVALID_HANDLE;
    if (t->started) return RES_INVALID_STATE;
    t->started = 1;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, HOST_STACK);
    int rc = pthread_create(&t->pt, &attr, thread_main, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        som_log("pthread_create failed: %d", rc);
        return RES_OUT_OF_MEMORY;
    }
    pthread_detach(t->pt);
    return RES_OK;
}

void kthread_exit_current(GuestCtx *ctx) {
    som_leave_guest(ctx);
}

/* ---- pausing threads (svc SetThreadActivity / GetThreadContext3) ----
 * Unity's garbage collector stops every other thread, reads its registers and
 * scans its stack. A pause sends SOM_SIG_SUSPEND to the target. If the signal
 * lands in guest code, the handler snapshots the interrupted registers and
 * parks the thread until resumed. If the target is inside a kernel call or an
 * SDK hook, the guest registers are already saved in its GuestCtx; the handler
 * snapshots those and the thread parks when it returns to the guest. */

static pthread_mutex_t g_pause_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pause_cv = PTHREAD_COND_INITIALIZER;

static void put64(uint8_t *b, size_t off, uint64_t v) { memcpy(b + off, &v, 8); }

static void snapshot_from_ucontext(KThread *t, ucontext_t *u) {
    uint8_t *b = t->ctx_buf;
    mcontext_t *m = &u->uc_mcontext;
    memset(b, 0, sizeof t->ctx_buf);
    memcpy(b, m->regs, 29 * 8);
    put64(b, 0xE8, m->regs[29]);
    put64(b, 0xF0, m->regs[30]);
    put64(b, 0xF8, m->sp);
    put64(b, 0x100, m->pc);
    uint32_t psr = (uint32_t)m->pstate;
    memcpy(b + 0x108, &psr, 4);
    struct _aarch64_ctx *h = (struct _aarch64_ctx *)m->__reserved;
    while (h->magic) {
        if (h->magic == FPSIMD_MAGIC) {
            struct fpsimd_context *f = (struct fpsimd_context *)h;
            memcpy(b + 0x110, f->vregs, 512);
            memcpy(b + 0x310, &f->fpcr, 4);
            memcpy(b + 0x314, &f->fpsr, 4);
        }
        if (!h->size) break;
        h = (struct _aarch64_ctx *)((char *)h + h->size);
    }
    put64(b, 0x318, t->ctx->guest_tpidr);
    t->ctx_valid = 1;
}

static void snapshot_from_ctx(KThread *t) {
    GuestCtx *c = t->ctx;
    uint8_t *b = t->ctx_buf;
    memset(b, 0, sizeof t->ctx_buf);
    memcpy(b, c->x, 29 * 8);
    put64(b, 0xE8, c->x[29]);
    put64(b, 0xF0, c->x[30]);
    put64(b, 0xF8, c->sp);
    /* the guest resumes at the target of the stub''s final branch; hooks return through lr */
    uint32_t w = *(const uint32_t *)(c->ret_addr + 4);
    uint64_t pc = c->x[30];
    if ((w >> 26) == 0x5) {
        int64_t imm = (int32_t)(w << 6) >> 6;
        pc = c->ret_addr + 4 + (uint64_t)(imm * 4);
    }
    put64(b, 0x100, pc);
    uint32_t psr = (uint32_t)c->nzcv;
    memcpy(b + 0x108, &psr, 4);
    memcpy(b + 0x110, c->q, 512);
    put64(b, 0x318, c->guest_tpidr);
    t->ctx_valid = 1;
}

void som_suspend_handler(int sig, void *info, void *uc, int in_guest) {
    (void)sig;
    (void)info;
    KThread *t = t_thread;
    if (!t || !WANT_PAUSE(t)) return;
    if (in_guest) snapshot_from_ucontext(t, uc);
    else snapshot_from_ctx(t);
    pthread_mutex_lock(&g_pause_mu);
    t->paused = 1;
    pthread_cond_broadcast(&g_pause_cv);
    if (in_guest)
        while (WANT_PAUSE(t)) pthread_cond_wait(&g_pause_cv, &g_pause_mu);
    pthread_mutex_unlock(&g_pause_mu);
}

void kthread_check_pause(void) {
    KThread *t = t_thread;
    if (!t || !WANT_PAUSE(t)) return;
    /* the suspend handler takes g_pause_mu too: with the signal unblocked, one landing while this thread holds the
     * mutex would deadlock the thread on itself (the GC then waits for it forever) */
    sigset_t blk, old;
    sigemptyset(&blk);
    sigaddset(&blk, SOM_SIG_SUSPEND);
    pthread_sigmask(SIG_BLOCK, &blk, &old);
    pthread_mutex_lock(&g_pause_mu);
    while (WANT_PAUSE(t)) {
        t->paused = 1;
        pthread_cond_broadcast(&g_pause_cv);
        pthread_cond_wait(&g_pause_cv, &g_pause_mu);
    }
    pthread_mutex_unlock(&g_pause_mu);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

/* stop-the-world accounting for the stall report (svc.c som_stall_mark) */
extern uint64_t g_gc_pauses, g_gc_pause_ns;
static int g_npaused;
static struct timespec g_world_stop;

uint32_t kthread_set_activity(uint32_t handle, uint32_t activity) {
    KThread *t = handle == HANDLE_CUR_THREAD ? t_thread : kobj_get(handle, KO_THREAD);
    if (!t) return RES_INVALID_HANDLE;
    if (activity > 1) return RES_INVALID_ENUM;
    if (t == t_thread) return RES_INVALID_HANDLE; /* a thread cannot pause itself */
    if (activity == 1) {
        if (t->pause_req || !t->started || t->finished || !t->linux_tid) return RES_INVALID_STATE;
        pthread_mutex_lock(&g_pause_mu);
        t->pause_req = 1;
        t->paused = 0;
        t->ctx_valid = 0;
        pthread_mutex_unlock(&g_pause_mu);
        syscall(SYS_tgkill, (int)getpid(), t->linux_tid, SOM_SIG_SUSPEND);
        struct timespec dl;
        clock_gettime(CLOCK_MONOTONIC, &dl);
        dl.tv_sec += 5;
        pthread_mutex_lock(&g_pause_mu);
        while (!t->paused) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            struct timespec abs = {now.tv_sec + 5, now.tv_nsec};
            if (pthread_cond_timedwait(&g_pause_cv, &g_pause_mu, &abs) == ETIMEDOUT) {
                som_log("pause of thread %llu timed out", (unsigned long long)t->id);
                break;
            }
        }
        if (g_npaused++ == 0) {
            clock_gettime(CLOCK_MONOTONIC, &g_world_stop);
            __atomic_fetch_add(&g_gc_pauses, 1, __ATOMIC_RELAXED);
        }
        pthread_mutex_unlock(&g_pause_mu);
        return RES_OK;
    }
    if (!t->pause_req) return RES_INVALID_STATE;
    pthread_mutex_lock(&g_pause_mu);
    if (g_npaused > 0 && --g_npaused == 0) { /* the collector let the last thread go: one stop-the-world ends */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        __atomic_fetch_add(&g_gc_pause_ns, (uint64_t)((now.tv_sec - g_world_stop.tv_sec) * 1000000000ll + (now.tv_nsec - g_world_stop.tv_nsec)), __ATOMIC_RELAXED);
    }
    t->pause_req = 0;
    t->paused = 0;
    t->ctx_valid = 0;
    pthread_cond_broadcast(&g_pause_cv);
    pthread_mutex_unlock(&g_pause_mu);
    return RES_OK;
}

uint32_t kthread_get_context(uint32_t handle, void *out) {
    KThread *t = kobj_get(handle, KO_THREAD);
    if (!t) return RES_INVALID_HANDLE;
    if (!t->paused || !t->ctx_valid) return RES_INVALID_STATE;
    memcpy(out, t->ctx_buf, sizeof t->ctx_buf);
    return RES_OK;
}


/* App-wide pause for Android going to the background (screen lock, home): every guest thread parks until resumed, so
 * the game stops running, producing audio and burning battery. Threads in host code or a blocking kernel call park
 * when they return to the guest (kthread_check_pause); threads in guest code are interrupted by the signal. */
void som_guest_pause_all(int on) {
    if (on) {
        __atomic_store_n(&g_bg_pause, 1, __ATOMIC_SEQ_CST);
        int n = 0;
        for (int i = 0; i < 128; i++) {
            KThread *t = g_threads[i];
            if (!t || t == t_thread || !t->started || t->finished || !t->linux_tid) continue;
            syscall(SYS_tgkill, (int)getpid(), t->linux_tid, SOM_SIG_SUSPEND);
            n++;
        }
        som_log("guest paused (%d threads signalled)", n);
    } else {
        pthread_mutex_lock(&g_pause_mu);
        __atomic_store_n(&g_bg_pause, 0, __ATOMIC_SEQ_CST);
        pthread_cond_broadcast(&g_pause_cv);
        pthread_mutex_unlock(&g_pause_mu);
        som_log("guest resumed");
    }
}
