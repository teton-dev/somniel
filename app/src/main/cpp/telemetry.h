#pragma once
/* Opt-in telemetry collection points (telemetry.c). */

void tel_set_gpu(const char *name, unsigned driver, unsigned vendor, unsigned api_major, unsigned api_minor);
void tel_perf_window(double secs, double presents, double max_gap_ms, unsigned over50, unsigned over100);
void tel_write_crash(const char *text);
