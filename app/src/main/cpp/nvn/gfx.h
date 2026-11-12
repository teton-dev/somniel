// Interface of the graphics layer: window, shader service, back ends and the NVN front end.
#pragma once
#include <stdint.h>
#include "host.h"
#include "nvn_obj.h"

// ---- moved from ov.h
enum { OE_MSG_FOCUS_STATE_CHANGED = 15, OE_MSG_OPERATION_MODE_CHANGED = 30, OE_MSG_PERFORMANCE_MODE_CHANGED = 31 };
void oe_push_message(uint32_t message);                   // ov_system.c
void oe_set_focus(int in_focus);                          // ov_system.c: GetCurrentFocusState; TOTK_ASSUME_FOCUS pins 1
typedef struct { uint64_t alloc, free, realloc, user; } OvNvAllocator;
typedef struct { OvNvAllocator graphics, devtools; uint64_t memory, memory_size; int initialized; } OvNvState;
const OvNvState *ov_nv_state(void);                        // ov_gfx.c

// ---- GPU address window for CPU-invisible pools (section 3.1)
#define RT_GPU_VA_BASE 0x8000000000ull                     // 2^39, never mapped on the host
#define RT_GPU_VA_SIZE (64ull << 30)

// ---- window, vsync, focus (gfx_present.c and the window back end, gfx_wsys.h; vk_device.c). Host threads only; never run guest code.
void gfx_window_start(void);
void gfx_vsync_bind(uint64_t system_event);                // nn::vi vsync event; signalled at 60 Hz
uint64_t gfx_vsync_ticks(void);                            // ticks since start
uint64_t gfx_vsync_time_ns(uint64_t tick);                 // kern_now_ns() time of a tick (pacing)
double gfx_window_refresh_hz(void);                        // refresh rate of the window's monitor; 0 unknown
int gfx_window_focused(void);

// ---- back end (gfx_null.c, vk_*.c). Called by the NVN front end (nvn_api_*.c, nvn_ext.c) on guest threads.
typedef struct {
    const char *name;
    int  (*pool_init)(pool_t *p);                          // 0 fails MemoryPoolInitialize
    void (*pool_fini)(pool_t *p);
    int  (*pool_map_virtual)(pool_t *v, int n, const void *requests);   // NVNmappingRequest[n], 0x28 bytes each
    void (*texture_register)(int id, const tex_t *t, const view_t *v);
    void (*sampler_register)(int id, const samp_par *s);
    void (*texture_release)(const tex_t *t);
    void (*program_release)(const prog_t *p);
    uint64_t (*submit)(const uint64_t *handles, int n);    // replays; returns this submission's fence value
    int  (*fence_wait)(uint64_t value, uint64_t timeout_ns);   // 1 reached; caller has given its core up
    uint64_t (*fence_done)(void);                          // highest value whose side effects are written
    // Queues the present and returns at once; the host present goes out at at_ns (kern_now_ns clock), at once when that
    // has passed or at_ns is 0.
    void (*present)(const tex_t *t, const int crop[4], uint64_t at_ns);
    void (*finish)(void);
    void (*read_texels)(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row, uint64_t img);
    void (*write_texels)(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row, uint64_t img);
    void (*guest_written)(uint64_t addr, uint64_t size);   // host wrote pool storage (3.2)
    void (*guest_read)(uint64_t addr, uint64_t size);      // the CPU reads pool storage: GPU-written texels go there first
    // The CPU waited for fence: small render targets written by work up to it go to their storage (vk_texture.c
    // vk_texture_write_back_small).
    void (*cpu_synced)(uint64_t fence);
} GfxBackend;
extern const GfxBackend gfx_null_backend, gfx_vulkan_backend;
const GfxBackend *gfx_backend(void);                       // TOTK_GPU=null|vulkan (default vulkan)

// ---- NVN front end (nvn_procs.c)
void nvn_procs_register(void);                             // hostfn_register every proc; before hostfn_seal (ov_gfx.c)
uint64_t nvn_proc_address(const char *name);               // synthetic host address of a proc, 0 for an unknown name

// ---- statistics and frames (nvn_api_*.c, vk_*.c; boot.c prints them)
typedef struct { uint64_t frames, nvn_calls, nvn_distinct, unimplemented, validation_errors, shader_failures,
                 draws_skipped; } GfxStats;
GfxStats gfx_stats(void);
void rt_frame_presented(const tex_t *t, uint64_t n);       // boot.c: --frames N ends the run after the Nth present
                                                           // (saves --shot first through read_texels)

