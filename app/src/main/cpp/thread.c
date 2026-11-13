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
#include <linux/futex.h>
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

/* No mutex anywhere on this path. The signal handler used to take a pthread mutex (and park in pthread_cond_wait),
 * neither of which is async-signal-safe; the "stall: no submit" hangs ended with every guest thread asleep and the
 * collector, plus paused threads returning from kernel calls, all queued on that mutex with no running owner
 * (A17, 2026-11-12). Now: per-thread flags are atomics, a paused thread waits on the futex word g_pause_gen (bumped
 * by every resume), and the pauser waits on the target's `paused` word. Both futex calls are signal-safe. */
static volatile int g_pause_gen;

static void fwait(volatile int *addr, int val, long ns) {
    struct timespec ts = {ns / 1000000000l, ns % 1000000000l};
    syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, val, ns ? &ts : NULL, NULL, 0);
}
static void fwake(volatile int *addr) { syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, 0x7fffffff, NULL, NULL, 0); }

static int want_pause(KThread *t) {
    return __atomic_load_n(&t->pause_req, __ATOMIC_ACQUIRE) || __atomic_load_n(&g_bg_pause, __ATOMIC_ACQUIRE);
}

/* acknowledge (the snapshot is already written) and wake the pauser */
static void mark_paused(KThread *t) {
    __atomic_store_n(&t->paused, 1, __ATOMIC_RELEASE);
    fwake(&t->paused);
}

/* park until no pause is wanted: read the generation before testing, so a resume in between changes it and the
 * futex wait returns at once */
static void park(KThread *t) {
    for (;;) {
        int gen = __atomic_load_n(&g_pause_gen, __ATOMIC_ACQUIRE);
        if (!want_pause(t)) return;
        fwait(&g_pause_gen, gen, 0);
    }
}

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
    if (!t || !want_pause(t)) return;
    /* already acknowledged (kthread_check_pause got there first): the collector may be reading the snapshot, leave it */
    if (!__atomic_load_n(&t->paused, __ATOMIC_ACQUIRE)) {
        if (in_guest) snapshot_from_ucontext(t, uc);
        else snapshot_from_ctx(t);
        mark_paused(t);
    }
    if (in_guest) park(t); /* in host code the thread parks when it returns to the guest (kthread_check_pause) */
}

