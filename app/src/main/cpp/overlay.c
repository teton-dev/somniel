/* Performance overlay: the game's real frame rate and frame times, drawn flush into the top-left corner of every displayed
 * image. The numbers come from the game's own presents (vkr_frame_stats), not from the window loop, which always runs
 * at the panel's rate. Text is a 5x7 bitmap font rasterised on the CPU into a small host-visible buffer and copied onto
 * the swapchain image after the game's frame was blitted there. <data>/no_overlay turns it off (checked every 2 s).
 *
 * One horizontal bar that grows to the right with the level (the sidebar's slider):
 *   1: FPS
 *   2: FPS, the frame time, and a small graph of the last frame times
 *   3: level 2 plus the CPU and GPU temperatures (the hottest thermal zone of each)
 *   4: level 3 plus every CPU core's clock and the GPU's clock */
#include "overlay.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "debug_log.h"
#include "nvn/vkr.h"

extern const char *som_data_dir(void);
extern int som_target_fps(void); /* game_patches.c: the chosen frame rate, 0 if the hooks are off */

#define SCALE 3
#define CW (6 * SCALE)       /* advance per character */
#define TH (7 * SCALE)       /* glyph height */
#define PAD 8
#define GAP 12               /* between the bar's parts */
#define GW 180               /* graph width: GW / 2 frames at 2 px each */
#define GH 36                /* graph height */
#define VPAD 4               /* above and below the bar's contents: it is flush with the top edge, so it stays thin */
#define NSAMP (GW / 2)
#define MAXW 1728            /* the widest bar: level 4 on 8 cores */
#define MAXH (VPAD * 2 + GH)
#define SLOTS 2

static VkBuffer buf[SLOTS];
static VkDeviceMemory mem[SLOTS];
static uint8_t *ptr[SLOTS];
static int ready = -1;
static volatile int g_level = 2;

void overlay_set_level(int level) { g_level = level < 0 ? 0 : level > 4 ? 4 : level; }

/* 5x7 glyphs, one byte per row, bit 4 = leftmost column */
static const uint8_t *glyph(char c) {
    static const uint8_t digits[10][7] = {
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}};
    static const struct { char c; uint8_t g[7]; } letters[] = {
        {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
        {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'m', {0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11}},
        {'s', {0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E}}, {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
        {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
        {'^', {0x06, 0x09, 0x09, 0x06, 0x00, 0x00, 0x00}}, /* the degree sign */
        {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
        {'z', {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F}},
        {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {'-', {0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00}},
    };
    if (c >= '0' && c <= '9') return digits[c - '0'];
    for (unsigned i = 0; i < sizeof letters / sizeof letters[0]; i++)
        if (letters[i].c == c) return letters[i].g;
    return NULL; /* space and anything unknown */
}

static int init(void) {
    if (ready >= 0) return ready;
    ready = 0;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_vkr.phys, &mp);
    for (int s = 0; s < SLOTS; s++) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = (VkDeviceSize)MAXW * MAXH * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(g_vkr.dev, &bi, NULL, &buf[s]) != VK_SUCCESS) return 0;
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(g_vkr.dev, buf[s], &mr);
        uint32_t t = UINT32_MAX;
        VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < mp.memoryTypeCount && t == UINT32_MAX; i++)
            if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) t = i;
        if (t == UINT32_MAX) return 0;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, t};
        if (vkAllocateMemory(g_vkr.dev, &ai, NULL, &mem[s]) != VK_SUCCESS) return 0;
        vkBindBufferMemory(g_vkr.dev, buf[s], mem[s], 0);
        if (vkMapMemory(g_vkr.dev, mem[s], 0, bi.size, 0, (void **)&ptr[s]) != VK_SUCCESS) return 0;
    }
    return ready = 1;
}

static int enabled(void) {
    static int on = 1;
    static time_t next;
    time_t now = time(NULL);
    if (now >= next) {
        next = now + 2;
        char p[600];
        snprintf(p, sizeof p, "%s/no_overlay", som_data_dir());
        on = access(p, F_OK) != 0;
    }
    return on;
}

/* ---- temperatures: the hottest thermal zone whose type names a CPU core or cluster, and the same for the GPU ---- */

