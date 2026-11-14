#pragma once
/* Opt-in telemetry collection points (telemetry.c). */
#include <stdint.h>

/* What the guest/replay was mostly doing during a frame that missed its goal (classified by vkr.c). */
enum { TEL_CAUSE_GUEST, TEL_CAUSE_CPU, TEL_CAUSE_GPU, TEL_CAUSE_PIPE, TEL_CAUSE_TEX, TEL_CAUSE_OTHER, TEL_CAUSE_N };

void tel_set_gpu(const char *name, unsigned driver, unsigned vendor, unsigned api_major, unsigned api_minor);
/* One 5 s window with its present count (dropped if the app was in the background during it). */
void tel_perf_window(double secs, double presents);
/* One presented frame: gap_ns since the previous present, hitch_ns = the goal (1.5 target periods), cause = TEL_CAUSE_*
 * of the time spent since the previous present, new_pipes/pipe_ns = pipeline builds in that frame. */
void tel_frame(uint64_t gap_ns, uint64_t hitch_ns, int cause, unsigned new_pipes, uint64_t pipe_ns, int target_fps);
/* The activity went to the background (0) or came back (1): frame gaps around it are not performance. */
void tel_set_foreground(int fg);
void tel_write_crash(const char *text);
