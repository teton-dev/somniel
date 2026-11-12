/* Live game settings: the frame rate (30, 40 or 60) and the render scale, both set from the sidebar and applied at once.
 *
 * Frame rate. Engage was written for 30 fps: its per-frame logic steps by "vsync ticks" of 1/60 s, two ticks a frame.
 * A frame rate F means 60/F ticks per frame (2, 1.5, 1), and everything the game ties to frames has to follow, or the
 * game runs fast or slow instead of smooth. This is the idea of the community "60 FPS (no Speedup)" patch for 2.0.0
 * (systemdev, zeusiota, dogethis; gamebanana.com/mods/432758; engage-pc-recomp's recomp/patches-engage-v200.txt, MIT
 * lists the same sites), made dynamic with host hooks instead of fixed instruction patches:
 *  - GameTime.get_VsyncFixedCount / get_VsyncFixedTime (what most per-frame code reads) return this frame's ticks. A
 *    GameTime.Update hook, once per frame on the main thread, works them out. 40 fps is 1.5 ticks a frame: the count
 *    alternates 1, 2, 1, 2 so game time still advances exactly 60 ticks per second (float readers see the same value).
 *  - QualitySettings.set_vSyncCount stores 2 at 30 fps and 1 otherwise (it also becomes the window's present interval,
 *    which the pacer ignores: the frame period comes from som_target_fps).
 *  - HubPlayerController uses HubUtil's speed, acceleration and turn rate as distances per update: scaled by 30/F for
 *    speed and turn rate, (30/F)^2 for acceleration and deceleration (60 fps: half and a quarter, as the community patch).
 *  - The present pacer (nvn_api_obj.c present_period) holds frames to 1/F seconds.
 * Render scale. CustomRP.Settings.SetRenderScale (the one place the game sets the URP asset's scale, from RenderManager's
 * Push/Pop stack) has its argument multiplied by the user's percentage. When the percentage changes, the per-frame hook
 * calls RenderManager.CommitScale, which re-applies the stack's top through the same hook, so the change shows up within
 * a frame and needs no restart.
 *
 * Every original instruction is checked first: one mismatch (another build, or a mod already patched it) and nothing is
 * changed (the settings then do nothing). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "debug_log.h"
#include "guest.h"
#include "hook.h"

uint64_t rt_call(GuestCtx *c, uint64_t fn, int n, ...); /* nvn_glue.c */

static const uint8_t kMainBuildId[16] = {0x8c, 0x08, 0xb9, 0x71, 0x9e, 0x08, 0x5f, 0x91,
                                         0x84, 0x7b, 0x5e, 0x0f, 0x93, 0x5d, 0x94, 0x88};

#define RVA_VSYNC_COUNT  0x0250cfb0u /* GameTime.get_VsyncFixedCount: mov x0, xzr */
#define RVA_VSYNC_TIME   0x0250cfc0u /* GameTime.get_VsyncFixedTime: stp x29, x30, [sp, #-0x20]! */
#define RVA_TIME_UPDATE  0x0250d8e0u /* GameTime.Update: str d8, [sp, #-0x40]! */
#define RVA_SET_VSYNC    0x032f92d4u /* QualitySettings.set_vSyncCount: mov w19, w0 */
#define RVA_SET_SCALE    0x021a8db0u /* CustomRP.Settings.SetRenderScale: str d8, [sp, #-0x30]! */
#define RVA_COMMIT_SCALE 0x02011ae0u /* RenderManager.CommitScale() */
#define RVA_GMAP_MOVE    0x02528830u /* GmapCinemachineController.Move(float speed): stp d9, d8, [sp, #-0x30]! */

typedef struct { uint32_t rva, orig; } Site;
static const Site kSites[] = {
    {RVA_VSYNC_COUNT, 0xaa1f03e0}, {RVA_VSYNC_TIME, 0xa9be7bfd}, {RVA_TIME_UPDATE, 0xfc1c0fe8},
    {RVA_SET_VSYNC, 0x2a0003f3},   {RVA_SET_SCALE, 0xfc1d0fe8},   {RVA_GMAP_MOVE, 0x6dbd23e9},
    /* HubUtil params getters: ldr s0, [x0, #off]; the hook sits on the instruction after it (an ldp) */
    {0x02a5e870, 0xbd402000}, {0x02a5e874, 0xa9414ff4}, {0x02a5e8d0, 0xbd402400}, {0x02a5e8d4, 0xa9414ff4},
    {0x02a5e930, 0xbd402800}, {0x02a5e934, 0xa9414ff4}, {0x02a5e990, 0xbd402c00}, {0x02a5e994, 0xa9414ff4},
};

/* ---- settings (set from any thread, read by hooks and the pacer) ---- */
static volatile int g_fps = 60;
static volatile int g_scale_pct = 100; /* 50..125 in steps of 5 */
static volatile int g_vsync_ticks = 1; /* ticks of 1/60 s the game advances this frame */
static volatile int g_scale_seen;      /* SetRenderScale has run once */
static volatile int g_scale_applied = 100;
static volatile float g_scale_base = 1.0f;
static uint64_t g_main_base;
int game_fps60_on;                     /* the hooks are installed: the settings take effect */

void som_set_frame_rate(int fps) {
    if (fps != 30 && fps != 40 && fps != 60) return;
    if (fps != g_fps) som_log("settings: frame rate %d fps", fps);
    g_fps = fps;
}

