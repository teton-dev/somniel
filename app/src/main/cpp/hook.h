#pragma once
/* Host replacements for SDK functions. A hooked guest function starts with a
 * branch into a stub that saves the guest context and calls a host function
 * with it; the host function reads its arguments from x0..x7 (and q0..q7 for
 * floats) and leaves its result in x0, then the guest returns to its caller.
 *
 * The API-level replacements follow the approach, and for the system services
 * the behaviour, of engage-pc-recomp runtime/ov_system.c and the MacNativeGames
 * TotK recomp it adapts (MIT, Copyright (c) 2026 MacNativeGames contributors). */

#include <stdint.h>

#include "guest.h"

typedef void (*HostFn)(GuestCtx *c);

#define arg_x(c, i) ((c)->x[i])
#define arg_p(c, i) ((void *)(uintptr_t)(c)->x[i])
#define arg_s(c, i) (*(const float *)&(c)->q[i])
#define ret_x(c, v) ((c)->x[0] = (uint64_t)(v))

void hook_register(const char *symbol, HostFn fn);
int hooks_install(const NsoModule *mods, int nmods, Island *isl, int nisl);
/* debugging: pass-through probes listed in path ("module rva name" per line); see hook.c */
int probes_install(const NsoModule *mods, int nmods, Island *isl, int nisl, const char *path);
int hook_site(uint64_t site, const char *name, HostFn fn, int replace, Island *isl, int nisl); /* hook.c */
void hook_dispatch(unsigned idx, GuestCtx *c);
const char *hook_name(unsigned idx);
const char *hook_current_name(void); /* the hook being dispatched on this thread */
uint64_t hook_register_dynamic(const char *name, HostFn fn);
uint64_t module_symbol(const char *name);

/* dynamic symbol lookup in a loaded, not yet relocated module */
typedef int (*SymCallback)(const char *name, uint64_t value, uint64_t size, void *user);
void nso_for_each_symbol(const NsoModule *m, SymCallback cb, void *user);

/* hook sets (hooks_*.c) */
void hooks_system_register(void);
void hooks_time_register(void);
void hooks_gfx_register(void);
void hooks_hid_register(void);
void hooks_movie_register(void);
void hooks_swkbd_register(void);
void nro_loader_register(void); /* nn::ro plugin loading (nro_loader.c) */





