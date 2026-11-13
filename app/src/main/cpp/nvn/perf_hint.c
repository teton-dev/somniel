/* ADPF performance hints (Android's APerformanceHint): each frame the renderer reports how long it took against the
 * frame-rate target, and on Android 15+ the GPU part separately, so the system can raise CPU and GPU clocks.
 *
 * Why: on the Galaxy A17 (Exynos 1330, Mali-G68 MP2) the GPU's "Interactive" governor held 312-403 MHz of its 949 MHz
 * while the title menu took ~44 ms of GPU time per 33 ms frame, with our submits back to back. The kernel's clock
 * controls are not readable or writable by apps; ADPF is the supported way to ask.
 *
 * On by default on Mali GPUs only (the Adreno build's behaviour stays as it was). <data>/perf_hint_on forces it on,
 * <data>/perf_hint_off off. The NDK calls are looked up at run time: the app's minSdk (29) predates them. */
#include "perf_hint.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/system_properties.h>
#include <unistd.h>

extern void som_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern const char *som_data_dir(void);
extern int som_target_fps(void); /* game_patches.c: the frame-rate target (0: the game's own) */

typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
typedef struct AWorkDuration AWorkDuration;

static struct {
    int state; /* 0 not tried, 1 on, -1 off */
    APerformanceHintSession *s;
    int64_t target;
    int (*report)(APerformanceHintSession *, int64_t);
    int (*update_target)(APerformanceHintSession *, int64_t);
    int (*report2)(APerformanceHintSession *, AWorkDuration *);
    AWorkDuration *(*wd_create)(void);
    void (*wd_start)(AWorkDuration *, int64_t);
    void (*wd_total)(AWorkDuration *, int64_t);
    void (*wd_cpu)(AWorkDuration *, int64_t);
    void (*wd_gpu)(AWorkDuration *, int64_t);
    AWorkDuration *wd;
    uint64_t n, last_log;
    double sum_total, sum_cpu, sum_gpu;
} H;

static int file_exists(const char *name) {
    char p[600];
    snprintf(p, sizeof p, "%s/%s", som_data_dir(), name);
    return access(p, F_OK) == 0;
}

static int is_mali(void) {
    char v[PROP_VALUE_MAX];
    if (__system_property_get("ro.hardware.vulkan", v) > 0 && strstr(v, "mali")) return 1;
    return __system_property_get("ro.hardware.egl", v) > 0 && strstr(v, "mali");
}

static int64_t target_ns(void) {
    int fps = som_target_fps();
    return 1000000000LL / (fps > 0 ? fps : 30);
}

/* called on the renderer thread: the session covers that thread */
static void init(void) {
    H.state = -1;
    int on = is_mali();
    if (file_exists("perf_hint_on")) on = 1;
    if (file_exists("perf_hint_off")) on = 0;
    if (!on) return;
    void *lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return;
    APerformanceHintManager *(*get_mgr)(void) = (APerformanceHintManager * (*)(void)) dlsym(lib, "APerformanceHint_getManager");
    APerformanceHintSession *(*create)(APerformanceHintManager *, const int32_t *, size_t, int64_t) =
        (APerformanceHintSession * (*)(APerformanceHintManager *, const int32_t *, size_t, int64_t)) dlsym(lib, "APerformanceHint_createSession");
    H.report = (int (*)(APerformanceHintSession *, int64_t))dlsym(lib, "APerformanceHint_reportActualWorkDuration");
    H.update_target = (int (*)(APerformanceHintSession *, int64_t))dlsym(lib, "APerformanceHint_updateTargetWorkDuration");
    if (!get_mgr || !create || !H.report) {
        som_log("perf hint: no ADPF on this system");
        return;
    }
    /* Android 15+: CPU and GPU durations reported apart */
    H.report2 = (int (*)(APerformanceHintSession *, AWorkDuration *))dlsym(lib, "APerformanceHint_reportActualWorkDuration2");
    H.wd_create = (AWorkDuration * (*)(void)) dlsym(lib, "AWorkDuration_create");
    H.wd_start = (void (*)(AWorkDuration *, int64_t))dlsym(lib, "AWorkDuration_setWorkPeriodStartTimestampNanos");
    H.wd_total = (void (*)(AWorkDuration *, int64_t))dlsym(lib, "AWorkDuration_setActualTotalDurationNanos");
    H.wd_cpu = (void (*)(AWorkDuration *, int64_t))dlsym(lib, "AWorkDuration_setActualCpuDurationNanos");
    H.wd_gpu = (void (*)(AWorkDuration *, int64_t))dlsym(lib, "AWorkDuration_setActualGpuDurationNanos");
    if (H.report2 && H.wd_create && H.wd_start && H.wd_total && H.wd_cpu && H.wd_gpu) H.wd = H.wd_create();
    APerformanceHintManager *m = get_mgr();
    int32_t tid = gettid();
    H.target = target_ns();
    H.s = m ? create(m, &tid, 1, H.target) : NULL;
    if (!H.s) {
        som_log("perf hint: no session (ADPF not supported here)");
        return;
    }
    H.state = 1;
    som_log("perf hint: ADPF session on the renderer thread, target %.1f ms, GPU durations %s", H.target / 1e6, H.wd ? "reported" : "not supported");
}

int perf_hint_active(void) {
    if (!H.state) init();
    return H.state > 0;
}

void perf_hint_frame(int64_t start_ns, int64_t total_ns, int64_t cpu_ns, int64_t gpu_ns) {
    if (!perf_hint_active() || total_ns <= 0) return;
    int64_t t = target_ns();
    if (t != H.target && H.update_target) H.update_target(H.s, t), H.target = t;
    if (cpu_ns <= 0) cpu_ns = 1; /* AWorkDuration wants positive values */
    if (H.wd && gpu_ns > 0) {
        H.wd_start(H.wd, start_ns);
        H.wd_total(H.wd, total_ns);
        H.wd_cpu(H.wd, cpu_ns);
        H.wd_gpu(H.wd, gpu_ns);
        H.report2(H.s, H.wd);
    } else {
        H.report(H.s, total_ns);
    }
    H.n++, H.sum_total += total_ns, H.sum_cpu += cpu_ns, H.sum_gpu += gpu_ns > 0 ? gpu_ns : 0;
    if (start_ns - (int64_t)H.last_log >= 5000000000LL) {
        if (H.last_log && H.n)
            som_log("perf hint: reported per frame: total %.1f ms, cpu %.1f ms, gpu %.1f ms (target %.1f ms)", H.sum_total / 1e6 / H.n,
                    H.sum_cpu / 1e6 / H.n, H.sum_gpu / 1e6 / H.n, H.target / 1e6);
        H.last_log = (uint64_t)start_ns, H.n = 0, H.sum_total = H.sum_cpu = H.sum_gpu = 0;
    }
}