#define MAXZ 48
static char cpu_zone[MAXZ][96], gpu_zone[MAXZ][96];
static int n_cpu, n_gpu, zones_found;

static int read_file(const char *path, char *out, int n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int r = (int)read(fd, out, n - 1);
    close(fd);
    if (r < 0) return -1;
    out[r] = 0;
    return r;
}

static void find_zones(void) {
    zones_found = 1;
    DIR *d = opendir("/sys/class/thermal");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "thermal_zone", 12)) continue;
        char p[128], type[64];
        snprintf(p, sizeof p, "/sys/class/thermal/%s/type", e->d_name);
        if (read_file(p, type, sizeof type) <= 0) continue;
        for (char *c = type; *c; c++) *c = (char)tolower((unsigned char)*c);
        snprintf(p, sizeof p, "/sys/class/thermal/%s/temp", e->d_name);
        char probe[32];
        if (read_file(p, probe, sizeof probe) <= 0) continue; /* some zones refuse to be read */
        if (strstr(type, "gpu")) {
            if (n_gpu < MAXZ) snprintf(gpu_zone[n_gpu++], sizeof gpu_zone[0], "%s", p);
        } else if (strstr(type, "cpu")) {
            if (n_cpu < MAXZ) snprintf(cpu_zone[n_cpu++], sizeof cpu_zone[0], "%s", p);
        }
    }
    closedir(d);
    som_log("overlay: thermal zones: %d cpu, %d gpu", n_cpu, n_gpu);
}

/* the hottest of the zones, in degrees C, or -1 */
static int hottest(char zones[][96], int n) {
    int best = -1;
    for (int i = 0; i < n; i++) {
        char b[32];
        if (read_file(zones[i], b, sizeof b) <= 0) continue;
        long v = strtol(b, NULL, 10);
        if (v >= 1000) v /= 1000; /* millidegrees, the usual */
        if (v > 0 && v < 150 && v > best) best = (int)v;
    }
    return best;
}

/* ---- clocks: each CPU core's current frequency and the GPU's ---- */

#define MAXCPU 16
static int n_cores = -1;
static int core_id[MAXCPU]; /* the cores that are shown: every core but the efficiency (lowest-capacity) ones */

/* the core's relative performance: cpu_capacity, else its top frequency (both say big from little) */
static long core_perf(int i) {
    char p[96], b[32];
    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpu_capacity", i);
    if (read_file(p, b, sizeof b) > 0 && strtol(b, NULL, 10) > 0) return strtol(b, NULL, 10);
    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
    if (read_file(p, b, sizeof b) > 0) return strtol(b, NULL, 10);
    return 0;
}

static void find_cores(void) {
    int have[MAXCPU], nh = 0;
    long lo = 0, hi = 0;
    for (int i = 0; i < MAXCPU; i++) {
        char p[96];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
        if (access(p, R_OK) != 0) continue;
        long v = core_perf(i);
        if (!nh || v < lo) lo = v;
        if (!nh || v > hi) hi = v;
        have[nh++] = i;
    }
    n_cores = 0;
    for (int k = 0; k < nh; k++)
        if (lo == hi || core_perf(have[k]) > lo) core_id[n_cores++] = have[k]; /* all alike: show them all */
    som_log("overlay: clocks shown for %d of %d cores (efficiency cores left out)", n_cores, nh);
}

/* "CPU 2.8 2.8 2.8 2.8 3.2GHz" (GHz, one decimal; "-.-" for a core that is off) and "GPU 401MHz". Only the performance
 * cores are listed: the low-power (efficiency) ones, which have the lowest capacity, are left out. */
