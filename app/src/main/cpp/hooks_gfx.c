/* Graphics bring-up replacements: nv::*, nn::vi::* and nvnBootstrapLoader.
 *   nv::*      record the game's allocator callbacks and the graphics work memory
 *   nn::vi::*  display, layer and native window identities
 *   nvnBootstrapLoader  the first NVN call; M2 ends here, M3 answers it with the NVN function table
 * Structure follows engage-pc-recomp runtime/ov_gfx.c (MIT, see hook.h). */

#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "debug_log.h"
#include "hook.h"
#include "kobj.h"
#include "sync.h"
#include "nvn/host.h"
#include "nvn.h"
#include "nvn/gfx.h"

#define VI_R_INVALID 0xf601u

typedef struct {
    uint64_t alloc, free_fn, realloc_fn, user;
} NvAllocator;

static struct {
    NvAllocator graphics, devtools;
    uint64_t memory, memory_size;
    int initialized;
} g_nv;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void set_allocator(GuestCtx *c, NvAllocator *a) {
    pthread_mutex_lock(&g_lock);
    *a = (NvAllocator){arg_x(c, 0), arg_x(c, 1), arg_x(c, 2), arg_x(c, 3)};
    pthread_mutex_unlock(&g_lock);
}

static void gfx_graphics_allocator(GuestCtx *c) { set_allocator(c, &g_nv.graphics); }
static void gfx_devtools_allocator(GuestCtx *c) { set_allocator(c, &g_nv.devtools); }

static void gfx_initialize(GuestCtx *c) {
    uint64_t mem = arg_x(c, 0), size = arg_x(c, 1);
    if (!mem || !size) som_stop("nv::InitializeGraphics: no work memory (0x%llx, 0x%llx)", (unsigned long long)mem, (unsigned long long)size);
    pthread_mutex_lock(&g_lock);
    g_nv.memory = mem;
    g_nv.memory_size = size;
    g_nv.initialized = 1;
    pthread_mutex_unlock(&g_lock);
    som_log("nv: graphics work memory 0x%llx+0x%llx recorded", (unsigned long long)mem, (unsigned long long)size);
}

/* NvError NvOsDrvOpen(const char *path, int *fd): the NVIDIA multimedia stack (subsdk0: NvMM / OpenMAX, used by the
 * movie player) opens /dev/nvhost-nvdec and friends here. There is no video decoder yet: fail the open (2 =
 * NvError_NotSupported) instead of letting the SDK abort on its "nv::InitializeGraphics is required" check. */
static void nv_drv_open(GuestCtx *c) {
    const char *path = arg_p(c, 0);
    static int n;
    int32_t *fd = arg_p(c, 1);
    if (fd) *fd = -1;
    if (n++ < 20) som_log("nv: NvOsDrvOpen(\"%s\") refused: no hardware video decoder", path ? path : "(null)");
    ret_x(c, 2);
}

/* The rest of the NvOsDrv family: the callers do not always check the failed open, so every use of the (never
 * valid) fd fails the same way instead of reaching the SDK's uninitialized driver state. */
static void nv_drv_fail(GuestCtx *c) {
    static int n;
    if (n++ < 20) som_log("nv: NvOsDrv call on fd %d refused (lr 0x%llx)", (int)c->x[0], (unsigned long long)c->x[30]);
    ret_x(c, 2);
}
static void nv_drv_close(GuestCtx *c) { (void)c; }

static int g_vi_ready;
static void *g_display, *g_layer, *g_window;

static void vi_initialize(GuestCtx *c) {
    (void)c;
    pthread_mutex_lock(&g_lock);
    if (!g_display) {
        g_display = calloc(1, 64);
        g_layer = calloc(1, 64);
        g_window = calloc(1, 64);
    }
    g_vi_ready = 1;
    pthread_mutex_unlock(&g_lock);
}

static void vi_open_display(GuestCtx *c) {
    void **out = arg_p(c, 0);
    if (!out || !g_vi_ready) {
        ret_x(c, VI_R_INVALID);
        return;
    }
    *out = g_display;
    ret_x(c, 0);
}

