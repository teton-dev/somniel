/* Glue for the ported NVN front end: the services the base runtime provided,
 * rebuilt on Somniel (see nvn/host.h). */

#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "debug_log.h"
#include "hook.h"
#include "nvn/gfx.h"
#include "nvn/host.h"
#include "thread.h"
#include "guest.h"

unsigned rt_trace;

const char *cfg_get(const char *name) {
    (void)name;
    return NULL;
}
int cfg_bool(const char *name) {
    (void)name;
    return 0;
}

const RtConfig *rt_config(void) {
    static const RtConfig cfg = {0, 0.0, NULL, "null"}; /* the Vulkan backend replaces "null" later */
    return &cfg;
}

uint64_t kern_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void rt_frame_presented(const tex_t *t, uint64_t n) {
    (void)t;
    if (n == 1 || n % 600 == 0) som_log("nvn: present #%llu", (unsigned long long)n);
}

/* ---- the NVN lock (see nvn/host.h) ---- */

static pthread_mutex_t g_nvn_mu = PTHREAD_MUTEX_INITIALIZER;
static __thread int t_nvn_depth;
static const char *volatile g_nvn_holder_fn; /* diagnostics: what the holder is doing */
static volatile int g_nvn_holder_tid = -1;

static int my_tid(void) { return t_ctx ? t_ctx->id : -1; }

static volatile uint64_t g_wd_epoch = 1; /* counts the watchdog's 10 s rounds: a clock for g_in_since without a clock read per call */