static void read_clocks(char *cpu, int cn, char *gpu, int gn) {
    if (n_cores < 0) find_cores();
    int o = snprintf(cpu, cn, "CPU");
    for (int k = 0; k < n_cores && o < cn - 8; k++) {
        int i = core_id[k];
        char p[96], b[32];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
        long khz = read_file(p, b, sizeof b) > 0 ? strtol(b, NULL, 10) : 0; /* an offline core has no frequency */
        if (khz > 0) o += snprintf(cpu + o, cn - o, " %.1f", (double)khz / 1e6);
        else o += snprintf(cpu + o, cn - o, " -.-");
    }
    snprintf(cpu + o, cn - o, "GHz");
    static const char *const gpu_files[] = {"/sys/class/kgsl/kgsl-3d0/gpuclk", "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq",
                                            "/sys/class/devfreq/3d00000.qcom,kgsl-3d0/cur_freq"};
    long hz = 0;
    for (unsigned i = 0; i < sizeof gpu_files / sizeof gpu_files[0] && hz <= 0; i++) {
        char b[32];
        if (read_file(gpu_files[i], b, sizeof b) > 0) hz = strtol(b, NULL, 10);
    }
    if (hz > 0) snprintf(gpu, gn, "GPU %ldMHz", hz / 1000000); else snprintf(gpu, gn, "GPU --");
}

/* ---- drawing ---- */

typedef struct { uint8_t r, g, b; } Rgb;

static uint8_t *g_px; /* the staging buffer being drawn into; rows are MAXW pixels */
static int g_bgr;

static void put_px(int x, int y, Rgb col) {
    if (x < 0 || y < 0 || x >= MAXW || y >= MAXH) return;
    uint8_t *p = g_px + ((size_t)y * MAXW + (size_t)x) * 4;
    p[0] = g_bgr ? col.b : col.r, p[1] = col.g, p[2] = g_bgr ? col.r : col.b, p[3] = 255;
}

/* draws s at x with its glyphs centred in a bar of height h; returns the x after it */
static int put_text(int x, int h, const char *s, Rgb col) {
    int y0 = (h - TH) / 2;
    for (int i = 0; s[i]; i++, x += CW) {
        const uint8_t *g = glyph(s[i]);
        if (!g) continue;
        for (int gy = 0; gy < 7; gy++)
            for (int gx = 0; gx < 5; gx++) {
                if (!(g[gy] >> (4 - gx) & 1)) continue;
                for (int sy = 0; sy < SCALE; sy++)
                    for (int sx = 0; sx < SCALE; sx++) put_px(x + gx * SCALE + sx, y0 + gy * SCALE + sy, col);
            }
    }
    return x;
}

static void line(int x0, int y0, int x1, int y1, Rgb col) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        put_px(x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) err += dy, x0 += sx;
        if (e2 <= dx) err += dx, y0 += sy;
    }
}

/* the last frame times as a line around the target frame time: a fixed range of the target +- 8 ms (about 2 px per ms),
 * so ordinary 1 ms wobble stays a gentle ripple and only a real spike (a frame 5 ms or more over) climbs toward the top;
 * anything beyond the range is clamped to the edge. A dim dashed line marks the target. */
static void draw_graph(int gx, int gy, const float *t, int n, float target_ms) {
    float half = 8.0f;
    if (half > target_ms) half = target_ms;
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) put_px(gx + x, gy + y, (Rgb){36, 36, 44});
    int ty = gy + (GH - 1) / 2;
    for (int x = 0; x < GW; x += 2) put_px(gx + x, ty, (Rgb){90, 90, 110});
    int px = -1, py = -1;
    for (int i = 0; i < n; i++) {
        float v = 0.5f - (t[i] - target_ms) / (2.0f * half); /* 0 = top (slow), 1 = bottom */
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        int x = gx + GW - (n - i) * 2, y = gy + (int)(v * (GH - 1));
        Rgb c = t[i] <= target_ms * 1.25f ? (Rgb){110, 235, 120} : t[i] <= target_ms * 1.8f ? (Rgb){255, 225, 70}
                                                                                          : (Rgb){255, 90, 90};
        if (px >= 0) line(px, py, x, y, c);
        px = x, py = y;
    }
}

static Rgb temp_col(int c) { return c >= 85 ? (Rgb){255, 80, 80} : c >= 72 ? (Rgb){255, 230, 60} : (Rgb){255, 255, 255}; }

