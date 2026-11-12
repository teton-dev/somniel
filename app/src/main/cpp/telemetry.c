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
static struct {
    double secs, frames, max_gap_ms, min_fps;
    unsigned over50, over100, windows;
} g_p;
static volatile int g_lock;

static void lock(void) { while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) {} }
static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

void tel_set_gpu(const char *name, unsigned driver, unsigned vendor, unsigned api_major, unsigned api_minor) {
    lock();
    snprintf(g_gpu, sizeof g_gpu, "%s|0x%x|0x%x|%u.%u", name, driver, vendor, api_major, api_minor);
    unlock();
}

void tel_perf_window(double secs, double presents, double max_gap_ms, unsigned over50, unsigned over100) {
    if (presents <= 0 || secs <= 0) return; /* idle window (nothing presented): not a performance sample */
    double fps = presents / secs;
    lock();
    g_p.secs += secs;
    g_p.frames += presents;
    if (max_gap_ms > g_p.max_gap_ms) g_p.max_gap_ms = max_gap_ms;
    if (!g_p.windows || fps < g_p.min_fps) g_p.min_fps = fps;
    g_p.over50 += over50;
    g_p.over100 += over100;
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

/* {seconds, frames, longest gap ms, lowest 5 s window fps, over 50 ms, over 100 ms, windows}; resets the totals. */
JNIEXPORT jdoubleArray JNICALL Java_dev_teton_somniel_Telemetry_nativeTakePerf(JNIEnv *env, jclass cls) {
    lock();
    double v[7] = {g_p.secs, g_p.frames, g_p.max_gap_ms, g_p.min_fps, g_p.over50, g_p.over100, g_p.windows};
    memset(&g_p, 0, sizeof g_p);
    unlock();
    jdoubleArray a = (*env)->NewDoubleArray(env, 7);
    (*env)->SetDoubleArrayRegion(env, a, 0, 7, v);
    return a;
}
