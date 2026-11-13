#pragma once
/* Logging to logcat plus somniel.log in the app's internal storage, and the
 * frame-time CSV the perf scripts read. Safe to call from any thread. */

#ifdef __cplusplus
extern "C" {
#endif

void som_log_init(const char *dir);
const char *som_log_dir(void); /* the internal files directory given to som_log_init ("" before it): somniel.log lives there */
void som_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* svc_trace.log only. Off unless <internal files>/svc_trace.on exists at start (adb shell run-as dev.teton.somniel
 * touch files/svc_trace.on): formatting and writing every kernel call under one lock slows all guest threads. */
extern int som_trace_on;
extern int som_nvn_timing_on; /* <data>/nvn_timing exists: time every NVN call (3 clock reads per call: costly) */
void som_trace_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define som_trace(...) do { if (__builtin_expect(som_trace_on, 0)) som_trace_(__VA_ARGS__); } while (0)
void som_perf_frame(double frame_ms, double cpu_ms, double gpu_ms);
void som_log_shutdown(void);

#ifdef __cplusplus
}
#endif
