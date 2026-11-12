#pragma once
/* Finds every NVN shader stage in the game's data on the device (C port of tools/unity_shaders.py and the scan in
 * tools/aot_shaders.py): Unity Shader objects in the romfs image's bundles and serialized files, and the engine's
 * built-in programs in exefs/main's read-only data. */
#include <stdint.h>

/* one stage: NVN control section (0x880 bytes; +0x700 c1 offset, +0x704 length, +0x714 stage) and its code.
 * Called from scan worker threads, possibly several at once; the buffers are valid only during the call. */
typedef void (*us_stage_fn)(void *ctx, uint32_t nvn_stage, const uint8_t *ctl, const uint8_t *code, uint32_t len);

typedef struct {
    unsigned files, candidates, checked, holders, unreadable, stages;
} UsStats;

/* Scans the packed romfs image (tools/pack_romfs.py layout) with nthreads workers. Bundles are read first, since
 * their Shader type trees serve data.unity3d, which has none. st->checked counts up while it runs. 0 on success. */
int us_scan_romfs(const char *romfs_path, int nthreads, us_stage_fn fn, void *ctx, UsStats *st);
/* the stages in an NSO's read-only data (exefs/main: blits, movie YUV conversion, one compute); count or -1 */
int us_scan_nso(const char *nso_path, us_stage_fn fn, void *ctx);