static void take(const char *fn) {
    if (pthread_mutex_trylock(&g_nvn_mu) == 0) { /* the usual case: nobody else is in NVN */
        g_nvn_holder_tid = my_tid();
        g_nvn_holder_fn = fn;
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5;
    if (pthread_mutex_timedlock(&g_nvn_mu, &ts) != 0) {
        som_log("nvn lock: t%d (%s) waited 5 s; held by t%d in %s", my_tid(), fn ? fn : "?", g_nvn_holder_tid,
                g_nvn_holder_fn ? g_nvn_holder_fn : "?");
        pthread_mutex_lock(&g_nvn_mu);
    }
    g_nvn_holder_tid = my_tid();
    g_nvn_holder_fn = fn;
}

/* watchdog: which NVN call each guest thread is in, logged when one does not return for 10 s */
#define WD_THREADS 128
static const char *volatile g_in_fn[WD_THREADS];
static volatile uint64_t g_in_since[WD_THREADS];
static pthread_once_t g_wd_once = PTHREAD_ONCE_INIT;

static GuestCtx *volatile g_ctx_of[WD_THREADS];
extern volatile uint64_t g_vkr_last_submit_ns;

static int readable(uint64_t a, uint64_t n) { return a >= AS_BASE && a + n < AS_END && !(a & 7); }

static void dump_words(const char *what, uint64_t a, unsigned n) {
    if (!readable(a, n * 4)) { som_log("stall: %s 0x%llx unreadable", what, (unsigned long long)a); return; }
    const uint32_t *w = (const uint32_t *)a;
    for (unsigned i = 0; i < n; i += 8)
        som_log("stall: %s+0x%03x: %08x %08x %08x %08x %08x %08x %08x %08x", what, i * 4, w[i], w[i + 1], w[i + 2], w[i + 3],
                w[i + 4], w[i + 5], w[i + 6], w[i + 7]);
}

/* The AcquireSemaphore frame whose return address is main+lr_off holds the caller's x19 at fp+0x28. */
static uint64_t caller_x19(GuestCtx *c, uint64_t lr_off) {
    uint64_t fp = c->x[29], off;
    for (int i = 0; i < 16 && readable(fp, 16); i++) {
        uint64_t next = ((uint64_t *)fp)[0], lr = ((uint64_t *)fp)[1];
        const char *m = som_module_of(lr, &off);
        if (m && !strcmp(m, "main") && off == lr_off) return ((uint64_t *)fp)[5];
        if (next <= fp) break;
        fp = next;
    }
    return 0;
}

void sync_debug_dump(void);
static uint64_t main_base_of(GuestCtx *c) {
    uint64_t fp = c->x[29], off;
    for (int i = 0; i < 16 && readable(fp, 16); i++) {
        uint64_t next = ((uint64_t *)fp)[0], lr = ((uint64_t *)fp)[1];
        const char *m = som_module_of(lr, &off);
        if (m && !strcmp(m, "main")) return lr - off;
        if (next <= fp) break;
        fp = next;
    }
    return 0;
}

void nvn_wd_register(GuestCtx *c) {
    if (c->id >= 0 && c->id < WD_THREADS && g_ctx_of[c->id] != c) g_ctx_of[c->id] = c;
}

static void log_chain(int id, GuestCtx *c) {
    char line[700];
    uint64_t off, fp = c->x[29];
    const char *m = som_module_of(c->x[30], &off);
    int n = snprintf(line, sizeof line, "stall: t%d lr=%s+0x%llx", id, m ? m : "?", (unsigned long long)off);
    for (int i = 0; i < 14 && readable(fp, 16) && n < (int)sizeof line - 40; i++) {
        uint64_t next = ((uint64_t *)fp)[0], lr = ((uint64_t *)fp)[1];
        m = som_module_of(lr, &off);
        n += snprintf(line + n, sizeof line - (size_t)n, " <- %s+0x%llx", m ? m : "?", (unsigned long long)off);
        if (next <= fp) break;
        fp = next;
    }
    som_log("%s", line);
}

static void stall_dump(void) {
    som_log("stall: no submit for 8 s, dumping");
    sync_debug_dump();
    for (int i = 0; i < WD_THREADS; i++) {
        GuestCtx *c = g_ctx_of[i];
        if (!c) continue;
        if (i == 33 || i == 0) log_chain(i, c);
        {   /* CRI server spinning in its output poll: the drain function (main+0x177dd18) saved its caller's x19
             * (the CRI output device) at fp-0x8, where fp is its frame record (return address main+0x1752080) */
            for (int tries = 0; tries < 2000; tries++) {
            int found = 0;
            uint64_t fp = c->x[29], off;
            for (int k = 0; k < 16 && readable(fp, 16); k++) {
                uint64_t next = ((uint64_t *)fp)[0], lr = ((uint64_t *)fp)[1];
                const char *m = som_module_of(lr, &off);
                if (m && !strcmp(m, "main") && off == 0x1752080) {
                    uint64_t dev = ((uint64_t *)fp)[-1];
                    som_log("stall: t%d CRI device 0x%llx", i, (unsigned long long)dev);
                    dump_words("cri device", dev, 0x60);
                    if (readable(dev + 0x138, 8)) {
                        uint64_t out = *(uint64_t *)(dev + 0x138);
                        som_log("stall: CRI output 0x%llx vtable 0x%llx", (unsigned long long)out,
                                readable(out, 8) ? (unsigned long long)*(uint64_t *)out : 0ull);
                        dump_words("cri output", out, 0x30);
                    }
                    found = 1;
                    break;
                }
                if (next <= fp) break;
                fp = next;
            }
            if (found) break;
            }
        }
        uint64_t mb = main_base_of(c);
        if (mb) {
            som_log("stall: t%d svc x0..x3 %llx %llx %llx %llx", i, (unsigned long long)c->x[0], (unsigned long long)c->x[1],
                    (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
            static int once;
            if (!once++) {
                dump_words("vsync event main+0x6600090", mb + 0x6600090, 0x10);
                dump_words("vsync tick main+0x66002a0", mb + 0x66002a0, 4);
            }
        }
    }
    for (int i = 0; i < WD_THREADS; i++) {
        GuestCtx *c = g_ctx_of[i];
        if (!c) continue;
        uint64_t s = caller_x19(c, 0xab5fec);
        if (s) {
            som_log("stall: t%d waits on stream 0x%llx", i, (unsigned long long)s);
            dump_words("stream", s, 0x60);
            uint64_t sem = *(uint64_t *)(s + 0x28), wsem = *(uint64_t *)(s + 0x20);
            if (readable(sem, 0x10)) dump_words("read sem", sem, 8);
            if (readable(wsem, 0x10)) dump_words("write sem", wsem, 8);
        }
        uint64_t d = caller_x19(c, 0x9efd4c);
        if (d) {
            som_log("stall: t%d waits for present on device 0x%llx", i, (unsigned long long)d);
            uint64_t o = readable(d + 0x24c0, 8) ? *(uint64_t *)(d + 0x24c0) : 0;
            som_log("stall: device+0x24c0 = 0x%llx, flag+0x2790 = %u", (unsigned long long)o,
                    readable(d + 0x2790, 8) ? *(uint8_t *)(d + 0x2790) : 0xff);
            if (o) dump_words("present obj", o, 0x60);
        }
    }
}

static void *watchdog(void *arg) {
    (void)arg;
    int dumped = 0;
    extern volatile int g_bg_pause; /* thread.c: app in the background, guest parked on purpose */
    for (;;) {
        static int was_paused;
        if (g_bg_pause || was_paused) g_vkr_last_submit_ns = kern_now_ns(); /* a lock is not a stall */
        was_paused = g_bg_pause;
        if (!dumped && !g_bg_pause && g_vkr_last_submit_ns && kern_now_ns() - g_vkr_last_submit_ns > 8000000000ull) {
            dumped = 1;
            stall_dump();
        }
        struct timespec d = {10, 0};
        nanosleep(&d, NULL);
        uint64_t now = ++g_wd_epoch;
        for (int i = 0; i < WD_THREADS; i++) {
            const char *fn = g_in_fn[i];
            uint64_t since = g_in_since[i];
            if (fn && since && now - since >= 2 && !g_bg_pause)
                som_log("nvn watchdog: t%d in %s for %llu s (lock held by t%d in %s)", i, fn,
                        (unsigned long long)((now - since) * 10), g_nvn_holder_tid,
                        g_nvn_holder_fn ? g_nvn_holder_fn : "-");
        }
    }
    return NULL;
}

static void wd_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, watchdog, NULL) == 0) pthread_detach(t);
}

