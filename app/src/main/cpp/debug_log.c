#include "debug_log.h"

#include <android/log.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static FILE *g_log;
static FILE *g_perf;
static FILE *g_trace;
static char g_trace_path[512];
static long g_trace_bytes;
#define TRACE_MAX (64l << 20) /* rotate to svc_trace.old.log past this: a busy guest writes GBs otherwise */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_dir[400];

const char *som_log_dir(void) { return g_dir; }

void som_log_init(const char *dir) {
    char path[512];
    if (dir) snprintf(g_dir, sizeof g_dir, "%s", dir);
    pthread_mutex_lock(&g_lock);
    if (dir && !g_log) {
        snprintf(path, sizeof path, "%s/somniel.log", dir);
        g_log = fopen(path, "w");
        snprintf(path, sizeof path, "%s/svc_trace.on", dir);
        som_trace_on = access(path, F_OK) == 0;
        snprintf(path, sizeof path, "%s/nvn_timing", dir);
        som_nvn_timing_on = access(path, F_OK) == 0;
        snprintf(g_trace_path, sizeof g_trace_path, "%s/svc_trace.log", dir);
        g_trace = fopen(g_trace_path, "w");
        if (g_trace) setvbuf(g_trace, NULL, _IOFBF, 1 << 20);
        snprintf(path, sizeof path, "%s/perf", dir);
        mkdir(path, 0770);
        snprintf(path, sizeof path, "%s/perf/frametimes.csv", dir);
        g_perf = fopen(path, "w");
        if (g_perf) fputs("frame_ms,cpu_ms,gpu_ms\n", g_perf);
    }
    pthread_mutex_unlock(&g_lock);
}

void som_log(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    __android_log_write(ANDROID_LOG_INFO, "somniel", line);
    pthread_mutex_lock(&g_lock);
    if (g_log) {
        fprintf(g_log, "%s\n", line);
        fflush(g_log);
    }
    if (g_trace) fflush(g_trace); /* keep the trace current up to every log line (crash reports go through here) */
    pthread_mutex_unlock(&g_lock);
}

__thread int som_trace_quiet; /* svc.c sets it while a thread repeats the same call (polling loops) */

int som_trace_on;
int som_nvn_timing_on;

void som_trace_(const char *fmt, ...) {
    if (som_trace_quiet) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_lock);
    if (g_trace) {
        int n = fprintf(g_trace, "%s\n", line);
        if (n > 0) g_trace_bytes += n;
        if (g_trace_bytes > TRACE_MAX) {
            char old[600];
            fclose(g_trace);
            snprintf(old, sizeof old, "%.*s.old.log", (int)(strlen(g_trace_path) - 4), g_trace_path);
            rename(g_trace_path, old);
            g_trace = fopen(g_trace_path, "w");
            if (g_trace) setvbuf(g_trace, NULL, _IOFBF, 1 << 20);
            g_trace_bytes = 0;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void som_perf_frame(double frame_ms, double cpu_ms, double gpu_ms) {
    pthread_mutex_lock(&g_lock);
    if (g_perf) fprintf(g_perf, "%.3f,%.3f,%.3f\n", frame_ms, cpu_ms, gpu_ms);
    pthread_mutex_unlock(&g_lock);
}

void som_log_shutdown(void) {
    pthread_mutex_lock(&g_lock);
    if (g_log) fclose(g_log);
    if (g_perf) fclose(g_perf);
    if (g_trace) fclose(g_trace);
    g_log = g_perf = g_trace = NULL;
    pthread_mutex_unlock(&g_lock);
}