static void vi_create_layer(GuestCtx *c) {
    void **out = arg_p(c, 0);
    if (!out || !g_vi_ready || arg_p(c, 1) != g_display) {
        ret_x(c, VI_R_INVALID);
        return;
    }
    *out = g_layer;
    ret_x(c, 0);
}

static void vi_native_window(GuestCtx *c) {
    void **out = arg_p(c, 0);
    if (!out || !g_vi_ready || arg_p(c, 1) != g_layer) {
        ret_x(c, VI_R_INVALID);
        return;
    }
    *out = g_window;
    ret_x(c, 0);
}

/* ---- system events and the vsync ticker ----
 * nn::os::SystemEventType is the SDK's own structure: the host makes the kernel event by calling the SDK's
 * CreateSystemEvent on the guest thread, keeps the writable handle, and signals it from any thread. */
static uint32_t g_vsync_wh;

uint32_t bind_system_event(Cpu *c, uint64_t ev, int autoclear) {
    uint64_t create = module_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    uint64_t getw = module_symbol("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE");
    if (!create || !getw) som_stop("system events: the SDK functions are not loaded");
    uint32_t r = (uint32_t)rt_call(c, create, 3, ev, (uint64_t)(autoclear ? 1 : 0), (uint64_t)1);
    if (r) som_stop("CreateSystemEvent failed: 0x%x", r);
    return (uint32_t)rt_call(c, getw, 1, ev);
}

static void *vsync_thread(void *arg) {
    (void)arg;
    struct timespec ts = {0, 16666667};
    for (;;) {
        nanosleep(&ts, NULL);
        KEvent *e = kobj_get(g_vsync_wh, KO_EVENT_W);
        if (e) kevent_signal(e);
    }
    return NULL;
}

/* Result nn::vi::GetDisplayVsyncEvent(os::SystemEventType *event, Display *display): auto-clear system event */
static void vi_vsync_event(GuestCtx *c) {
    uint64_t ev = arg_x(c, 0);
    if (!ev || !g_vi_ready || arg_p(c, 1) != g_display) {
        ret_x(c, VI_R_INVALID);
        return;
    }
    if (!g_vsync_wh) {
        g_vsync_wh = bind_system_event(c, ev, 1);
        pthread_t t;
        if (pthread_create(&t, NULL, vsync_thread, NULL) == 0) pthread_detach(t);
        som_log("vi: vsync event bound (writable handle 0x%x), ticking at 60 Hz", g_vsync_wh);
    }
    ret_x(c, 0);
}

/* void *nvnBootstrapLoader(const char *name): the first NVN call; answers with the NVN function table */
static void nvn_bootstrap(GuestCtx *c) {
    const char *name = arg_p(c, 0);
    static int n;
    if (n++ < 5) som_log("nvnBootstrapLoader(\"%s\")", name ? name : "(null)");
    ret_x(c, name ? nvn_resolve_proc(name) : 0);
}
void hooks_gfx_register(void) {
    nvn_procs_register(); /* the NVN proc table; stubs are made on demand */
    hook_register("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", gfx_graphics_allocator);
    hook_register("_ZN2nv18InitializeGraphicsEPvm", gfx_initialize);
    hook_register("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", gfx_devtools_allocator);
    hook_register("NvOsDrvOpen", nv_drv_open);
    hook_register("NvOsDrvClose", nv_drv_close);
    hook_register("NvOsDrvIoctl", nv_drv_fail);
    hook_register("NvOsDrvIoctl2", nv_drv_fail);
    hook_register("NvOsDrvIoctl3", nv_drv_fail);
    hook_register("NvOsDrvMapSharedMem", nv_drv_fail);
    hook_register("_Z17NvOsDrvQueryEventijPN2nn2os15SystemEventTypeE", nv_drv_fail);
    hook_register("_ZN2nn2vi10InitializeEv", vi_initialize);
    hook_register("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open_display);
    hook_register("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_create_layer);
    hook_register("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_native_window);
    hook_register("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_vsync_event);
    hook_register("nvnBootstrapLoader", nvn_bootstrap);
}



