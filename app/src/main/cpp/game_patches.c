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
#define RVA_SET_LODBIAS  0x032f9150u /* QualitySettings.set_lodBias(float): str d8, [sp, #-0x20]! (hooked) */
#define RVA_SET_MAXLOD   0x032f91e0u /* QualitySettings.set_maximumLODLevel(int): stp x29, x30, [sp, #-0x20]! (called) */
#define RVA_GET_RPASSET  0x02c508f0u /* GraphicsSettings.get_renderPipelineAsset(): stp x29, x30, [sp, #-0x20]! (called) */
/* Offscreen character views (the unit info window's 3D model, and other CustomOffscreenCamera users): the camera
 * renders into its target texture through UniversalRenderPipeline.RenderSingleCamera, called straight from
 * CustomOffscreenCamera.CustomBeginFrameRendering, so the render scale setting never reached it (about 18 ms a frame of
 * GPU on the Galaxy A17 while a unit is hovered). The hook sits on the `mov x0, x19` before that tail call (x20: the
 * camera) and gives the camera's target texture the render scale's share of its original size, releasing it first
 * when it already exists (Unity recreates it at the new size when the camera draws). */
#define RVA_OFFSCREEN_RENDER 0x02a4d784u /* CustomOffscreenCamera.CustomBeginFrameRendering: mov x0, x19 */
#define RVA_CAM_TARGET   0x02c3b430u /* Camera.get_targetTexture (called) */
#define RVA_CAM_PIXEL_W  0x02c3b2f0u /* Camera.get_pixelWidth (called) */
#define RVA_CAM_PIXEL_H  0x02c3b340u /* Camera.get_pixelHeight (called) */
#define RVA_RT_RELEASE   0x02f85b30u /* RenderTexture.Release (called) */
/* PostProcessPass.RenderFinalPass: `ldr w8, [x20, #0x144]`, x20 = CameraData, +0x144 its antialiasing mode (1 FXAA,
 * 3 CustomFxaa, 4 CustomFxaa_IgnoreChara all enable the _FXAA keyword; the battle map uses 3). The final pass (the
 * upscale to the window, with the UI drawn over it) then reads the picture 9 times a pixel: with it off the final pass
 * went from 10.8% to 5.0% of the GPU frame on the Galaxy A17 (2026-11-12). The Anti-aliasing option (GFX_AA) sets the
 * mode to None (0) just before the read. */
#define RVA_FINAL_AA     0x0319880cu
#define RVA_RT_GET_W     0x02f84a20u /* RenderTexture.get_width (called) */
#define RVA_RT_SET_W     0x02f84a70u /* RenderTexture.set_width (called) */
#define RVA_RT_GET_H     0x02f84ac0u /* RenderTexture.get_height (called) */
#define RVA_RT_SET_H     0x02f84b10u /* RenderTexture.set_height (called) */
#define RVA_RT_CREATED   0x02f85b80u /* RenderTexture.IsCreated (called) */