void som_set_render_scale(int pct) {
    if (pct < 50) pct = 50;
    if (pct > 125) pct = 125;
    pct = (pct + 2) / 5 * 5;
    if (pct != g_scale_pct) som_log("settings: render scale %d%%", pct);
    g_scale_pct = pct;
}

/* the frame period the present pacer holds to, in fps (0: the game's own present interval) */
int som_target_fps(void) { return game_fps60_on ? g_fps : 0; }

static void set_float(GuestCtx *c, int q, float v) {
    c->q[q] = 0;
    memcpy(&c->q[q], &v, sizeof v);
}

static float get_float(const GuestCtx *c, int q) {
    float v;
    memcpy(&v, &c->q[q], sizeof v);
    return v;
}

/* GameTime.Update, once per frame on the main thread */
static void on_frame(GuestCtx *c) {
    static int last_fps;
    static double acc;
    int fps = g_fps;
    if (fps != last_fps) last_fps = fps, acc = 0;
    acc += 60.0 / fps;
    int n = (int)(acc + 1e-9);
    if (n < 1) n = 1;
    acc -= n;
    g_vsync_ticks = n;
    if (g_scale_seen && g_scale_applied != g_scale_pct) { /* the slider moved: re-apply the stack's top scale */
        g_scale_applied = g_scale_pct;
        rt_call(c, g_main_base + RVA_COMMIT_SCALE, 1, (uint64_t)0);
    }
}

static void on_vsync_count(GuestCtx *c) { ret_x(c, g_vsync_ticks); }
static void on_vsync_time(GuestCtx *c) { set_float(c, 0, (float)g_vsync_ticks * (1.0f / 60.0f)); }
static void on_set_vsync(GuestCtx *c) { c->x[0] = g_fps == 30 ? 2 : 1; }

static void on_set_scale(GuestCtx *c) {
    int pct = g_scale_pct;
    float base = get_float(c, 0);
    g_scale_base = base;
    g_scale_seen = 1;
    g_scale_applied = pct;
    set_float(c, 0, base * (float)pct * 0.01f);
}

/* HubUtil.Params getters, after the load: s0 holds the value */
static void on_hub_linear(GuestCtx *c) { set_float(c, 0, get_float(c, 0) * (30.0f / (float)g_fps)); }
static void on_hub_accel(GuestCtx *c) {
    float k = 30.0f / (float)g_fps;
    set_float(c, 0, get_float(c, 0) * k * k);
}

void game_patches_apply(const NsoModule *mods, int nmods, Island *isl, int nisl) {
    const NsoModule *main_m = NULL;
    for (int i = 0; i < nmods; i++)
        if (!strcmp(mods[i].name, "main")) main_m = &mods[i];
    if (!main_m) return;
    if (memcmp(main_m->build_id, kMainBuildId, sizeof kMainBuildId)) {
        som_log("patches: frame rate and render scale skipped: main is not Engage 2.0.0 (build id differs)");
        return;
    }
    for (size_t i = 0; i < sizeof kSites / sizeof kSites[0]; i++) {
        uint32_t cur = *(const uint32_t *)(main_m->base + kSites[i].rva);
        if (cur != kSites[i].orig) {
            som_log("patches: skipped: main+%#x is %08x, expected %08x (another mod patched it?)", kSites[i].rva, cur,
                    kSites[i].orig);
            return;
        }
    }
    g_main_base = main_m->base;
    const uint64_t b = main_m->base;
    int ok = 1;
    ok &= hook_site(b + RVA_VSYNC_COUNT, "GameTime.VsyncFixedCount", on_vsync_count, 1, isl, nisl);
    ok &= hook_site(b + RVA_VSYNC_TIME, "GameTime.VsyncFixedTime", on_vsync_time, 1, isl, nisl);
    ok &= hook_site(b + RVA_TIME_UPDATE, "GameTime.Update", on_frame, 0, isl, nisl);
    ok &= hook_site(b + RVA_SET_VSYNC, "QualitySettings.set_vSyncCount", on_set_vsync, 0, isl, nisl);
    ok &= hook_site(b + RVA_SET_SCALE, "Settings.SetRenderScale", on_set_scale, 0, isl, nisl);
    ok &= hook_site(b + 0x02a5e874, "HubUtil.MaxSpeed", on_hub_linear, 0, isl, nisl);
    ok &= hook_site(b + 0x02a5e8d4, "HubUtil.Acceleration", on_hub_accel, 0, isl, nisl);
    ok &= hook_site(b + 0x02a5e934, "HubUtil.Deceleration", on_hub_accel, 0, isl, nisl);
    ok &= hook_site(b + 0x02a5e994, "HubUtil.TurnRate", on_hub_linear, 0, isl, nisl);
    /* world map walking: GmapCinemachineController.Move(speed) adds a fixed +-1 (+-3 fast) to the path position per call */
    ok &= hook_site(b + RVA_GMAP_MOVE, "GmapCinemachineController.Move", on_hub_linear, 0, isl, nisl);
    game_fps60_on = ok;
    som_log("patches: frame rate and render scale hooks %s (%d fps, %d%%)", ok ? "installed" : "FAILED (no island space?)", g_fps,
            g_scale_pct);
}
