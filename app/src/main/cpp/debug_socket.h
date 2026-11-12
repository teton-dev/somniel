#pragma once
/* Debug socket: abstract UNIX socket "somniel_debug", one JSON command per
 * line. `adb forward tcp:7777 localabstract:somniel_debug` exposes it. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SomStats {
    volatile unsigned long long frames;
    volatile double last_frame_ms;
    volatile double avg_fps;
} SomStats;

extern SomStats g_som_stats;

void som_debug_socket_start(void);

#ifdef __cplusplus
}
#endif
