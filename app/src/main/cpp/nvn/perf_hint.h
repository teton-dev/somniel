/* ADPF performance hints for the renderer thread (see perf_hint.c). */
#pragma once
#include <stdint.h>

/* 1 when a hint session is open (tried once, on the calling thread: the renderer's) */
int perf_hint_active(void);
/* one frame: its start (CLOCK_MONOTONIC ns), wall duration, CPU part and GPU part (0: not known) */
void perf_hint_frame(int64_t start_ns, int64_t total_ns, int64_t cpu_ns, int64_t gpu_ns);
