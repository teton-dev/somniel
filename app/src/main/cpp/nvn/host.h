/* Shim: the runtime services the ported NVN front end expects (engage-pc-recomp
 * runtime/host.h), mapped onto Somniel. */
#pragma once
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../debug_log.h"
#include "../guest.h"
#include "../hook.h"

typedef GuestCtx Cpu;
typedef void (*GuestFn)(Cpu *c);

#undef arg_x
#undef arg_p
#undef arg_s
#undef ret_x
#define LD64(a) (*(const uint64_t *)(uintptr_t)(a))
static inline uint64_t arg_x(const Cpu *c, int i) { return i < 8 ? c->x[i] : LD64(c->sp + 8 * (uint64_t)(i - 8)); }
static inline void *arg_p(const Cpu *c, int i) { return (void *)(uintptr_t)arg_x(c, i); }
static inline float arg_s(const Cpu *c, int i) { uint32_t b = (uint32_t)c->q[i]; float f; memcpy(&f, &b, 4); return f; }
static inline double arg_d(const Cpu *c, int i) { uint64_t b = (uint64_t)c->q[i]; double d; memcpy(&d, &b, 8); return d; }
static inline void ret_x(Cpu *c, uint64_t v) { c->x[0] = v; }
static inline void ret_s(Cpu *c, float f) { uint32_t b; memcpy(&b, &f, 4); c->q[0] = b; }
static inline void ret_d(Cpu *c, double d) { uint64_t b; memcpy(&b, &d, 8); c->q[0] = b; }

#define rt_log som_log
#define rt_logf som_log
#define rt_fatal(...) som_stop(__VA_ARGS__)
enum { RT_TRACE_SVC = 1, RT_TRACE_HOSTCALL = 2, RT_TRACE_IPC = 4, RT_TRACE_FS = 8, RT_TRACE_GFX = 16, RT_TRACE_PERF = 32,
       RT_TRACE_SCHED = 64, RT_TRACE_AUDIO = 128, RT_TRACE_PRESENT = 256 };
extern unsigned rt_trace;
#define RT_ENGAGE 1
#define RT_RESULT(module, desc) ((uint32_t)(module) | ((uint32_t)(desc) << 9))
#define RT_ADDRESS_LIMIT (1ull << 39)

/* The NVN front end was written for one virtual core at a time; guest threads run truly parallel here, so every NVN
 * call holds one global lock (nvn_glue.c). It is recursive per thread; kern_core_away() drops it completely while the
 * caller waits (NVN_BLOCKING) or calls back into the guest, kern_core_back() takes it again. */
void nvn_lock_enter(const char *fn);
void nvn_lock_leave(void);
int kern_core_away(void);
void kern_core_back(int *away);
uint64_t kern_now_ns(void);
static inline uint64_t rt_ticks(void) {
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

/* host -> guest call (guest_call.c): up to 8 integer arguments, returns x0 */
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...);

typedef struct { int boot; double seconds; const char *shot, *gpu; } RtConfig;
const RtConfig *rt_config(void);

/* pacing and movies: later milestones */
static inline int frame_step_realtime(void) { return 0; }
static inline uint64_t frame_step_cap_ns(void) { return 0; }
static inline void frame_step_present(uint64_t display_ns, int interval) { (void)display_ns, (void)interval; }
static inline void movie_tick(Cpu *c) { (void)c; }
static inline uint64_t perf_begin(void) { return 0; }
enum { PERF_PACE, PERF_FENCE, PERF_DRAIN, PERF_PIPE, PERF_XLATE, PERF_TEX, PERF_ASTC, PERF_N };
static inline void perf_end(int kind, uint64_t t0) { (void)kind, (void)t0; }
