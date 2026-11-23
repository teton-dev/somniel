#include "debug_log.h"

#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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

/* ---- export of somniel.log to Download/somniel_logs (LogExporter.java) ---- */
void *SDL_GetAndroidJNIEnv(void);
void *SDL_GetAndroidActivity(void);

typedef struct { char prefix[48], reason[256]; } ExportJob;

static void *export_thread(void *arg) {
    ExportJob *j = (ExportJob *)arg;
    sleep(3); /* let the next few log lines (stats, retries, the stall dump) land in the copy */
    char path[600];
    snprintf(path, sizeof path, "%s/somniel.log", som_log_dir());
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    int ok = 0;
    if (env && act) {
        /* app classes are not visible to FindClass on a native thread: go through the activity's class loader */
        jclass ak = (*env)->GetObjectClass(env, act);
        jmethodID gcl = (*env)->GetMethodID(env, ak, "getClassLoader", "()Ljava/lang/ClassLoader;");
        jobject loader = gcl ? (*env)->CallObjectMethod(env, act, gcl) : NULL;
        jclass lk = loader ? (*env)->GetObjectClass(env, loader) : NULL;
        jmethodID lc = lk ? (*env)->GetMethodID(env, lk, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") : NULL;
        jstring cn = (*env)->NewStringUTF(env, "dev.teton.somniel.LogExporter");
        jclass ek = lc ? (jclass)(*env)->CallObjectMethod(env, loader, lc, cn) : NULL;
        if ((*env)->ExceptionCheck(env)) ek = NULL;
        jmethodID m = ek ? (*env)->GetStaticMethodID(env, ek, "exportLog",
                                                     "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V") : NULL;
        if (m) {
            jstring jp = (*env)->NewStringUTF(env, path), jf = (*env)->NewStringUTF(env, j->prefix),
                    jr = (*env)->NewStringUTF(env, j->reason);
            (*env)->CallStaticVoidMethod(env, ek, m, act, jp, jf, jr);
            ok = !(*env)->ExceptionCheck(env);
            (*env)->DeleteLocalRef(env, jp);
            (*env)->DeleteLocalRef(env, jf);
            (*env)->DeleteLocalRef(env, jr);
        }
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
        (*env)->DeleteLocalRef(env, cn);
        if (ek) (*env)->DeleteLocalRef(env, ek);
        if (lk) (*env)->DeleteLocalRef(env, lk);
        if (loader) (*env)->DeleteLocalRef(env, loader);
        (*env)->DeleteLocalRef(env, ak);
        (*env)->DeleteLocalRef(env, act);
    }
    som_log("log export (%s) %s", j->prefix, ok ? "handed to LogExporter" : "FAILED (no JNI/activity/class)");
    free(j);
    return NULL;
}

void som_export_log(const char *prefix, const char *reason) {
    ExportJob *j = calloc(1, sizeof *j);
    if (!j) return;
    snprintf(j->prefix, sizeof j->prefix, "%s", prefix);
    snprintf(j->reason, sizeof j->reason, "%s", reason);
    pthread_t t;
    if (pthread_create(&t, NULL, export_thread, j) == 0) pthread_detach(t);
    else free(j);
}