void overlay_draw(VkCommandBuffer cb, VkImage dst, VkFormat fmt, VkExtent2D ext, uint32_t slot) {
    int level = g_level;
    if (level <= 0 || !enabled() || ext.height < MAXH || !init()) return;
    slot %= SLOTS;
    /* the text changes four times a second: readable, and the same numbers as the log's windows */
    static char t_fps[16], t_ms[16], t_cpu[16], t_gpu[16], t_clk[96], t_gclk[24];
    static float times[NSAMP];
    static int ntimes;
    static uint64_t next_ns, next_temp_ns, next_clk_ns;
    static Rgb fps_col = {255, 255, 255};
    static int cpu_c = -1, gpu_c = -1;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (now >= next_ns) {
        next_ns = now + 250000000ull;
        VkrFrameStats st;
        vkr_frame_stats(&st);
        if (st.frames) {
            snprintf(t_fps, sizeof t_fps, "FPS %5.1f", st.fps);
            snprintf(t_ms, sizeof t_ms, "%4.1fms", st.avg_ms);
        } else {
            snprintf(t_fps, sizeof t_fps, "FPS   -");
            t_ms[0] = 0;
        }
        /* green: within 5% of 60; yellow: within 5% of 30; red: below that */
        fps_col = st.fps >= 57.0f ? (Rgb){80, 255, 80} : st.fps >= 28.5f ? (Rgb){255, 230, 60} : (Rgb){255, 80, 80};
    }
    if (level >= 2) ntimes = vkr_frame_times(times, NSAMP); /* every frame: the graph scrolls with the game */
    if (level >= 4 && now >= next_clk_ns) {
        next_clk_ns = now + 250000000ull;
        read_clocks(t_clk, sizeof t_clk, t_gclk, sizeof t_gclk);
    }
    if (level >= 3 && now >= next_temp_ns) {
        next_temp_ns = now + 1000000000ull;
        if (!zones_found) find_zones();
        cpu_c = hottest(cpu_zone, n_cpu);
        gpu_c = hottest(gpu_zone, n_gpu);
        if (cpu_c >= 0) snprintf(t_cpu, sizeof t_cpu, "CPU %2d^C", cpu_c); else snprintf(t_cpu, sizeof t_cpu, "CPU --");
        if (gpu_c >= 0) snprintf(t_gpu, sizeof t_gpu, "GPU %2d^C", gpu_c); else snprintf(t_gpu, sizeof t_gpu, "GPU --");
    }

    g_px = ptr[slot];
    g_bgr = fmt == VK_FORMAT_B8G8R8A8_SRGB || fmt == VK_FORMAT_B8G8R8A8_UNORM;
    int h = level == 1 ? VPAD * 2 + TH : VPAD * 2 + GH;
    int w = PAD * 2 + 9 * CW; /* "FPS 100.0" */
    if (level >= 2) w += GAP + 6 * CW + GAP + GW;
    if (level >= 3) w += GAP + 8 * CW + GAP + 8 * CW;
    if (level >= 4) w += GAP + (int)strlen(t_clk) * CW + GAP + (int)strlen(t_gclk) * CW;
    if (w > MAXW) w = MAXW;
    if ((int)ext.width < w) return; /* a screen too narrow for this level */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) put_px(x, y, (Rgb){0, 0, 0});

    int x = PAD;
    x = put_text(x, h, t_fps, fps_col) + GAP;
    if (level >= 2) {
        x = put_text(x, h, t_ms, (Rgb){255, 255, 255}) + GAP;
        float target = 1000.0f / (float)(som_target_fps() ? som_target_fps() : 60);
        draw_graph(x, (h - GH) / 2, times, ntimes, target);
        x += GW + GAP;
    }
    if (level >= 3) {
        x = put_text(x, h, t_cpu, temp_col(cpu_c)) + GAP;
        x = put_text(x, h, t_gpu, temp_col(gpu_c)) + GAP;
    }
    if (level >= 4) {
        x = put_text(x, h, t_clk, (Rgb){190, 215, 255}) + GAP;
        put_text(x, h, t_gclk, (Rgb){190, 215, 255});
    }

    /* after the frame's blit into the same image */
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VkBufferImageCopy rg = {0, MAXW, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {(uint32_t)w, (uint32_t)h, 1}}; /* the very top-left corner */
    vkCmdCopyBufferToImage(cb, buf[slot], dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
}