typedef struct { uint32_t rva, orig; } Site;
static const Site kSites[] = {
    {RVA_VSYNC_COUNT, 0xaa1f03e0}, {RVA_VSYNC_TIME, 0xa9be7bfd}, {RVA_TIME_UPDATE, 0xfc1c0fe8},
    {RVA_SET_VSYNC, 0x2a0003f3},   {RVA_SET_SCALE, 0xfc1d0fe8},   {RVA_GMAP_MOVE, 0x6dbd23e9},
    {RVA_SET_LODBIAS, 0xfc1e0fe8}, {RVA_SET_MAXLOD, 0xa9be7bfd},  {RVA_GET_RPASSET, 0xa9be7bfd},
    {RVA_OFFSCREEN_RENDER, 0xaa1303e0}, {RVA_FINAL_AA, 0xb9414688},
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

int som_handheld_profile(void);  /* hooks_system.c */
static volatile int g_fps_from_ui; /* the sidebar has sent its (saved or default) frame rate */

void som_set_frame_rate(int fps) {
    if (fps != 30 && fps != 40 && fps != 60) return;
    g_fps_from_ui = 1;
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

/* ---- graphics options (sidebar), 0 everywhere = exactly as the game renders ----
 * GFX_SHADOWS  0 High (the game's own), 1 Medium (shadow maps at half resolution), 2 Low (a quarter), 3 Off.
 *              Applied every frame on the URP asset's fields (UniversalRenderPipelineAsset 2.0.0 offsets), keeping the
 *              game's own per-scene values (ApplyCustomShadowData rewrites them) so High restores them exactly.
 * GFX_MODEL    removed (always 0, a stored value is ignored): scaling QualitySettings.lodBias (Reduced x0.6) or capping
 *              maximumLODLevel (Low) made whole objects vanish, not just simplify: Unity culls a LOD group below its
 *              last level, and the ship deck in Route to Elusia has no lower level (A17, 2026-11-12).
 * GFX_TEXBIAS  0 Normal, 1 Softer (+0.5 mip bias on every sampler), 2 Soft (+1): less texture bandwidth. Read when
 *              samplers are made: takes effect at the next launch (vkr.c).
 * GFX_SHADERS  0 Accurate (every float operation rounded separately, as on the Switch), 1 Fast (the translator's
 *              reduced precision: the driver may fuse multiply-adds), 2 Fastest (also half precision, fp16, for
 *              pixel-shader arithmetic). Read when the shader cache opens: next launch; each mode keeps its own
 *              cache file (shader_cache.c).
 * GFX_PROFILE  Console mode: 0 Auto (handheld on Mali GPUs), 1 Handheld, 2 Docked. Latched by som_handheld_profile
 *              (hooks_system.c) the first time it is asked: next launch.
 * GFX_TEXSIZE  Texture size: 0 Full, 1 Half, 2 Quarter: large sampled textures lose their top mip levels (less
 *              memory and texture bandwidth). Read when images are made (vkr.c): next launch.
 * GFX_AA       Anti-aliasing (the final pass's FXAA): 0 On (the game's own), 1 Off. Applied at once (on_final_aa).
 * GFX_ASPECT   Aspect ratio: 0 Stretch to fill the screen, 1 16:9 (the picture's own shape, centred, black bars).
 *              Applied at once (vkr_blit_present, main.c). */
enum { GFX_SHADOWS = 0, GFX_MODEL = 1, GFX_TEXBIAS = 2, GFX_SHADERS = 3, GFX_PROFILE = 4, GFX_TEXSIZE = 5, GFX_AA = 6, GFX_ASPECT = 7,
       GFX_N = 8 };
static volatile int g_gfx[GFX_N];

void som_set_graphics(int key, int v) {
    static const char *names[] = {"shadows", "model detail", "texture sharpness", "shader precision", "console mode", "texture size", "anti-aliasing", "aspect ratio"};
    if (key < 0 || key >= GFX_N || v < 0 || v > 3) return;
    if (g_gfx[key] != v && key < 8) som_log("settings: %s %d", names[key], v);
    g_gfx[key] = v;
}

/* the Aspect ratio option keeps the picture's shape (vkr_blit_present letterboxes it) */
int som_keep_aspect(void) { return g_gfx[GFX_ASPECT] == 1; }

/* the extra mip LOD bias for every sampler (vkr.c) */
float som_tex_lod_bias(void) { return g_gfx[GFX_TEXBIAS] * 0.5f; }

/* the Shader precision level for translation: 0 Accurate, 1 Fast, 2 Fastest (shader_cache.c) */
int som_shader_precision(void) { return g_gfx[GFX_SHADERS]; }

/* the Console mode choice: 0 Auto, 1 Handheld, 2 Docked (hooks_system.c) */
int som_profile_choice(void) { return g_gfx[GFX_PROFILE]; }

/* the Texture size choice: top mip levels to leave out, 0 to 2 (vkr.c) */
int som_tex_size(void) { return g_gfx[GFX_TEXSIZE]; }

static void set_float(GuestCtx *c, int q, float v) {
    c->q[q] = 0;
    memcpy(&c->q[q], &v, sizeof v);
}

static float get_float(const GuestCtx *c, int q) {
    float v;
    memcpy(&v, &c->q[q], sizeof v);
    return v;
}

/* Shadows: the UniversalRenderPipelineAsset fields (2.0.0 offsets) the shadow passes read every frame. A value we did
 * not write is the game's own (scene presets rewrite them), kept to restore it. */
static struct { uint32_t off; int is_bool, game, written, have; } g_shf[] = {
    {0x64, 1}, /* m_MainLightShadowsSupported */
    {0x68, 0}, /* m_MainLightShadowmapResolution */
    {0x74, 1}, /* m_AdditionalLightShadowsSupported */
    {0x78, 0}, /* m_AdditionalLightsShadowmapResolution */
    {0xBC, 1}, /* m_LocalShadowsSupported */
    {0xC0, 0}, /* m_LocalShadowsAtlasResolution */
    {0xC8, 0}, /* m_ShadowAtlasResolution */
};

static int res_ok(int v) { return v >= 128 && v <= 8192 && !(v & (v - 1)); }

static void shadows_apply(uint64_t asset) {
    static int disabled, last_lvl = -1;
    if (disabled) return;
    uint8_t *a = (uint8_t *)(uintptr_t)asset;
    int lvl = g_gfx[GFX_SHADOWS];
    if (!g_shf[1].have && !res_ok(*(int32_t *)(a + 0x68))) { /* not the layout we expect: touch nothing */
        som_log("graphics: render pipeline asset 0x%llx has main shadow resolution %d: shadow option disabled",
                (unsigned long long)asset, *(int32_t *)(a + 0x68));
        disabled = 1;
        return;
    }
    for (size_t k = 0; k < sizeof g_shf / sizeof g_shf[0]; k++) {
        int cur = g_shf[k].is_bool ? a[g_shf[k].off] : *(int32_t *)(a + g_shf[k].off);
        if (!g_shf[k].have || cur != g_shf[k].written) g_shf[k].game = cur, g_shf[k].have = 1;
        int want = g_shf[k].game;
        if (g_shf[k].is_bool) want = lvl == 3 ? 0 : want;
        else if (lvl == 1 || lvl == 2) want = res_ok(want) ? (want >> lvl < 256 ? 256 : want >> lvl) : want;
        if (cur != want) {
            if (g_shf[k].is_bool) a[g_shf[k].off] = (uint8_t)want;
            else *(int32_t *)(a + g_shf[k].off) = want;
        }
        g_shf[k].written = want;
    }
    if (lvl != last_lvl) {
        som_log("graphics: shadows %s: main light %s, %d px (the game's %d)", lvl == 0 ? "High" : lvl == 1 ? "Medium" : lvl == 2 ? "Low" : "Off",
                g_shf[0].written ? "on" : "off", g_shf[1].written, g_shf[1].game);
        last_lvl = lvl;
    }
}

static volatile float g_lodbias_game = -1.0f; /* the game's last QualitySettings.lodBias */

/* QualitySettings.set_lodBias(float), before it runs: scaled for the model detail option */
static void on_set_lodbias(GuestCtx *c) {
    float v = get_float(c, 0), k = 1.0f; /* model detail removed: the game's own bias (hook kept for the log line) */
    static int n;
    if (n++ < 8 || v != g_lodbias_game) som_log("graphics: game sets lodBias %.2f (applied %.2f)", v, v * k);
    g_lodbias_game = v;
    set_float(c, 0, v * k);
}

/* once per frame: the shadow fields and the mesh LOD cap */
static void graphics_frame(GuestCtx *c) {
    static uint64_t asset;
    static unsigned n;
    static int maxlod_applied; /* the game's default is 0 */
    if (!asset || (n++ % 600) == 0) asset = rt_call(c, g_main_base + RVA_GET_RPASSET, 1, (uint64_t)0);
    if (asset) shadows_apply(asset);
    int maxlod = 0; /* model detail removed: the game's default, never capped */
    if (maxlod != maxlod_applied) {
        rt_call(c, g_main_base + RVA_SET_MAXLOD, 2, (uint64_t)maxlod, (uint64_t)0);
        som_log("graphics: maximumLODLevel %d", maxlod);
        maxlod_applied = maxlod;
    }
}

/* GameTime.Update, once per frame on the main thread */
static void on_frame(GuestCtx *c) {
    graphics_frame(c);
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

/* an offscreen camera about to draw (x20): its target texture at the render scale's share of its original size.
 * Each texture's original size is remembered; a texture whose size is neither that nor ours is new (or the game resized
 * it) and is measured again. */
#define OFFSCREEN_MAX 32
static struct { uint64_t rt; int w, h, sw, sh; } g_offscreen[OFFSCREEN_MAX];
static void on_offscreen_render(GuestCtx *c) {
    uint64_t cam = c->x[20], b = g_main_base;
    static int nlog;
    if (!cam) return;
    uint64_t rt = rt_call(c, b + RVA_CAM_TARGET, 2, cam, (uint64_t)0);
    if (!rt) return;
    int w = (int)rt_call(c, b + RVA_RT_GET_W, 2, rt, (uint64_t)0);
    int h = (int)rt_call(c, b + RVA_RT_GET_H, 2, rt, (uint64_t)0);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return;
    int i, fr = -1;
    for (i = 0; i < OFFSCREEN_MAX; i++) {
        if (g_offscreen[i].rt == rt) break;
        if (fr < 0 && !g_offscreen[i].rt) fr = i;
    }
    if (i == OFFSCREEN_MAX || (w != g_offscreen[i].w || h != g_offscreen[i].h) && (w != g_offscreen[i].sw || h != g_offscreen[i].sh)) {
        if (i == OFFSCREEN_MAX) i = fr >= 0 ? fr : (int)(rt >> 4) % OFFSCREEN_MAX;
        g_offscreen[i].rt = rt, g_offscreen[i].w = w, g_offscreen[i].h = h, g_offscreen[i].sw = w, g_offscreen[i].sh = h;
        if (nlog++ < 24)
            som_log("graphics: offscreen camera %llx target %llx %dx%d, camera %dx%d", (unsigned long long)cam,
                    (unsigned long long)rt, w, h, (int)rt_call(c, b + RVA_CAM_PIXEL_W, 2, cam, (uint64_t)0),
                    (int)rt_call(c, b + RVA_CAM_PIXEL_H, 2, cam, (uint64_t)0));
    }
    int pct = g_scale_pct > 100 ? 100 : g_scale_pct;
    int nw = g_offscreen[i].w * pct / 100, nh = g_offscreen[i].h * pct / 100;
    if (nw < 16) nw = 16;
    if (nh < 16) nh = 16;
    if (nw == w && nh == h) return;
    if (rt_call(c, b + RVA_RT_CREATED, 2, rt, (uint64_t)0) & 1) rt_call(c, b + RVA_RT_RELEASE, 2, rt, (uint64_t)0);
    rt_call(c, b + RVA_RT_SET_W, 3, rt, (uint64_t)nw, (uint64_t)0);
    rt_call(c, b + RVA_RT_SET_H, 3, rt, (uint64_t)nh, (uint64_t)0);
    g_offscreen[i].sw = nw, g_offscreen[i].sh = nh;
    if (nlog++ < 24) som_log("graphics: offscreen target %llx %dx%d -> %dx%d (render scale %d%%)", (unsigned long long)rt, w, h, nw, nh, pct);
}

/* the final pass about to read CameraData.antialiasing (x20 + 0x144): None when the Anti-aliasing option says off */
static void on_final_aa(GuestCtx *c) {
    int32_t *pv = (int32_t *)(uintptr_t)(c->x[20] + 0x144);
    int v = *pv, aa = g_gfx[GFX_AA];
    static unsigned seen;
    static int last = -1;
    int off = aa == 1;
    if (off != last) som_log("graphics: anti-aliasing %s (option %d)", off ? "off" : "as the game sets it", aa), last = off;
    if (v >= 0 && v < 32 && !(seen >> v & 1)) seen |= 1u << v, som_log("graphics: the final pass has anti-aliasing mode %d", v);
    if (off) *pv = 0;
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
    if (!g_fps_from_ui && som_handheld_profile()) g_fps = 30; /* handheld profile default, until the sidebar says otherwise */
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
    ok &= hook_site(b + RVA_SET_LODBIAS, "QualitySettings.set_lodBias", on_set_lodbias, 0, isl, nisl);
    ok &= hook_site(b + RVA_OFFSCREEN_RENDER, "CustomOffscreenCamera.CustomBeginFrameRendering", on_offscreen_render, 0, isl, nisl);
    ok &= hook_site(b + RVA_FINAL_AA, "PostProcessPass.RenderFinalPass antialiasing", on_final_aa, 0, isl, nisl);
    game_fps60_on = ok;
    som_log("patches: frame rate and render scale hooks %s (%d fps, %d%%)", ok ? "installed" : "FAILED (no island space?)", g_fps,
            g_scale_pct);
}