/* Time spent in each NVN API call (all guest threads), so a hitch can be blamed on a call. The table is written with the
 * NVN lock held (enter after take, leave before unlock) and keyed by the call-name pointer (string literals). */
#define NT_SLOTS 512
static struct { const char *fn; uint64_t n, ns, wait; } g_nt[NT_SLOTS];
static __thread uint64_t t_nt_start, t_nt_wait;
static __thread const char *t_nt_fn;

static void nt_record(const char *fn, uint64_t ns, uint64_t wait) {
    uint32_t h = (uint32_t)(((uintptr_t)fn >> 3) * 2654435761u) % NT_SLOTS;
    for (int probe = 0; probe < NT_SLOTS; probe++, h = (h + 1) % NT_SLOTS) {
        if (g_nt[h].fn == fn || !g_nt[h].fn) {
            g_nt[h].fn = fn;
            g_nt[h].n++, g_nt[h].ns += ns, g_nt[h].wait += wait;
            return;
        }
    }
}

/* The calls that took the most time since the previous call of this function, "Name ms(calls, +lock wait ms)". Call it
 * once per presented frame; the text is only built when out is given. */
void nvn_time_report(char *out, size_t cap) {
    static uint64_t p_ns[NT_SLOTS], p_n[NT_SLOTS], p_w[NT_SLOTS];
    size_t o = 0;
    if (out && cap) out[0] = 0;
    for (int k = 0; out && k < 6; k++) {
        int best = -1;
        uint64_t bd = 0;
        for (int i = 0; i < NT_SLOTS; i++) {
            uint64_t d = g_nt[i].ns - p_ns[i];
            if (g_nt[i].fn && d > bd) bd = d, best = i;
        }
        if (best < 0 || bd < 1000000 || o + 80 >= cap) break;
        o += (size_t)snprintf(out + o, cap - o, " %s %.1f(%llu, wait %.1f)", g_nt[best].fn, (double)bd / 1e6,
                              (unsigned long long)(g_nt[best].n - p_n[best]), (double)(g_nt[best].wait - p_w[best]) / 1e6);
        p_ns[best] = g_nt[best].ns; /* not picked again */
    }
    for (int i = 0; i < NT_SLOTS; i++) p_ns[i] = g_nt[i].ns, p_n[i] = g_nt[i].n, p_w[i] = g_nt[i].wait;
}

void nvn_lock_enter(const char *fn) {
    int id = my_tid();
    if (t_nvn_depth == 0 && id >= 0 && id < WD_THREADS) {
        pthread_once(&g_wd_once, wd_start);
        g_ctx_of[id] = t_ctx;
        g_in_since[id] = g_wd_epoch;
        g_in_fn[id] = fn;
    }
    if (t_nvn_depth++ == 0) {
        uint64_t a = kern_now_ns();
        take(fn);
        t_nt_start = kern_now_ns();
        t_nt_wait = t_nt_start - a;
        t_nt_fn = fn;
    }
}

void nvn_lock_leave(void) {
    if (--t_nvn_depth == 0) {
        if (t_nt_fn) nt_record(t_nt_fn, kern_now_ns() - t_nt_start, t_nt_wait);
        g_nvn_holder_fn = NULL;
        pthread_mutex_unlock(&g_nvn_mu);
        int id = my_tid();
        if (id >= 0 && id < WD_THREADS) g_in_fn[id] = NULL;
    }
}

int kern_core_away(void) {
    int d = t_nvn_depth;
    if (d) {
        t_nvn_depth = 0;
        pthread_mutex_unlock(&g_nvn_mu);
    }
    return d;
}

void kern_core_back(int *away) {
    if (*away) {
        take("(back from a wait)");
        t_nvn_depth = *away;
    }
}

/* ---- host -> guest calls ---- */

static void call_return_hook(Cpu *c) { som_call_return(c); }

uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) {
    static uint64_t ret_stub;
    if (!ret_stub) ret_stub = hook_register_dynamic("host-call-return", call_return_hook);
    if (n < 0 || n > 8 || !ret_stub) som_stop("rt_call: %d arguments", n);
    uint64_t args[8] = {0};
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) args[i] = va_arg(ap, uint64_t);
    va_end(ap);

    /* kernel calls made by the callee overwrite the saved registers: keep ours */
    GuestCtx saved = *c;
    uint64_t sp = (c->sp - 0x1000) & ~0xFull;
    int away = kern_core_away(); /* the guest callee may wait for another thread that needs NVN */
    uint64_t r = som_call_guest(c, fn, sp, args, ret_stub);
    kern_core_back(&away);
    memcpy(c->x, saved.x, sizeof c->x);
    c->sp = saved.sp;
    c->nzcv = saved.nzcv;
    c->ret_addr = saved.ret_addr;
    c->svc_num = saved.svc_num;
    memcpy(c->q, saved.q, sizeof c->q);
    return r;
}