void kthread_check_pause(void) {
    KThread *t = t_thread;
    if (!t || !want_pause(t)) return;
    /* a pause the signal already acknowledged (its snapshot of this thread's kernel-call registers stands); without one
     * (the background pause, or the signal not delivered yet) take the snapshot here */
    if (!__atomic_load_n(&t->paused, __ATOMIC_ACQUIRE)) {
        snapshot_from_ctx(t);
        mark_paused(t);
    }
    park(t);
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
        if (!t->started || t->finished || !t->linux_tid) return RES_INVALID_STATE;
        /* clear the acknowledgement first, then publish the request: whoever claims the request owns this pause */
        if (__atomic_load_n(&t->pause_req, __ATOMIC_ACQUIRE)) return RES_INVALID_STATE;
        __atomic_store_n(&t->ctx_valid, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&t->paused, 0, __ATOMIC_RELEASE);
        int zero = 0;
        if (!__atomic_compare_exchange_n(&t->pause_req, &zero, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return RES_INVALID_STATE;
        static int stoppers;
        static uint64_t overlaps;
        int now_in = __atomic_add_fetch(&stoppers, 1, __ATOMIC_SEQ_CST);
        if (now_in > 1 && (++overlaps & (overlaps - 1)) == 0)
            som_log("pause: %d threads pausing at once (thread %llu pauses %llu; %llu times so far)", now_in,
                    t_thread ? (unsigned long long)t_thread->id : 0ull, (unsigned long long)t->id, (unsigned long long)overlaps);
        syscall(SYS_tgkill, (int)getpid(), t->linux_tid, SOM_SIG_SUSPEND);
        /* wait for the acknowledgement (in 2 ms slices, then give up after 5 s). This thread may itself be paused
         * meanwhile: its own handler only snapshots and returns here (it is in host code), and it parks at its next
         * return to the guest. */
        struct timespec dl, m;
        clock_gettime(CLOCK_MONOTONIC, &dl);
        dl.tv_sec += 5;
        while (!__atomic_load_n(&t->paused, __ATOMIC_ACQUIRE)) {
            fwait(&t->paused, 0, 2000000);
            clock_gettime(CLOCK_MONOTONIC, &m);
            if (!__atomic_load_n(&t->paused, __ATOMIC_ACQUIRE) && (m.tv_sec > dl.tv_sec || (m.tv_sec == dl.tv_sec && m.tv_nsec >= dl.tv_nsec))) {
                som_log("pause of thread %llu timed out", (unsigned long long)t->id);
                break;
            }
        }
        if (__atomic_fetch_add(&g_npaused, 1, __ATOMIC_ACQ_REL) == 0) {
            clock_gettime(CLOCK_MONOTONIC, &g_world_stop);
            __atomic_fetch_add(&g_gc_pauses, 1, __ATOMIC_RELAXED);
        }
        __atomic_sub_fetch(&stoppers, 1, __ATOMIC_SEQ_CST);
        return RES_OK;
    }
    int one = 1;
    if (!__atomic_compare_exchange_n(&t->pause_req, &one, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return RES_INVALID_STATE;
    if (__atomic_sub_fetch(&g_npaused, 1, __ATOMIC_ACQ_REL) == 0) { /* the collector let the last thread go */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        __atomic_fetch_add(&g_gc_pause_ns, (uint64_t)((now.tv_sec - g_world_stop.tv_sec) * 1000000000ll + (now.tv_nsec - g_world_stop.tv_nsec)), __ATOMIC_RELAXED);
    }
    __atomic_store_n(&t->ctx_valid, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&t->paused, 0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_pause_gen, 1, __ATOMIC_ACQ_REL);
    fwake(&g_pause_gen);
    return RES_OK;
}

uint32_t kthread_get_context(uint32_t handle, void *out) {
    KThread *t = kobj_get(handle, KO_THREAD);
    if (!t) return RES_INVALID_HANDLE;
    if (!__atomic_load_n(&t->paused, __ATOMIC_ACQUIRE) || !t->ctx_valid) return RES_INVALID_STATE;
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
        __atomic_store_n(&g_bg_pause, 0, __ATOMIC_SEQ_CST);
        __atomic_add_fetch(&g_pause_gen, 1, __ATOMIC_ACQ_REL);
        fwake(&g_pause_gen);
        som_log("guest resumed");
    }
}

/* stall diagnostics: for each guest thread, its pause flags, the snapshot pc/lr when paused, and from /proc its Linux
 * state, the CPU time it gained over 200 ms (spinning vs blocked) and the syscall it sits in (nr, sp, pc) */
static int task_stat(int tid, char *state, unsigned long long *ticks) {
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = strrchr(buf, ')');
    if (!p) return 0;
    unsigned long long ut = 0, st = 0;
    /* after ") ": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime */
    if (sscanf(p + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", state, &ut, &st) != 3) return 0;
    *ticks = ut + st;
    return 1;
}

void kthread_debug_dump(void) {
    unsigned long long t0[128] = {0};
    for (int i = 0; i < 128; i++) {
        KThread *t = g_threads[i];
        char s;
        if (t && t->linux_tid) task_stat(t->linux_tid, &s, &t0[i]);
    }
    usleep(200000);
    som_log("stall: threads: npaused %d bg_pause %d gen %d", __atomic_load_n(&g_npaused, __ATOMIC_ACQUIRE), g_bg_pause,
            g_pause_gen);
    for (int i = 0; i < 128; i++) {
        KThread *t = g_threads[i];
        if (!t) continue;
        char s = '?', sc[160] = "";
        unsigned long long t1 = 0;
        if (t->linux_tid) {
            task_stat(t->linux_tid, &s, &t1);
            char path[64];
            snprintf(path, sizeof path, "/proc/self/task/%d/syscall", t->linux_tid);
            FILE *f = fopen(path, "r");
            if (f) {
                if (fgets(sc, sizeof sc, f)) sc[strcspn(sc, "\n")] = 0;
                fclose(f);
            }
        }
        char where[200] = "";
        if (t->ctx_valid) {
            uint64_t pc, lr, off1 = 0, off2 = 0;
            memcpy(&pc, t->ctx_buf + 0x100, 8);
            memcpy(&lr, t->ctx_buf + 0xF0, 8);
            const char *m1 = som_module_of(pc, &off1), *m2 = som_module_of(lr, &off2);
            snprintf(where, sizeof where, " snap pc=%s+0x%llx lr=%s+0x%llx", m1 ? m1 : "?", (unsigned long long)off1,
                     m2 ? m2 : "?", (unsigned long long)off2);
        }
        som_log("stall: thread id %llu handle 0x%x tid %d %s%s req %d paused %d ctx %d | %c +%llu ticks | svc 0x%llx | syscall %s%s",
                (unsigned long long)t->id, t->handle, t->linux_tid, t->started ? "" : "unstarted ", t->finished ? "finished " : "",
                t->pause_req, t->paused, t->ctx_valid, s, t1 - t0[i], t->ctx ? (unsigned long long)t->ctx->svc_num : 0ull, sc, where);
    }
}
