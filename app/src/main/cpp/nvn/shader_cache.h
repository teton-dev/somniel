#pragma once
/* On-device shader translation and the one-file shader cache, <data>/shaders.cache (see shader_cache.c). */
#include <stddef.h>
#include <stdint.h>

/* The translation of a stage by its renderer key (vkr.c shader_load1): "<code hash>" (plain draw state),
 * "<code hash>_t<slot><type>..." (types of the textures it asked about) or "<code hash>_<answers hash>" (the current
 * draw state). Looks in the cache; on a miss, translates the stage bound for the current draw (gfx_null.c ts state)
 * with libxlate.so and appends it. Returns 1 with malloc'd *spv (*spv_len bytes) and *res (NUL-terminated res.json). */
int sc_get(const char *key, int stage, uint8_t **spv, size_t *spv_len, char **res);

/* Starts the background "prepare all" pass: every shader stage in romfs.bin and exefs/main is translated into the
 * cache, once per game data (a "!prepared" record marks it done). <data>/no_prepare turns it off. Call once the
 * data directory is set; returns at once. */
void sc_prepare_start(void);
