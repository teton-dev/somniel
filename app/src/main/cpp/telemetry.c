/* Native half of the opt-in telemetry (Telemetry.java does the consent, the batching and the upload).
 * This file only collects: GPU identity, frame-rate totals since the last upload, and a crash report file.
 * Nothing leaves the device from here; nothing is collected that identifies a person (no paths, no names). */
#include <jni.h>
#include <stdio.h>
#include <string.h>

#include "debug_log.h"
#include "telemetry.h"

static char g_gpu[256];

/* Totals since the last Telemetry.takePerf(). Written by the renderer thread, read by the Java thread: a small lock
 * is fine (once per 5 s window). */
#define HIST_N 512 /* 1 ms bins; the last one is "511 ms or more" */
#define PAUSE_GAP_NS 5000000000ull /* a longer gap between presents is a pause (lock screen, loading), not a hitch */
static struct {
    double secs, frames, min_fps;
    unsigned windows, windows_dropped;
    unsigned max_gap_ms;
    unsigned over50, over100, over250, over1000, long_gaps;
    unsigned hitches, cause[TEL_CAUSE_N];
    unsigned pipes;
    double pipe_ms;
    unsigned target_fps;
    unsigned counted; /* frames in hist */
    unsigned hist[HIST_N];
} g_p;
static volatile int g_lock;
static int g_fg = 1;       /* in the foreground (under the lock) */
static int g_skip = 0;     /* the next frame gap straddles a pause: ignore it */
static int g_win_dirty = 0; /* the current 5 s window straddles a pause: drop it */

static void lock(void) { while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) {} }
static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

void tel_set_foreground(int fg) {
    lock();
    g_fg = fg;
    g_skip = g_win_dirty = 1; /* both edges: the gap/window around them includes time with no rendering */
    unlock();
}

void tel_frame(uint64_t gap_ns, uint64_t hitch_ns, int cause, unsigned new_pipes, uint64_t pipe_ns, int target_fps) {
    if (!gap_ns) return; /* the first present: no previous one */
    lock();
    if (!g_fg || g_skip) { /* pause edge: this gap is not a frame time */
        g_skip = !g_fg ? g_skip : 0;
        unlock();
        return;
    }
    if (gap_ns > PAUSE_GAP_NS) { /* counted on its own: a true 5 s+ freeze is visible here but does not wreck the stats */
        g_p.long_gaps++;
        unlock();
        return;
    }
    unsigned ms = (unsigned)(gap_ns / 1000000ull);
    if (ms > g_p.max_gap_ms) g_p.max_gap_ms = ms;
    g_p.hist[ms >= HIST_N ? HIST_N - 1 : ms]++;
    g_p.counted++;
    if (gap_ns > 50000000ull) g_p.over50++;
    if (gap_ns > 100000000ull) g_p.over100++;
    if (gap_ns > 250000000ull) g_p.over250++;
    if (gap_ns > 1000000000ull) g_p.over1000++;
    if (hitch_ns && gap_ns > hitch_ns) {
        g_p.hitches++;
        g_p.cause[cause < 0 || cause >= TEL_CAUSE_N ? TEL_CAUSE_OTHER : cause]++;
    }
    g_p.pipes += new_pipes;
    g_p.pipe_ms += (double)pipe_ns / 1e6;
    g_p.target_fps = target_fps > 0 ? (unsigned)target_fps : 0;
    unlock();
}

void tel_set_gpu(const char *name, unsigned driver, unsigned vendor, unsigned api_major, unsigned api_minor) {
    lock();
    snprintf(g_gpu, sizeof g_gpu, "%s|0x%x|0x%x|%u.%u", name, driver, vendor, api_major, api_minor);
    unlock();
}

void tel_perf_window(double secs, double presents) {
    if (presents <= 0 || secs <= 0) return; /* idle window (nothing presented): not a performance sample */
    double fps = presents / secs;
    lock();
    if (!g_fg || g_win_dirty) { /* the window touched a background period */
        g_p.windows_dropped++;
        g_win_dirty = !g_fg; /* a window that starts after the resume is clean again */
        unlock();
        return;
    }
    g_p.secs += secs;
    g_p.frames += presents;
    if (!g_p.windows || fps < g_p.min_fps) g_p.min_fps = fps;
    g_p.windows++;
    unlock();
}

/* A guest or host crash: one short text file the next launch uploads (if the user opted in) and deletes. Only the
 * signal, the module+offset of each frame and the GPU. No register values, no memory, no paths. */
void tel_write_crash(const char *text) {
    const char *dir = som_log_dir();
    if (!dir[0]) return;
    char path[512];
    snprintf(path, sizeof path, "%s/crash_report.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

JNIEXPORT jstring JNICALL Java_dev_teton_somniel_Telemetry_nativeGpu(JNIEnv *env, jclass cls) {
    char tmp[256];
    lock();
    memcpy(tmp, g_gpu, sizeof tmp);
    unlock();
    return (*env)->NewStringUTF(env, tmp);
}

/* Upper edge (ms) of the histogram bin that holds the p-th fraction of the counted frames. */
static double percentile(double p) {
    if (!g_p.counted) return 0;
    unsigned need = (unsigned)(p * g_p.counted + 0.999999), cum = 0;
    if (need < 1) need = 1;
    for (int i = 0; i < HIST_N; i++) {
        cum += g_p.hist[i];
        if (cum >= need) return i + 1;
    }
    return HIST_N;
}

/* Layout shared with Telemetry.java (P_* indices); resets the totals. */
enum {
    V_SECS, V_FRAMES, V_MAX_GAP, V_MIN_FPS, V_WINDOWS, V_OVER50, V_OVER100, V_OVER250, V_OVER1000, V_LONG_GAPS,
    V_P50, V_P95, V_P99, V_P999, V_HITCHES, V_CAUSE0, /* ..V_CAUSE0+TEL_CAUSE_N-1 */
    V_PIPES = V_CAUSE0 + TEL_CAUSE_N, V_PIPE_MS, V_WIN_DROPPED, V_TARGET_FPS, V_COUNTED, V_N
};

JNIEXPORT jdoubleArray JNICALL Java_dev_teton_somniel_Telemetry_nativeTakePerf(JNIEnv *env, jclass cls) {
    double v[V_N] = {0};
    lock();
    v[V_SECS] = g_p.secs, v[V_FRAMES] = g_p.frames, v[V_MAX_GAP] = g_p.max_gap_ms, v[V_MIN_FPS] = g_p.min_fps;
    v[V_WINDOWS] = g_p.windows, v[V_OVER50] = g_p.over50, v[V_OVER100] = g_p.over100, v[V_OVER250] = g_p.over250;
    v[V_OVER1000] = g_p.over1000, v[V_LONG_GAPS] = g_p.long_gaps;
    v[V_P50] = percentile(0.50), v[V_P95] = percentile(0.95), v[V_P99] = percentile(0.99), v[V_P999] = percentile(0.999);
    v[V_HITCHES] = g_p.hitches;
    for (int k = 0; k < TEL_CAUSE_N; k++) v[V_CAUSE0 + k] = g_p.cause[k];
    v[V_PIPES] = g_p.pipes, v[V_PIPE_MS] = g_p.pipe_ms, v[V_WIN_DROPPED] = g_p.windows_dropped;
    v[V_TARGET_FPS] = g_p.target_fps, v[V_COUNTED] = g_p.counted;
    memset(&g_p, 0, sizeof g_p);
    unlock();
    jdoubleArray a = (*env)->NewDoubleArray(env, V_N);
    (*env)->SetDoubleArrayRegion(env, a, 0, V_N, v);
    return a;
}
