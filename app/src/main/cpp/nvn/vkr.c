/* Vulkan replay of NVN op streams. See vkr.h. Everything is deliberately simple: submits are synchronous, all images
 * live in GENERAL layout, buffers the GPU reads are copied from guest memory into a ring written per submit. */
#include "vkr.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "bc_decode.h"
#include "gfx.h"
#include "host.h"
#include "nvn_tex.h"
#include "perf_hint.h"
#include "shader_cache.h"
#include "../telemetry.h"

int nvn_depth_minus_w(void); /* nvn_api_core.c */
uint32_t *spv_fs_opt(const uint32_t *w, size_t *n_bytes, const char *key); /* spv_opt.c */

VkrCtx g_vkr;

extern const char *som_data_dir(void);
int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_storage, void *dst_linear, size_t dst_pitch);

#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { som_log("vkr: %s failed: %d", #x, (int)r_); } } while (0)
#define MAXI 8

/* ---------------------------------------------------------------- formats */
typedef struct { VkFormat vk; uint8_t depth, stencil; } Fmt;

static Fmt fmt_of(int f) {
    static const VkFormat tab[0x9D] = {
        [0x01] = VK_FORMAT_R8_UNORM, [0x02] = VK_FORMAT_R8_SNORM, [0x03] = VK_FORMAT_R8_UINT, [0x04] = VK_FORMAT_R8_SINT,
        [0x05] = VK_FORMAT_R16_SFLOAT, [0x06] = VK_FORMAT_R16_UNORM, [0x07] = VK_FORMAT_R16_SNORM, [0x08] = VK_FORMAT_R16_UINT,
        [0x09] = VK_FORMAT_R16_SINT, [0x0A] = VK_FORMAT_R32_SFLOAT, [0x0B] = VK_FORMAT_R32_UINT, [0x0C] = VK_FORMAT_R32_SINT,
        [0x0D] = VK_FORMAT_R8G8_UNORM, [0x0E] = VK_FORMAT_R8G8_SNORM, [0x0F] = VK_FORMAT_R8G8_UINT, [0x10] = VK_FORMAT_R8G8_SINT,
        [0x11] = VK_FORMAT_R16G16_SFLOAT, [0x12] = VK_FORMAT_R16G16_UNORM, [0x13] = VK_FORMAT_R16G16_SNORM,
        [0x14] = VK_FORMAT_R16G16_UINT, [0x15] = VK_FORMAT_R16G16_SINT, [0x16] = VK_FORMAT_R32G32_SFLOAT,
        [0x17] = VK_FORMAT_R32G32_UINT, [0x18] = VK_FORMAT_R32G32_SINT,
        [0x25] = VK_FORMAT_R8G8B8A8_UNORM, [0x26] = VK_FORMAT_R8G8B8A8_SNORM, [0x27] = VK_FORMAT_R8G8B8A8_UINT,
        [0x28] = VK_FORMAT_R8G8B8A8_SINT, [0x29] = VK_FORMAT_R16G16B16A16_SFLOAT, [0x2A] = VK_FORMAT_R16G16B16A16_UNORM,
        [0x2B] = VK_FORMAT_R16G16B16A16_SNORM, [0x2C] = VK_FORMAT_R16G16B16A16_UINT, [0x2D] = VK_FORMAT_R16G16B16A16_SINT,
        [0x2E] = VK_FORMAT_R32G32B32A32_SFLOAT, [0x2F] = VK_FORMAT_R32G32B32A32_UINT, [0x30] = VK_FORMAT_R32G32B32A32_SINT,
        [0x31] = VK_FORMAT_S8_UINT, [0x32] = VK_FORMAT_D16_UNORM, [0x33] = VK_FORMAT_D32_SFLOAT, [0x34] = VK_FORMAT_D32_SFLOAT,
        [0x35] = VK_FORMAT_D32_SFLOAT_S8_UINT, [0x36] = VK_FORMAT_D32_SFLOAT_S8_UINT,
        [0x37] = VK_FORMAT_R8G8B8A8_SRGB, [0x38] = VK_FORMAT_R8G8B8A8_SRGB,
        [0x3C] = VK_FORMAT_B5G6R5_UNORM_PACK16, [0x3D] = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        [0x3E] = VK_FORMAT_A2B10G10R10_UINT_PACK32, [0x3F] = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        [0x40] = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
        [0x41] = VK_FORMAT_BC1_RGB_UNORM_BLOCK, [0x42] = VK_FORMAT_BC1_RGBA_UNORM_BLOCK, [0x43] = VK_FORMAT_BC2_UNORM_BLOCK,
        [0x44] = VK_FORMAT_BC3_UNORM_BLOCK, [0x45] = VK_FORMAT_BC1_RGB_SRGB_BLOCK, [0x46] = VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
        [0x47] = VK_FORMAT_BC2_SRGB_BLOCK, [0x48] = VK_FORMAT_BC3_SRGB_BLOCK, [0x49] = VK_FORMAT_BC4_UNORM_BLOCK,
        [0x4A] = VK_FORMAT_BC4_SNORM_BLOCK, [0x4B] = VK_FORMAT_BC5_UNORM_BLOCK, [0x4C] = VK_FORMAT_BC5_SNORM_BLOCK,
        [0x4D] = VK_FORMAT_BC7_UNORM_BLOCK, [0x4E] = VK_FORMAT_BC7_SRGB_BLOCK, [0x4F] = VK_FORMAT_BC6H_SFLOAT_BLOCK,
        [0x50] = VK_FORMAT_BC6H_UFLOAT_BLOCK,
        [0x6D] = VK_FORMAT_R8G8B8A8_UNORM, [0x6E] = VK_FORMAT_R8G8B8A8_SNORM, [0x71] = VK_FORMAT_R16G16B16A16_SFLOAT,
        [0x99] = VK_FORMAT_B8G8R8A8_UNORM, [0x9A] = VK_FORMAT_B8G8R8A8_UNORM, [0x9B] = VK_FORMAT_B8G8R8A8_SRGB,
        [0x9C] = VK_FORMAT_B8G8R8A8_SRGB,
    };
    static const struct { uint8_t bw, bh; VkFormat unorm, srgb; } astc[14] = {
        {4, 4, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_SRGB_BLOCK}, {5, 4, VK_FORMAT_ASTC_5x4_UNORM_BLOCK, VK_FORMAT_ASTC_5x4_SRGB_BLOCK},
        {5, 5, VK_FORMAT_ASTC_5x5_UNORM_BLOCK, VK_FORMAT_ASTC_5x5_SRGB_BLOCK}, {6, 5, VK_FORMAT_ASTC_6x5_UNORM_BLOCK, VK_FORMAT_ASTC_6x5_SRGB_BLOCK},
        {6, 6, VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_SRGB_BLOCK}, {8, 5, VK_FORMAT_ASTC_8x5_UNORM_BLOCK, VK_FORMAT_ASTC_8x5_SRGB_BLOCK},
        {8, 6, VK_FORMAT_ASTC_8x6_UNORM_BLOCK, VK_FORMAT_ASTC_8x6_SRGB_BLOCK}, {8, 8, VK_FORMAT_ASTC_8x8_UNORM_BLOCK, VK_FORMAT_ASTC_8x8_SRGB_BLOCK},
        {10, 5, VK_FORMAT_ASTC_10x5_UNORM_BLOCK, VK_FORMAT_ASTC_10x5_SRGB_BLOCK}, {10, 6, VK_FORMAT_ASTC_10x6_UNORM_BLOCK, VK_FORMAT_ASTC_10x6_SRGB_BLOCK},
        {10, 8, VK_FORMAT_ASTC_10x8_UNORM_BLOCK, VK_FORMAT_ASTC_10x8_SRGB_BLOCK}, {10, 10, VK_FORMAT_ASTC_10x10_UNORM_BLOCK, VK_FORMAT_ASTC_10x10_SRGB_BLOCK},
        {12, 10, VK_FORMAT_ASTC_12x10_UNORM_BLOCK, VK_FORMAT_ASTC_12x10_SRGB_BLOCK}, {12, 12, VK_FORMAT_ASTC_12x12_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_SRGB_BLOCK},
    };
    Fmt o = {VK_FORMAT_UNDEFINED, 0, 0};
    if (f >= 0x79 && f <= 0x94) {
        unsigned i = ((unsigned)f - 0x79) % 14;
        o.vk = f >= 0x87 ? astc[i].srgb : astc[i].unorm;
        return o;
    }
    if (f > 0 && f < 0x9D) o.vk = tab[f];
    if (f == 0x31) o.stencil = 1;
    if (f >= 0x32 && f <= 0x34) o.depth = 1;
    if (f == 0x35 || f == 0x36) o.depth = o.stencil = 1;
    return o;
}

/* ---------------------------------------------------------------- memory helpers */
static VkPhysicalDeviceMemoryProperties memprops;

/* bisect flags from <data>/vkr_flags (decimal): 1 ASTC textures -> dummy, 2 skip texture uploads, 4 no window blit */
/* re-read every 2 s, so experiments run without a restart (pipelines are keyed by the flags too) */
static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---------------------------------------------------------------- profiler
 * Where a submit's time goes, logged every 5 s ("vkr: perf") and for every slow submit ("vkr: slow submit"). */
enum { PF_TEX, PF_PIPE, PF_SHADER, PF_DESC, PF_COPY, PF_WAIT, PF_N };
typedef struct {
    uint64_t t[PF_N];
    uint64_t b_ubo, b_vb, b_ib, b_sb, b_tex;
    uint32_t n_pass, n_pipe, n_tex, n_shader, n_fb, n_set, n_dedup;
    uint32_t n_late;   /* draws left out because their pipeline was still compiling */
    uint32_t n_lclear; /* passes begun with clear-on-load instead of loading their targets */
} PfStat;
static PfStat PS; /* this submit */
static struct {
    PfStat s;
    uint64_t start, submits, draws, t_replay, t_guest, max_total, last_end;
    uint32_t slow;
    /* presents (vkr_present): the game's real frame rate */
    uint64_t presents, last_present, max_gap;
    uint32_t over50, over100;
} PW;
static uint64_t pf_begin; /* start of this submit */
#define PF_T0() uint64_t pf_t0_ = now_ns()
#define PF_T1(k) (PS.t[k] += now_ns() - pf_t0_)

/* <data>/vkr_skip_pass: "N" or "LO HI": pass indices (as the GPU pass timing numbers them) whose draws are left out
 * (-2 every pass, -3 the passes into a presented window image, absent -1): measures what a pass really costs on a GPU whose per-pass timestamps overlap (Mali). */
static int g_skip_pass = -1, g_skip_hi = -1, pass_no = -1;
static int flags_cur = -1;
static int flags_refresh(void) {
    static int f = -1;
    static uint64_t next;
    uint64_t now = now_ns();
    if (f < 0 || now >= next) {
        next = now + 2000000000ull;
        char path[600];
        snprintf(path, sizeof path, "%s/vkr_flags", som_data_dir());
        FILE *fp = fopen(path, "r");
        int v = 0;
        if (fp) {
            if (fscanf(fp, "%d", &v) != 1) v = 0;
            fclose(fp);
        }
        if (v != f) som_log("vkr: flags %d", v);
        f = v;
        snprintf(path, sizeof path, "%s/vkr_skip_pass", som_data_dir());
        int sp = -1, hi = -1;
        if ((fp = fopen(path, "r"))) {
            if (fscanf(fp, "%d %d", &sp, &hi) < 1) sp = -1;
            fclose(fp);
        }
        if (hi < sp) hi = sp;
        if (sp != g_skip_pass || hi != g_skip_hi) som_log("vkr: skip pass %d-%d", sp, hi);
        g_skip_pass = sp, g_skip_hi = hi;
    }
    return flags_cur = f;
}
/* refreshed at the start of every submit */
static inline int vkr_flags(void) { return flags_cur >= 0 ? flags_cur : flags_refresh(); }

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits >> i & 1) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

/* ---------------------------------------------------------------- upload ring */
/* 96 MB per frame context; 48 MB on Mali GPUs (the low-memory phones): on the Galaxy A17 a frame uses ~3 MB and loading
 * into a map peaked at 36 MB (texture uploads; 16 MB with Texture size Half). A texture that does not fit is uploaded
 * again at its next use, when the ring is empty. Set once, before the rings are made. */
static uint32_t g_ring_bytes = 96u << 20;
#define RING_BYTES g_ring_bytes
static VkBuffer ring_buf;
static VkDeviceMemory ring_mem;
static uint8_t *ring_ptr;
static uint64_t ring_off;
static uint64_t ring_peak, ring_full; /* the most one submit used, and allocations refused, since the last status line */

/* Two frame contexts, used in turn: a submit is not waited for. Its command buffer, upload ring and descriptor pool
 * stay untouched until the submit after the next begins, which first waits for it (normally long done), so the GPU
 * works on one frame while the game and the replay prepare the next. ring_buf / ring_ptr, R.cb, R.fence and R.dpool
 * are those of the current context. */
#define NFRAME 2
static struct {
    VkCommandBuffer cb;
    VkFence fence;
    VkDescriptorPool dpool;
    VkBuffer ring_buf;
    VkDeviceMemory ring_mem;
    uint8_t *ring_ptr;
    int pending; /* submitted, fence not yet waited for */
} FR[NFRAME];
static int fr_cur;

static int ring_init1(void);
static int ring_init(void) {
    if (FR[0].ring_buf) return 1;
    {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(g_vkr.phys, &pp);
        if (pp.vendorID == 0x13B5) g_ring_bytes = 48u << 20; /* ARM (Mali) */
        som_log("vkr: upload ring %u MB per frame context", g_ring_bytes >> 20);
    }
    for (int k = 0; k < NFRAME; k++) {
        ring_buf = VK_NULL_HANDLE;
        if (!ring_init1()) return 0;
        FR[k].ring_buf = ring_buf, FR[k].ring_mem = ring_mem, FR[k].ring_ptr = ring_ptr;
    }
    ring_buf = FR[0].ring_buf, ring_mem = FR[0].ring_mem, ring_ptr = FR[0].ring_ptr;
    return 1;
}

static int ring_init1(void) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = RING_BYTES;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK_CHECK(vkCreateBuffer(g_vkr.dev, &bi, NULL, &ring_buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_vkr.dev, ring_buf, &mr);
    uint32_t t = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t == UINT32_MAX) return 0;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, t};
    VK_CHECK(vkAllocateMemory(g_vkr.dev, &ai, NULL, &ring_mem));
    VK_CHECK(vkBindBufferMemory(g_vkr.dev, ring_buf, ring_mem, 0));
    VK_CHECK(vkMapMemory(g_vkr.dev, ring_mem, 0, RING_BYTES, 0, (void **)&ring_ptr));
    return 1;
}

/* n bytes at the given alignment; NULL (and *off unset) when the ring is full */
static uint8_t *ring_alloc(uint64_t n, uint64_t align, uint64_t *off) {
    uint64_t o = (ring_off + align - 1) & ~(align - 1);
    if (o + n > RING_BYTES) { ring_full++; return NULL; }
    ring_off = o + n;
    if (ring_off > ring_peak) ring_peak = ring_off;
    *off = o;
    return ring_ptr + o;
}

/* ---- copies of guest buffers already in the ring this submit
 * The game binds the same vertex, index and uniform ranges for many draws; each range is copied once per submit. An op
 * that writes guest memory during the replay (uniform update, buffer copy, buffer fill) drops the copies it overlaps. */
enum { DD_UBO, DD_VB, DD_IB, DD_SB, DD_C1 };
#define DD_MAX 4096
static struct { uint64_t addr, size, off; uint16_t next; uint8_t kind, dead; } dd[DD_MAX];
static uint16_t dd_head[4096]; /* index + 1 */
static int ndd;

static inline uint32_t dd_slot(uint64_t addr) { return (uint32_t)((addr >> 4) * 0x9E3779B97F4A7C15ull >> 52); }
static void dd_reset(void) {
    if (ndd) memset(dd_head, 0, sizeof dd_head);
    ndd = 0;
}
static int dd_find(uint64_t addr, uint64_t size, int kind, uint64_t *off) {
    for (uint32_t i = dd_head[dd_slot(addr)]; i; i = dd[i - 1].next)
        if (dd[i - 1].addr == addr && dd[i - 1].size == size && dd[i - 1].kind == kind && !dd[i - 1].dead) {
            *off = dd[i - 1].off;
            return 1;
        }
    return 0;
}
static void dd_add(uint64_t addr, uint64_t size, int kind, uint64_t off) {
    if (ndd >= DD_MAX) return;
    uint32_t sl = dd_slot(addr);
    dd[ndd].addr = addr, dd[ndd].size = size, dd[ndd].off = off, dd[ndd].kind = (uint8_t)kind, dd[ndd].dead = 0;
    dd[ndd].next = dd_head[sl];
    dd_head[sl] = (uint16_t)++ndd;
}
static void dd_written(uint64_t addr, uint64_t size) {
    for (int i = 0; i < ndd; i++)
        if (dd[i].addr < addr + size && addr < dd[i].addr + dd[i].size + 64) dd[i].dead = 1; /* + 64: storage copies start up to 63 bytes early */
}

/* ---------------------------------------------------------------- images */
typedef struct SubView { uint32_t level, layer; VkImageView v; struct SubView *next; } SubView;
typedef struct TexView { view_t key; VkImageView v; struct TexView *next; } TexView;
typedef struct Img {
    uint64_t serial;
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view; /* all levels and layers, type from the target */
    VkFormat fmt;
    uint32_t w, h, depth, layers, levels;
    VkImageAspectFlags aspect;
    int inited, loaded, is_rt, is_depth;
    int linear_ok; /* the format supports linear filtering (depth formats often do not) */
    VkFormat bc;   /* the texture's BC format when the GPU has no BC (Mali): fmt is then its decoded format (bc_decode.c) */
    volatile int presented; /* the game has presented it (a window texture): only its crop is ever shown (pass_setup) */
    const tex_t *t;
    SubView *sv; /* single level and layer views (render targets), kept for the image's life */
    TexView *tv; /* sampled through a texture view (nvnTexturePoolRegisterTexture with a view) */
    VkComponentMapping comp; /* the texture's own swizzle, as in view */
    uint32_t drop; /* top mip levels left out (Texture size option): image level L is the texture's level L + drop */
    uint64_t bytes;
    struct Img *next;
} Img;

static uint64_t g_img_live, g_img_bytes, g_img_freed;
static int present_crop[4]; /* the window crop of the last present (x, y, w, h), set by vkr_present */
static Img *img_tab[4096];
static pthread_mutex_t img_mu = PTHREAD_MUTEX_INITIALIZER;

static uint32_t mip(uint32_t s, uint32_t l) { s >>= l; return s ? s : 1; }

static Img *img_get(const tex_t *t) {
    pthread_mutex_lock(&img_mu);
    Img **slot = &img_tab[t->serial & 4095];
    for (Img *i = *slot; i; i = i->next)
        if (i->serial == t->serial) {
            pthread_mutex_unlock(&img_mu);
            return i;
        }
    pthread_mutex_unlock(&img_mu);

    const nvn_texdesc *d = &t->d;
    Fmt f = fmt_of(d->format);
    /* experiment 32768: R11G11B10F targets as RGBA16F, which keeps negative and NaN shader outputs for inspection */
    if ((vkr_flags() & 32768) && f.vk == VK_FORMAT_B10G11R11_UFLOAT_PACK32) f.vk = VK_FORMAT_R16G16B16A16_SFLOAT;
    if ((vkr_flags() & 512) && f.vk == VK_FORMAT_D32_SFLOAT_S8_UINT) f.vk = VK_FORMAT_D32_SFLOAT, f.stencil = 0; /* experiment */
    if (f.vk == VK_FORMAT_UNDEFINED) {
        static int warned[0x9D];
        if (d->format >= 0 && d->format < 0x9D && !warned[d->format]++) som_log("vkr: texture format %#x has no Vulkan format", d->format);
        return NULL;
    }
    /* BC on a GPU without BC (the Mali-G68): an uncompressed image, decoded on the CPU at upload (img_load). The Adreno
     * supports every BC format, so this is never taken there. */
    VkFormat bc_src = VK_FORMAT_UNDEFINED;
    if (bc_fallback_format(f.vk) != VK_FORMAT_UNDEFINED) {
        VkFormatProperties p0;
        vkGetPhysicalDeviceFormatProperties(g_vkr.phys, f.vk, &p0);
        if (!(p0.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            static int noted[0x9D];
            if (d->format >= 0 && d->format < 0x9D && !noted[d->format]++)
                som_log("vkr: texture format %#x (Vulkan %d) is not supported by this GPU: decoded on the CPU to Vulkan %d", d->format,
                        (int)f.vk, (int)bc_fallback_format(f.vk));
            bc_src = f.vk;
            f.vk = bc_fallback_format(f.vk);
        }
    }
    nvn_texlayout L;
    nvn_tex_layout(d, &L);
    int cube = d->target == NVN_TEXTURE_TARGET_CUBEMAP || d->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    int three = d->target == NVN_TEXTURE_TARGET_3D, one = d->target == NVN_TEXTURE_TARGET_1D || d->target == NVN_TEXTURE_TARGET_1D_ARRAY;
    int array = d->target == NVN_TEXTURE_TARGET_1D_ARRAY || d->target == NVN_TEXTURE_TARGET_2D_ARRAY ||
                d->target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY || d->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    Img *i = calloc(1, sizeof *i);
    i->serial = t->serial;
    i->t = t;
    i->fmt = f.vk;
    i->bc = bc_src;
    i->w = d->width ? d->width : 1;
    i->h = one ? 1 : (d->height ? d->height : 1);
    i->depth = three ? (d->depth ? d->depth : 1) : 1;
    i->levels = d->levels ? d->levels : 1;
    {   /* no more levels than the size allows */
        uint32_t m = i->w > i->h ? i->w : i->h, maxl = 1;
        if (i->depth > m) m = i->depth;
        while (m > 1) m >>= 1, maxl++;
        if (i->levels > maxl) i->levels = maxl;
    }
    {   /* Texture size (sidebar Graphics, game_patches.c): large sampled-only textures with mipmaps lose their top 1-2
         * levels. Sampling picks levels by the image's own size, so the picture just loses detail; views and copies
         * shift by drop. Never a render target or GPU-written texture: no display, image, compressible, linear or
         * linear-render-target flag (Unity's render textures are compressible), no depth, 3D or multisample. */
        extern int som_tex_size(void);
        int q = som_tex_size();
        uint32_t lo = i->w < i->h ? i->w : i->h, hi = i->w > i->h ? i->w : i->h;
        if (q > 0 && !(d->flags & 0x5F) && !f.depth && !f.stencil && !three && d->samples <= 1 && i->levels >= 2 && hi >= 512) {
            uint32_t dr = (uint32_t)q < i->levels - 1 ? (uint32_t)q : i->levels - 1;
            while (dr && (lo >> dr) < 64) dr--;
            if (dr) {
                static unsigned nlog;
                if (nlog++ < 6) som_log("vkr: texture size: serial %llu %ux%u (%u levels, flags %#x) drops %u top level(s)",
                                        (unsigned long long)t->serial, i->w, i->h, i->levels, d->flags, dr);
                i->drop = dr, i->w = mip(i->w, dr), i->h = mip(i->h, dr), i->levels -= dr;
            }
        }
    }
    i->layers = cube ? 6 * (d->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY && d->depth ? d->depth / 6 ? d->depth / 6 : 1 : 1)
                     : array ? (d->depth ? d->depth : 1) : 1;
    i->is_depth = f.depth || f.stencil;
    i->aspect = i->is_depth ? (f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)
                            : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = three ? VK_IMAGE_TYPE_3D : one ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
    ci.format = f.vk;
    ci.extent = (VkExtent3D){i->w, i->h, i->depth};
    ci.mipLevels = i->levels;
    ci.arrayLayers = i->layers;
    /* Multisampled NVN textures become single-sample images: the render passes and pipelines here are all
     * 1x, and a 2x attachment in a 1x pass is invalid (device loss in Ch. 24's Past Alear scene). */
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    if (d->samples > 1) {
        static int nms;
        if (nms++ < 10) som_log("vkr: multisampled texture %ux%u fmt %d samples %d target %d", i->w, i->h, (int)f.vk, d->samples, d->target);
    }
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    /* usage from what the format supports: attachment usage on a format that cannot be one (ASTC) is invalid */
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_vkr.phys, f.vk, &fp);
    VkFormatFeatureFlags ff = fp.optimalTilingFeatures;
    i->linear_ok = (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    if (!ff) {
        /* The GPU has no support at all for this format (BC on the Mali-G68). Its driver still creates the image, then
         * crashes in vkCmdCopyBufferToImage (format_query_internal::create_integer_pfs). Leave the texture out: it reads
         * as the dummy image (black) until a decode fallback exists. Never taken on the Adreno (BC supported). */
        static int warned[0x9D];
        if (d->format >= 0 && d->format < 0x9D && !warned[d->format]++)
            som_log("vkr: texture format %#x (Vulkan %d) is not supported by this GPU: such textures are left out", d->format, (int)f.vk);
        free(i);
        return NULL;
    }
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (i->is_depth && (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (!i->is_depth && (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = vkCreateImage(g_vkr.dev, &ci, NULL, &i->image);
    if (r != VK_SUCCESS) {
        /* formats that cannot be attachments (compressed): retry without */
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        r = vkCreateImage(g_vkr.dev, &ci, NULL, &i->image);
    }
    if (r != VK_SUCCESS) {
        som_log("vkr: vkCreateImage %ux%u fmt %d failed %d", i->w, i->h, (int)f.vk, (int)r);
        free(i);
        return NULL;
    }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_vkr.dev, i->image, &mr);
    uint32_t mt = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) mt = find_mem(mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt};
    VK_CHECK(vkAllocateMemory(g_vkr.dev, &ai, NULL, &i->mem));
    {
        static uint64_t nimg, bytes;
        nimg++, bytes += mr.size;
        i->bytes = mr.size, g_img_live++, g_img_bytes += mr.size;
        if ((nimg & (nimg - 1)) == 0 || nimg % 500 == 0)
            som_log("vkr: images created %llu, %llu MB device memory", (unsigned long long)nimg, (unsigned long long)(bytes >> 20));
    }
    VK_CHECK(vkBindImageMemory(g_vkr.dev, i->image, i->mem, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = i->image;
    vi.viewType = cube ? (i->layers > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE)
                : three ? VK_IMAGE_VIEW_TYPE_3D
                : one ? (array ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                      : (array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
    vi.format = f.vk;
    vi.subresourceRange = (VkImageSubresourceRange){i->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : i->aspect, 0, i->levels, 0, i->layers};
    /* The texture's component swizzle (NVN ZERO, ONE, R, G, B, A = 0..5; Vulkan's are one higher). Unity's movie
     * shader reads luma from the alpha of an R8 texture swizzled to (0, 0, 0, R); unswizzled, alpha read 1.0. This view
     * is only ever sampled: render targets use sub_view(). */
    {
        const unsigned sw[4] = {d->swizzle_r, d->swizzle_g, d->swizzle_b, d->swizzle_a}, ident[4] = {2, 3, 4, 5};
        VkComponentSwizzle *dst[4] = {&vi.components.r, &vi.components.g, &vi.components.b, &vi.components.a};
        int custom = 0, unset = !sw[0] && !sw[1] && !sw[2] && !sw[3]; /* a descriptor never given defaults */
        for (int k = 0; k < 4 && !unset; k++)
            if (sw[k] <= 5 && sw[k] != ident[k]) {
                *dst[k] = (VkComponentSwizzle)(VK_COMPONENT_SWIZZLE_ZERO + sw[k]);
                custom = 1;
            }
        static int nlog;
        if (custom && !i->is_depth && nlog++ < 10)
            som_log("vkr: texture serial %llu %ux%u fmt %d swizzle %u %u %u %u", (unsigned long long)t->serial, i->w, i->h, (int)f.vk,
                    sw[0], sw[1], sw[2], sw[3]);
    }
    i->comp = vi.components;
    VK_CHECK(vkCreateImageView(g_vkr.dev, &vi, NULL, &i->view));
    pthread_mutex_lock(&img_mu);
    i->next = *slot;
    *slot = i;
    pthread_mutex_unlock(&img_mu);
    return i;
}

/* ---------------------------------------------------------------- texture / sampler registries */
static const tex_t *tex_by_id[1 << 20];
static Img *img_by_id[1 << 20]; /* the image last used for the id: valid while its serial is the texture's */
static view_t view_by_id[1 << 20]; /* the view the id was registered with (all zero: the whole texture) */
static samp_par samp_by_id[4096];
static uint8_t samp_have[4096];
static VkSampler samp_vk[4096][4]; /* per id: [shadow * 2 + nearest] */

void vkr_texture_register(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || id >= (1 << 20)) return;
    tex_by_id[id] = t;
    view_by_id[id] = v ? *v : (view_t){0};
    if (v && (v->set_levels || v->set_layers || v->set_format || v->set_target || v->set_swizzle || v->set_dsmode)) {
        static int n;
        if (t && n++ < 24)
            som_log("vkr: texture id %d registered with a view: levels %d (%u+%u) layers %d (%u+%u) format %d (%#x, texture %#x) target %d (%u, texture %d) swizzle %d (%u %u %u %u) dsmode %d (%u); texture %ux%u levels %d",
                    id, v->set_levels, v->base_level, v->nlevels, v->set_layers, v->min_layer, v->nlayers, v->set_format, v->format,
                    t->d.format, v->set_target, v->target, t->d.target, v->set_swizzle, v->swz[0], v->swz[1], v->swz[2], v->swz[3],
                    v->set_dsmode, v->dsmode, t->d.width, t->d.height, t->d.levels);
    }
}

void vkr_sampler_register(int id, const samp_par *s) {
    if (id < 0 || id >= 4096) return;
    samp_by_id[id] = *s;
    samp_have[id] = 1;
    memset(samp_vk[id], 0, sizeof samp_vk[id]); /* every variant follows the new parameters */
}

static VkSamplerAddressMode wrap_of(int w) {
    switch (w) {
    case NVN_WRAP_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case NVN_WRAP_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case NVN_WRAP_MODE_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case NVN_WRAP_MODE_MIRROR_CLAMP: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

static VkCompareOp cmp_of(int f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }

/* Samplers by content: the game registers the same few parameter sets under many ids and registers ids again, and a
 * device has a limited number of sampler objects (a new VkSampler per registration ran towards that limit). */
typedef struct SampEnt { samp_par p; uint8_t shadow, nearest; VkSampler s; struct SampEnt *next; } SampEnt;
static SampEnt *samp_tab[256];
static unsigned samp_count;

/* shadow: the shader does depth-compare lookups (Ryujinx SamplerType.Shadow, 1 << 10). A compare-enabled sampler used
 * by plain sampling or gather is undefined (an Adreno device loss on Engage's depth-downsample gather), and a Dref
 * lookup through a sampler without compare is undefined too (shadows that changed with the camera).
 * nearest: the image format cannot be filtered linearly (D32_SFLOAT_S8_UINT on Adreno): linear there is a device loss. */
static VkSampler sampler_make(const samp_par *p, int shadow, int nearest) {
    uint32_t h = 2166136261u;
    const uint8_t *b = (const uint8_t *)p;
    for (size_t k = 0; k < sizeof *p; k++) h = (h ^ b[k]) * 16777619u;
    h = (h ^ (uint32_t)(shadow * 2 + nearest)) * 16777619u;
    SampEnt **slot = &samp_tab[h & 255];
    for (SampEnt *e = *slot; e; e = e->next)
        if (e->shadow == shadow && e->nearest == nearest && !memcmp(&e->p, p, sizeof *p)) return e->s;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = p->magf == NVN_MAG_FILTER_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    si.minFilter = (p->minf & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = p->minf >= 4 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (nearest) {
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    }
    si.addressModeU = wrap_of(p->ws);
    si.addressModeV = wrap_of(p->wt);
    si.addressModeW = wrap_of(p->wr);
    extern float som_tex_lod_bias(void); /* game_patches.c: the texture sharpness option (+0, +0.5, +1) */
    si.mipLodBias = p->lod_bias + som_tex_lod_bias();
    si.minLod = p->lod_min < 0 ? 0 : p->lod_min;
    si.maxLod = p->lod_max >= 1000.0f ? VK_LOD_CLAMP_NONE : p->lod_max < si.minLod ? si.minLod : p->lod_max;
    if (p->minf < 2) si.minLod = 0, si.maxLod = 0.25f; /* NEAREST / LINEAR: no mipmapping, the base level only */
    /* anisotropic filtering as the game asks for it (it was left off: ground and wall textures blurred at a slant) */
    float amax = g_vkr.max_aniso;
    if (p->aniso > 1.0f && amax > 1.0f && !nearest && !shadow && si.minFilter == VK_FILTER_LINEAR) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = p->aniso < amax ? p->aniso : amax;
    }
    si.compareEnable = shadow;
    si.compareOp = p->cmp_mode == NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE ? cmp_of(p->cmp_func) : VK_COMPARE_OP_LESS_OR_EQUAL;
    /* the border colour: Vulkan has three fixed ones without an extension; the nearest of them */
    si.borderColor = p->border[3] < 0.5f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                   : p->border[0] + p->border[1] + p->border[2] >= 1.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                                                         : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    if (shadow && p->border[0] >= 0.5f) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; /* depth reads the red channel */
    SampEnt *e = calloc(1, sizeof *e);
    e->p = *p, e->shadow = (uint8_t)shadow, e->nearest = (uint8_t)nearest;
    VK_CHECK(vkCreateSampler(g_vkr.dev, &si, NULL, &e->s));
    e->next = *slot;
    *slot = e;
    samp_count++;
    if (samp_count <= 48 || (samp_count & (samp_count - 1)) == 0)
        som_log("vkr: sampler #%u: min %d mag %d wrap %d %d %d lod %g..%g bias %g aniso %g cmp %d/%d border %g %g %g %g%s%s", samp_count,
                p->minf, p->magf, p->ws, p->wt, p->wr, p->lod_min, p->lod_max, p->lod_bias, p->aniso, p->cmp_mode, p->cmp_func,
                p->border[0], p->border[1], p->border[2], p->border[3], shadow ? " [compare]" : "", nearest ? " [nearest]" : "");
    return e->s;
}

static VkSampler sampler_for(int id, int shadow, int nearest) {
    static samp_par dflt;
    static int dflt_set;
    shadow = shadow != 0, nearest = nearest != 0;
    if (id < 0 || id >= 4096 || !samp_have[id]) {
        if (!dflt_set) {
            dflt.minf = NVN_MIN_FILTER_LINEAR, dflt.magf = NVN_MAG_FILTER_LINEAR;
            dflt.ws = dflt.wt = dflt.wr = NVN_WRAP_MODE_CLAMP_TO_EDGE;
            dflt.lod_min = -1000.f, dflt.lod_max = 1000.f, dflt.aniso = 1.f;
            dflt_set = 1;
        }
        return sampler_make(&dflt, shadow, nearest);
    }
    VkSampler *slot = &samp_vk[id][shadow * 2 + nearest];
    if (!*slot) *slot = sampler_make(&samp_by_id[id], shadow, nearest);
    return *slot;
}

/* The image view for a texture id registered with an NVNtextureView: its level and layer range, target and swizzle.
 * (Sampling ignored the view: an id that exposes only some levels or one layer read the whole texture.) A view that
 * reinterprets the format is sampled in the texture's own format (the images are not created format-mutable). */
static VkImageView tex_view(Img *i, const view_t *v) {
    if (!(v->set_levels || v->set_layers || v->set_target || v->set_swizzle)) return i->view;
    for (TexView *e = i->tv; e; e = e->next)
        if (!memcmp(&e->key, v, sizeof *v)) return e->v;
    int target = v->set_target ? v->target : i->t->d.target;
    uint32_t bl = v->set_levels ? v->base_level : 0, nlv = v->set_levels && v->nlevels ? v->nlevels : i->levels;
    uint32_t ml = v->set_layers ? v->min_layer : 0, nly = v->set_layers && v->nlayers ? v->nlayers : i->layers;
    if (i->drop && v->set_levels) { /* levels the image left out start at its level 0 */
        uint32_t cut = bl < i->drop ? i->drop - bl : 0;
        bl = bl > i->drop ? bl - i->drop : 0;
        nlv = nlv > cut ? nlv - cut : 1;
    }
    if (bl >= i->levels) bl = i->levels - 1;
    if (bl + nlv > i->levels) nlv = i->levels - bl;
    if (ml >= i->layers) ml = i->layers - 1;
    if (ml + nly > i->layers) nly = i->layers - ml;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = i->image;
    vi.format = i->fmt;
    vi.components = i->comp;
    switch (target) {
    case NVN_TEXTURE_TARGET_1D: vi.viewType = i->h == 1 && i->t->d.target <= NVN_TEXTURE_TARGET_1D ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D, nly = 1; break;
    case NVN_TEXTURE_TARGET_1D_ARRAY: vi.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY; break;
    case NVN_TEXTURE_TARGET_3D: vi.viewType = VK_IMAGE_VIEW_TYPE_3D, ml = 0, nly = 1; break;
    case NVN_TEXTURE_TARGET_CUBEMAP: vi.viewType = nly >= 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D_ARRAY, nly = nly >= 6 ? 6 : nly; break;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: vi.viewType = nly >= 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_2D_ARRAY, nly -= nly >= 6 ? nly % 6 : 0; break;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
    default: vi.viewType = VK_IMAGE_VIEW_TYPE_2D, nly = 1; break;
    }
    if (i->depth > 1 && vi.viewType != VK_IMAGE_VIEW_TYPE_3D) return i->view; /* a 3D image has no layered views */
    if (i->t->d.target == NVN_TEXTURE_TARGET_1D || i->t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY)
        vi.viewType = vi.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : vi.viewType == VK_IMAGE_VIEW_TYPE_2D ? VK_IMAGE_VIEW_TYPE_1D : vi.viewType;
    if (v->set_swizzle) {
        VkComponentSwizzle *dst[4] = {&vi.components.r, &vi.components.g, &vi.components.b, &vi.components.a};
        const unsigned ident[4] = {2, 3, 4, 5};
        for (int k = 0; k < 4; k++)
            *dst[k] = v->swz[k] <= 5 && v->swz[k] != ident[k] ? (VkComponentSwizzle)(VK_COMPONENT_SWIZZLE_ZERO + v->swz[k]) : VK_COMPONENT_SWIZZLE_IDENTITY;
    }
    vi.subresourceRange = (VkImageSubresourceRange){i->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : i->aspect, bl, nlv, ml, nly};
    TexView *e = calloc(1, sizeof *e);
    e->key = *v;
    if (vkCreateImageView(g_vkr.dev, &vi, NULL, &e->v) != VK_SUCCESS) {
        static int warned;
        if (warned++ < 8) som_log("vkr: texture view (target %d levels %u+%u layers %u+%u) on serial %llu failed: whole texture", target, bl, nlv, ml, nly, (unsigned long long)i->serial);
        e->v = i->view;
    }
    e->next = i->tv;
    i->tv = e;
    return e->v;
}

/* ---------------------------------------------------------------- shaders */
typedef struct { uint16_t binding; int16_t slot; } CbufRes;
typedef struct { uint16_t binding, type; int slot, sampler_slot; } TexRes;
typedef struct Shader {
    char key[40];
    int ok, stage; /* stage: 0 vs, 1 fs */
    VkShaderModule module;
    CbufRes cb[16];
    int ncb;
    TexRes tx[16];
    int ntx;
    CbufRes sb[8]; /* storage buffers: binding and NVN storage-buffer slot */
    int nsb;
    int unsupported; /* images */
    int needs_variant; /* code-hash translation that asked about texture types: only the per-state variant is right */
    uint8_t tq[7];     /* the texture slots it asked about */
    int ntq;
    int missing;           /* no .spv yet: looked for again after retry_ns (sync_shaders.sh pushes while running) */
    uint64_t retry_ns;
    VkDescriptorSetLayout layout;
    struct Shader *next;
} Shader;

static Shader *shaders[4096]; /* by key hash */
static inline uint32_t str_hash(const char *k) {
    uint32_t h = 2166136261u;
    for (; *k; k++) h = (h ^ (uint8_t)*k) * 16777619u;
    return h;
}
static pthread_mutex_t shader_mu = PTHREAD_MUTEX_INITIALIZER;

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)sz + 1);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        fclose(f);
        return NULL;
    }
    b[sz] = 0;
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static long jnum(const char *o, const char *e, const char *name, long def) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\":", name);
    const char *p = o;
    size_t pl = strlen(pat);
    for (; p + pl < e; p++)
        if (!strncmp(p, pat, pl)) return strtol(p + pl, NULL, 10);
    return def;
}

/* the objects of a top-level array "name": [ {...}, {...} ] */
static int jarray(const char *js, const char *name, const char **objs_start, const char **objs_end, int max) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\": [", name);
    const char *p = strstr(js, pat);
    if (!p) return 0;
    p += strlen(pat);
    int n = 0;
    while (*p && *p != ']') {
        if (*p == '{') {
            const char *q = strchr(p, '}');
            if (!q) break;
            if (n < max) objs_start[n] = p, objs_end[n] = q, n++;
            p = q;
        }
        p++;
    }
    return n;
}

static void patch_fs_set(uint32_t *w, size_t nw) {
    /* header is 5 words; OpDecorate = 71, Decoration DescriptorSet = 34 */
    for (size_t i = 5; i < nw;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc) break;
        if (op == 71 && wc >= 4 && w[i + 2] == 34) w[i + 3] = 1;
        i += wc;
    }
}

static Shader *shader_load1(const char *key, int stage);

/* The translation for a stage. Under the default draw state (plain) it is the ahead-of-time one, files named by the
 * 16-digit code hash (tools/aot_shaders.py translates every shader in the romfs that way). The per-state variant
 * "<code hash>_<answers hash>" (scripts/sync_shaders.sh) serves other states, shaders whose translation asked about
 * texture types, and shaders the ahead-of-time set lacks. */
static Shader *shader_load(const char *vkey, int stage, int plain) {
    if (plain) {
        char ck[40];
        memcpy(ck, vkey, 16);
        ck[16] = 0;
        Shader *s = shader_load1(ck, stage);
        if (s && !s->needs_variant) return s;
        if (s && s->ntq) { /* ahead-of-time variant for the types of the textures it asked about: "<hash>_t" + slot, type */
            char tk[40];
            int n = snprintf(tk, sizeof tk, "%s_t", ck);
            for (int i = 0; i < s->ntq; i++) n += snprintf(tk + n, sizeof tk - (size_t)n, "%02x%x", s->tq[i], ts_tex_type(stage, s->tq[i]));
            Shader *v = shader_load1(tk, stage);
            if (v) return v;
        }
    }
    return shader_load1(vkey, stage);
}

static Shader *shader_load1(const char *key, int stage) {
    uint64_t now = now_ns();
    pthread_mutex_lock(&shader_mu);
    Shader *s = NULL, **bucket = &shaders[str_hash(key) & 4095];
    for (Shader *k = *bucket; k; k = k->next)
        if (!strcmp(k->key, key)) {
            s = k;
            break;
        }
    if (s) {
        int retry = s->missing && now >= s->retry_ns;
        if (retry) s->retry_ns = now + 3000000000ull; /* one thread retries; the others keep skipping */
        pthread_mutex_unlock(&shader_mu);
        if (!retry) return s->ok ? s : NULL;
    } else {
        s = calloc(1, sizeof *s);
        snprintf(s->key, sizeof s->key, "%s", key);
        s->stage = stage;
        s->next = *bucket;
        *bucket = s;
        pthread_mutex_unlock(&shader_mu);
    }

    PF_T0();
    PS.n_shader++;
    char path[700];
    size_t n = 0, jn = 0;
    snprintf(path, sizeof path, "%s/spv/%s.spv", som_data_dir(), key);
    char *spv = slurp(path, &n);
    snprintf(path, sizeof path, "%s/spv/%s.res.json", som_data_dir(), key);
    char *js = slurp(path, &jn);
    /* Loose files in <data>/spv (pushed by the PC tools) win, so a hand-made translation can be tried; otherwise
     * the shader cache, which translates on the device on a miss (shader_cache.c). */
    if (!spv || !js) {
        free(spv), free(js);
        spv = NULL, js = NULL;
        uint8_t *cs;
        if (sc_get(key, stage, &cs, &n, &js)) spv = (char *)cs;
    }
    if (!spv || !js || n % 4) {
        if (!s->missing) som_log("vkr: no translated shader %s (looked for again every 3 s)", key);
        s->missing = 1;
        s->retry_ns = now + 3000000000ull;
        free(spv), free(js);
        return NULL;
    }
    if (s->missing) som_log("vkr: translated shader %s arrived", key);
    s->missing = 0;
    const char *os[16], *oe[16];
    int nc = jarray(js, "cbufs", os, oe, 16);
    for (int i = 0; i < nc; i++) s->cb[s->ncb++] = (CbufRes){(uint16_t)jnum(os[i], oe[i], "binding", 0), (int16_t)jnum(os[i], oe[i], "slot", -1)};
    int nt = jarray(js, "textures", os, oe, 16);
    for (int i = 0; i < nt; i++) {
        long h = jnum(os[i], oe[i], "handle", 0);
        TexRes t;
        t.binding = (uint16_t)jnum(os[i], oe[i], "binding", 0);
        t.type = (uint16_t)jnum(os[i], oe[i], "type", 0);
        /* A handle is two driver-constant word offsets (bits 0-13 and 14-27) and a type (28+). The NVN driver keeps
         * texture handles at c0+0x168 (8 bytes per slot) and separate sampler handles from c0+0x568. With a separate
         * sampler (type 1) the translator may put either word first, so each offset is classified by its region:
         * taking the order for granted turned "texture 0, sampler 0" into texture slot 128, sampled as a blank dummy
         * (the black title-menu background: the final composite reads the scene through such handles). */
        int wa = (int)(h & 0x3fff) * 4, wb = (int)((h >> 14) & 0x3fff) * 4;
        int sep = (h >> 28) == 1;
        if (sep && wa >= 0x568 && wb < 0x568) { int x = wa; wa = wb; wb = x; }
        int tb = wa - 0x168, sb = wb - 0x568;
        t.slot = tb >= 0 && tb < 0x400 && tb % 8 == 0 ? tb / 8 : -1;
        t.sampler_slot = sep && sb >= 0 && sb % 8 == 0 ? sb / 8 : -1;
        s->tx[s->ntx++] = t;
    }
    int ns = jarray(js, "sbufs", os, oe, 8);
    for (int i = 0; i < ns; i++) s->sb[s->nsb++] = (CbufRes){(uint16_t)jnum(os[i], oe[i], "binding", 0), (int16_t)jnum(os[i], oe[i], "slot", -1)};
    const char *dummy_s[1], *dummy_e[1];
    s->unsupported = jarray(js, "images", dummy_s, dummy_e, 1);
    s->needs_variant = strlen(key) == 16 && !strstr(js, "\"tex_queried\": []");
    const char *tq = s->needs_variant ? strstr(js, "\"tex_queried\": [") : NULL;
    for (tq = tq ? tq + 16 : NULL; tq && *tq && *tq != ']' && s->ntq < 7; tq++)
        if (*tq >= '0' && *tq <= '9') {
            char *end;
            s->tq[s->ntq++] = (uint8_t)strtol(tq, &end, 10);
            tq = end - 1;
        }
    free(js);
    if (stage == 1) patch_fs_set((uint32_t *)spv, n / 4);
    if (stage == 1) {
        size_t on = n;
        uint32_t *o = spv_fs_opt((const uint32_t *)spv, &on, key);
        if (o) free(spv), spv = (void *)o, n = on;
    }

    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = n;
    mi.pCode = (const uint32_t *)spv;
    VkResult r = vkCreateShaderModule(g_vkr.dev, &mi, NULL, &s->module);
    free(spv);
    if (r != VK_SUCCESS) {
        som_log("vkr: vkCreateShaderModule %s failed %d", key, (int)r);
        return NULL;
    }
    VkDescriptorSetLayoutBinding b[40];
    uint32_t nb = 0;
    VkShaderStageFlags sf = stage == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    b[nb++] = (VkDescriptorSetLayoutBinding){0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, sf, NULL};
    for (int i = 0; i < s->ncb; i++) b[nb++] = (VkDescriptorSetLayoutBinding){s->cb[i].binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, sf, NULL};
    for (int i = 0; i < s->ntx; i++) b[nb++] = (VkDescriptorSetLayoutBinding){s->tx[i].binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, sf, NULL};
    for (int i = 0; i < s->nsb; i++) b[nb++] = (VkDescriptorSetLayoutBinding){s->sb[i].binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sf, NULL};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = nb;
    li.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(g_vkr.dev, &li, NULL, &s->layout));
    s->ok = 1;
    PF_T1(PF_SHADER);
    som_log("vkr: loaded shader %s (%d cbufs, %d textures, %d storage buffers%s)", key, s->ncb, s->ntx, s->nsb,
            s->unsupported ? ", UNSUPPORTED images" : "");
    return s;
}

/* ---------------------------------------------------------------- replay state */
static struct {
    int active;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    VkDescriptorPool dpool;
    int recording;
    /* render pass */
    int n_color;
    Img *color[MAXI], *depth;
    VkImageView color_view[MAXI], depth_view;
    uint32_t color_level[MAXI], color_layer[MAXI];
    uint32_t rt_w, rt_h;
    int in_pass; /* 1 begun; 2 set up but not begun yet (only whole-target clears so far, see pass_open) */
    uint32_t pclear; /* while in_pass == 2: attachments cleared on load (bit i colour i, 8 depth, 9 stencil) */
    VkClearValue pcv[MAXI + 1];
    VkFramebuffer pfb;
    VkRenderPass rp;
    VkFramebuffer fb;
    /* dynamic */
    float vp[4], depth_range[2];
    int has_vp, scissor_set;
    int32_t sc[4];
    float blend_const[4];
    uint32_t sten_ref[2], sten_vmask[2], sten_wmask[2];
    float poly_off[3];
    /* pipeline state */
    blend_st blend[8];
    color_st cstate;
    chmask_st chmask;
    ms_st ms;
    poly_st poly;
    ds_st ds;
    uint32_t vattr[16];
    vs_st vstream[16];
    NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16];
    p_bind_buf ssbo[6][16];
    uint64_t texh[6][32], samph[6][32];
    uint64_t copy_stride;
    /* garbage collected at submit end */
    VkImageView tmp_views[4096];
    int ntmp_views;
    VkFramebuffer tmp_fbs[2048];
    int ntmp_fbs;
    uint64_t draws, draws_skipped, clears;
    uint32_t pgen; /* bumped by every op that changes state a pipeline is built from */
    int dbg; /* vkr_flags bit 8 (debug flush): keep the notes of recent binds that a GPU fault report prints */
} R;

static pthread_mutex_t submit_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t present_serial;
static const tex_t *present_tex;
static uint64_t present_serial_now; /* serial of the presented texture (the tex_t itself may be freed by the guest) */
static Img *dummy_img;

static void pcache_init(void);
static int vkr_ready(void) {
    if (!g_vkr.ready) return 0;
    static int inited;
    if (inited) return inited > 0;
    inited = -1;
    vkGetPhysicalDeviceMemoryProperties(g_vkr.phys, &memprops);
    if (!ring_init()) return 0;
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = g_vkr.family;
    VK_CHECK(vkCreateCommandPool(g_vkr.dev, &pi, NULL, &R.pool));
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 65536}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 65536},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 16384;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = ps;
    for (int k = 0; k < NFRAME; k++) {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, R.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VK_CHECK(vkAllocateCommandBuffers(g_vkr.dev, &ai, &FR[k].cb));
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(g_vkr.dev, &fi, NULL, &FR[k].fence));
        VK_CHECK(vkCreateDescriptorPool(g_vkr.dev, &dpi, NULL, &FR[k].dpool));
    }
    R.cb = FR[0].cb, R.fence = FR[0].fence, R.dpool = FR[0].dpool;
    pcache_init();
    inited = 1;
    som_log("vkr: renderer ready");
    return 1;
}

static void barrier_all(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* to GENERAL on first use. Barriers with layout changes, clears and copies are not allowed inside a render pass (they
 * made the Adreno lose the device when a new scene first sampled its textures): end the pass, the draw begins it again
 * with LOAD ops. */
static void end_pass(void);
static void img_init(Img *i) {
    if (i->inited) return;
    i->inited = 1;
    end_pass();
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = i->image;
    b.subresourceRange = (VkImageSubresourceRange){i->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdPipelineBarrier(R.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    /* compressed formats cannot be cleared (undefined behaviour, a device loss on Adreno): their upload fills them */
    int compressed = i->fmt >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && i->fmt <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
    if (!compressed && !i->is_depth && (i->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
        VkClearColorValue z = {{0, 0, 0, 0}};
        VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(R.cb, i->image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
    }
}

static void cmd_begin_fwd(void);
static void cap_snap(void);
static void pass_open(void);
static void gp_pass_end(void);
static void end_pass(void) {
    if (!R.in_pass) return;
    if (R.in_pass == 2) pass_open(); /* only clears: the pass still has to run for them */
    vkCmdEndRenderPass(R.cb);
    gp_pass_end();
    /* A narrower barrier here (attachment writes -> fragment-stage and later only, letting the next pass's vertex work
     * overlap) was tried on the Mali-G68 (2026-11-11): it left persistent edge artifacts in the picture. Keep the full one. */
    barrier_all(R.cb);
    R.in_pass = 0;
    cap_snap();
}

/* ---- render passes (cache) */
typedef struct RpEnt { VkFormat cf[MAXI], df; int n, xf; uint32_t clr; VkRenderPass rp; struct RpEnt *next; } RpEnt;
static RpEnt *rps;

/* clr: attachments whose old contents are replaced by a clear on load (pass_open). Load ops do not change render pass
 * compatibility, so pipelines and framebuffers are made with the clr = 0 pass and used with any variant. On a tiler a
 * LOAD reads the whole target from memory into the tile buffer; a CLEAR costs nothing. */
static VkRenderPass rp_get(uint32_t clr) {
    VkFormat cf[MAXI] = {0}, df = R.depth ? R.depth->fmt : VK_FORMAT_UNDEFINED;
    for (int i = 0; i < R.n_color; i++) cf[i] = R.color[i] ? R.color[i]->fmt : VK_FORMAT_UNDEFINED;
    /* experiments (wrong pictures, for measuring what the tile loads and stores cost the GPU):
     * 131072 nothing is loaded, 262144 depth/stencil is not stored */
    int xf = vkr_flags() & (131072 | 262144);
    for (RpEnt *e = rps; e; e = e->next)
        if (e->n == R.n_color && e->df == df && e->xf == xf && e->clr == clr && !memcmp(e->cf, cf, sizeof cf)) return e->rp;
    VkAttachmentLoadOp lo = xf & 131072 ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
    VkAttachmentStoreOp dso = xf & 262144 ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
#define LO(bit) ((clr >> (bit) & 1) ? VK_ATTACHMENT_LOAD_OP_CLEAR : lo)
    VkAttachmentDescription ad[MAXI + 1];
    VkAttachmentReference cr[MAXI], dr = {0};
    uint32_t na = 0;
    for (int i = 0; i < R.n_color; i++) {
        ad[na] = (VkAttachmentDescription){0, cf[i] ? cf[i] : VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, LO(i),
                                           VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                           VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL};
        cr[i] = (VkAttachmentReference){na++, VK_IMAGE_LAYOUT_GENERAL};
    }
    if (df != VK_FORMAT_UNDEFINED) {
        ad[na] = (VkAttachmentDescription){0, df, VK_SAMPLE_COUNT_1_BIT, LO(8), dso, LO(9), dso, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL};
#undef LO
        dr = (VkAttachmentReference){na++, VK_IMAGE_LAYOUT_GENERAL};
    }
    VkSubpassDescription sp = {0};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = (uint32_t)R.n_color;
    sp.pColorAttachments = cr;
    sp.pDepthStencilAttachment = df != VK_FORMAT_UNDEFINED ? &dr : NULL;
    VkRenderPassCreateInfo ri = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ri.attachmentCount = na;
    ri.pAttachments = ad;
    ri.subpassCount = 1;
    ri.pSubpasses = &sp;
    RpEnt *e = calloc(1, sizeof *e);
    memcpy(e->cf, cf, sizeof cf);
    e->df = df;
    e->n = R.n_color;
    e->xf = xf;
    e->clr = clr;
    VK_CHECK(vkCreateRenderPass(g_vkr.dev, &ri, NULL, &e->rp));
    e->next = rps;
    rps = e;
    return e->rp;
}

static VkImageView sub_view(Img *i, uint32_t level, uint32_t layer) {
    for (SubView *e = i->sv; e; e = e->next)
        if (e->level == level && e->layer == layer) return e->v;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = i->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = i->fmt;
    vi.subresourceRange = (VkImageSubresourceRange){i->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? (i->aspect & VK_IMAGE_ASPECT_STENCIL_BIT ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT) : i->aspect, level, 1, layer, 1};
    VkImageView v = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImageView(g_vkr.dev, &vi, NULL, &v));
    SubView *e = calloc(1, sizeof *e);
    e->level = level, e->layer = layer, e->v = v;
    e->next = i->sv;
    i->sv = e;
    return v;
}

typedef struct FbEnt { VkRenderPass rp; uint32_t nv, w, h; VkImageView v[MAXI + 1]; VkFramebuffer fb; struct FbEnt *next; } FbEnt;
static FbEnt *fbs[1024];

static void recent_note(const char *l);

/* ---- frame capture (debug). Change the mtime of <data>/dump_rt (adb shell touch) and the next 8 submits log one
 * line per render-target set (targets, draws, the shaders of its first draw, the textures it samples) and, after each
 * submit, every colour target they drew to is written to <data>/rtdump/<submit>_<serial>_<w>x<h>_f<vkformat>.raw
 * (tools/rtdump.py turns them into PNGs). */
#define CAP_MAX 48
static int cap_left;
static unsigned cap_submit, cap_id;
static struct { Img *img; uint32_t level, layer; uint64_t off; uint8_t *p; uint32_t w, h, bpp; } cap_rt[CAP_MAX];
static int cap_n;
static Img *cap_set[MAXI + 1];
static int cap_nset, cap_draws, cap_ntex, cap_have;
static uint64_t cap_tex[24];
static char cap_head[400], cap_shaders[100];

static uint32_t fmt_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT: return 2;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_UNORM: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;
    }
}

static void cap_flush(void) {
    if (!cap_have) return;
    char tl[400];
    int n = 0;
    tl[0] = 0;
    for (int i = 0; i < cap_ntex && n < 380; i++) n += snprintf(tl + n, sizeof tl - (size_t)n, " %llu", (unsigned long long)cap_tex[i]);
    som_log("cap %u: %s | draws %d, first %s | samples%s", cap_submit, cap_head, cap_draws, cap_shaders, tl);
    cap_have = 0;
}

static void cap_pass(void) {
    if (!cap_left) return;
    Img *set[MAXI + 1];
    int ns = 0;
    for (int i = 0; i < R.n_color; i++) set[ns++] = R.color[i];
    if (R.depth) set[ns++] = R.depth;
    if (cap_have && ns == cap_nset && !memcmp(set, cap_set, sizeof(Img *) * (size_t)ns)) return;
    cap_flush();
    memcpy(cap_set, set, sizeof(Img *) * (size_t)ns);
    cap_nset = ns;
    cap_draws = cap_ntex = 0;
    cap_shaders[0] = 0;
    cap_have = 1;
    int n = snprintf(cap_head, sizeof cap_head, "%ux%u", R.rt_w, R.rt_h);
    for (int i = 0; i < R.n_color && n < 330; i++) {
        Img *c = R.color[i];
        n += snprintf(cap_head + n, sizeof cap_head - (size_t)n, " c%d=%llu(fmt %d %ux%u lvl %u)", i, (unsigned long long)c->serial,
                      (int)c->fmt, c->w, c->h, R.color_level[i]);
        int seen = 0;
        for (int k = 0; k < cap_n; k++) seen |= cap_rt[k].img == c && cap_rt[k].level == R.color_level[i] && cap_rt[k].layer == R.color_layer[i];
        if (!seen && cap_n < CAP_MAX) cap_rt[cap_n++] = (__typeof__(cap_rt[0])){c, R.color_level[i], R.color_layer[i], 0, NULL, 0, 0, 0};
    }
    if (R.depth && n < 380) snprintf(cap_head + n, sizeof cap_head - (size_t)n, " ds=%llu(fmt %d)", (unsigned long long)R.depth->serial, (int)R.depth->fmt);
    if (R.depth && R.n_color == 0 && cap_left == 1) { /* depth-only passes (shadow maps): the depth target of the last submit too */
        int seen = 0;
        for (int k = 0; k < cap_n; k++) seen |= cap_rt[k].img == R.depth;
        if (!seen && cap_n < CAP_MAX) cap_rt[cap_n++] = (__typeof__(cap_rt[0])){R.depth, 0, 0, 0, NULL, 0, 0, 0};
    }
}

static void cap_draw(const char *vs, const char *fs) {
    if (!cap_left || !cap_have) return;
    if (!cap_draws++) snprintf(cap_shaders, sizeof cap_shaders, "%.16s/%.16s", vs, fs);
}

static void cap_sample(const Img *im) {
    if (!cap_left || !cap_have || !im) return;
    for (int i = 0; i < cap_ntex; i++)
        if (cap_tex[i] == im->serial) return;
    if (cap_ntex < 24) cap_tex[cap_ntex++] = im->serial;
}

/* start of a submit: is a capture asked for? */
static void cap_check(void) {
    static unsigned n;
    static time_t last_mtime;
    if (cap_left) return;
    if (n++ % 30) return;
    char p[600];
    struct stat st;
    snprintf(p, sizeof p, "%s/dump_rt", som_data_dir());
    if (stat(p, &st) || st.st_mtime == last_mtime) return;
    int first = last_mtime == 0;
    last_mtime = st.st_mtime;
    if (first && time(NULL) - st.st_mtime > 10) return; /* an old trigger from an earlier run */
    snprintf(p, sizeof p, "%s/rtdump", som_data_dir());
    mkdir(p, 0755);
    cap_left = 8;
    cap_id = (unsigned)(time(NULL) % 1000000);
    cap_submit = 0;
    som_log("cap: capture %06u: the next 8 submits", cap_id);
}

/* end of a submit, before the command buffer ends: copy the captured targets into the ring */
static void cap_copy(void) {
    if (!cap_left) return;
    cap_flush();
    for (int k = 0; k < cap_n; k++) {
        Img *i = cap_rt[k].img;
        uint32_t bpp = fmt_bytes(i->fmt), w = mip(i->w, cap_rt[k].level), h = mip(i->h, cap_rt[k].level);
        cap_rt[k].p = NULL;
        if (i->is_depth) bpp = i->fmt == VK_FORMAT_D16_UNORM ? 2 : (i->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? 4 : 0; /* the depth aspect: D16 or 32-bit float */
        if (!bpp) continue;
        uint8_t *p = ring_alloc((uint64_t)w * h * bpp, 16, &cap_rt[k].off);
        if (!p) {
            som_log("cap: no ring space for serial %llu", (unsigned long long)i->serial);
            continue;
        }
        cap_rt[k].p = p, cap_rt[k].w = w, cap_rt[k].h = h, cap_rt[k].bpp = bpp;
        barrier_all(R.cb);
        VkBufferImageCopy rg = {cap_rt[k].off, 0, 0, {i->is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, cap_rt[k].level, cap_rt[k].layer, 1}, {0, 0, 0}, {w, h, 1}};
        vkCmdCopyImageToBuffer(R.cb, i->image, VK_IMAGE_LAYOUT_GENERAL, ring_buf, 1, &rg);
    }
}

/* last captured submit: a snapshot of colour target 0 after every pass that drew to a full-size target */
#define SNAP_MAX 8
static struct { Img *img; uint8_t *p; uint32_t w, h, bpp, seq; uint64_t draws; } snaps[SNAP_MAX];
static int nsnap;
static uint64_t snap_last_draws;
static unsigned snap_seq;
static void cap_snap(void) {
    if (cap_left != 1 || !R.n_color || !R.color[0]) return;
    Img *i = R.color[0];
    snap_seq++;
    if (R.draws == snap_last_draws || i->w < 1920 || nsnap >= SNAP_MAX) return;
    uint32_t bpp = fmt_bytes(i->fmt);
    if (!bpp || i->is_depth) return;
    uint64_t off;
    uint8_t *p = ring_alloc((uint64_t)i->w * i->h * bpp, 16, &off);
    if (!p) return;
    snap_last_draws = R.draws;
    VkBufferImageCopy rg = {off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, R.color_level[0], R.color_layer[0], 1}, {0, 0, 0}, {i->w, i->h, 1}};
    vkCmdCopyImageToBuffer(R.cb, i->image, VK_IMAGE_LAYOUT_GENERAL, ring_buf, 1, &rg);
    barrier_all(R.cb);
    snaps[nsnap++] = (__typeof__(snaps[0])){i, p, i->w, i->h, bpp, snap_seq, R.draws};
}

/* after the fence: write the copies out */
static void cap_write(void) {
    for (int k = 0; k < nsnap; k++) {
        char p[700];
        snprintf(p, sizeof p, "%s/rtdump/%06u_%02u_%llu_%ux%u_f%d.raw", som_data_dir(), cap_id, 50 + k,
                 (unsigned long long)snaps[k].img->serial, snaps[k].w, snaps[k].h, (int)snaps[k].img->fmt);
        FILE *f = fopen(p, "wb");
        if (f) fwrite(snaps[k].p, 1, (size_t)snaps[k].w * snaps[k].h * snaps[k].bpp, f), fclose(f);
        som_log("cap: snapshot %d after pass %u (draw %llu of the submit): serial %llu", 50 + k, snaps[k].seq,
                (unsigned long long)snaps[k].draws, (unsigned long long)snaps[k].img->serial);
    }
    nsnap = 0;
    snap_last_draws = 0;
    snap_seq = 0;
    if (!cap_left) return;
    for (int k = 0; k < cap_n; k++) {
        if (!cap_rt[k].p) continue;
        char p[700];
        snprintf(p, sizeof p, "%s/rtdump/%06u_%02u_%llu_%ux%u_f%d.raw", som_data_dir(), cap_id, cap_submit,
                 (unsigned long long)cap_rt[k].img->serial, cap_rt[k].w, cap_rt[k].h, (int)cap_rt[k].img->fmt);
        FILE *f = fopen(p, "wb");
        if (f) fwrite(cap_rt[k].p, 1, (size_t)cap_rt[k].w * cap_rt[k].h * cap_rt[k].bpp, f), fclose(f);
    }
    cap_n = 0;
    cap_submit++;
    if (!--cap_left) som_log("cap: done (%u submits)", cap_submit);
}

/* ---- GPU pass timing (vkr_flags 33554432; it was 1048576, which is also a depth-test experiment): a timestamp before each pass begins and after it ends, plus the submit's
 * start and end, read back when the frame context comes round again (its fence was waited for); every perf window logs
 * the GPU time per frame and the costliest passes by their position in the frame (the hub's 39 passes come in the same
 * order every frame). */
#define GP_MAX 160
typedef struct { uint32_t w, h, draws, nc, clr; VkFormat c0, d; char fs[9]; } GpMeta;
static struct { VkQueryPool qp; int on, full, n; GpMeta m[GP_MAX]; } GP[NFRAME]; /* full: per-pass timing too */
static int64_t gp_last_gpu_ns; /* the last collected submit's GPU time (start to end), for the ADPF hint */
static struct { double sum[GP_MAX]; GpMeta m[GP_MAX]; double frame, passes, gap; uint32_t frames, maxn, gaps; } GW;
static uint64_t gp_prev_end; /* the previous collected submit's end timestamp: the GPU's idle gap before the next */
static int gp_cur = -1; /* index of the open pass's slot in GP[fr_cur], -1 none */
static double gp_tick_ns;

static void gp_submit_begin(void) {
    GP[fr_cur].on = 0;
    gp_cur = -1;
    pass_no = -1;
    /* the submit's start and end alone when only the ADPF hint (perf_hint.c) needs the GPU time */
    int full = (vkr_flags() & 33554432) != 0;
    if (!full && !perf_hint_active()) return;
    if (!GP[fr_cur].qp) {
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 + 2 * GP_MAX;
        if (vkCreateQueryPool(g_vkr.dev, &qi, NULL, &GP[fr_cur].qp) != VK_SUCCESS) return;
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(g_vkr.phys, &pp);
        gp_tick_ns = pp.limits.timestampPeriod;
    }
    vkCmdResetQueryPool(R.cb, GP[fr_cur].qp, 0, 2 + 2 * GP_MAX);
    vkCmdWriteTimestamp(R.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, GP[fr_cur].qp, 0);
    GP[fr_cur].on = 1;
    GP[fr_cur].full = full;
    GP[fr_cur].n = 0;
}
static void gp_pass_begin(uint32_t clr) {
    if (!GP[fr_cur].on || !GP[fr_cur].full || GP[fr_cur].n >= GP_MAX) return;
    int i = GP[fr_cur].n++;
    GpMeta *m = &GP[fr_cur].m[i];
    memset(m, 0, sizeof *m);
    m->w = R.rt_w, m->h = R.rt_h, m->nc = (uint32_t)R.n_color, m->clr = clr, m->draws = R.draws;
    m->c0 = R.n_color && R.color[0] ? R.color[0]->fmt : VK_FORMAT_UNDEFINED;
    m->d = R.depth ? R.depth->fmt : VK_FORMAT_UNDEFINED;
    vkCmdWriteTimestamp(R.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, GP[fr_cur].qp, 2 + 2 * (uint32_t)i);
    gp_cur = i;
}
static void gp_pass_end(void) {
    if (gp_cur < 0 || !GP[fr_cur].on) return;
    GP[fr_cur].m[gp_cur].draws = R.draws - GP[fr_cur].m[gp_cur].draws;
    vkCmdWriteTimestamp(R.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, GP[fr_cur].qp, 3 + 2 * (uint32_t)gp_cur);
    gp_cur = -1;
}
static void gp_first_draw(const char *fs_key) {
    if (gp_cur >= 0 && GP[fr_cur].on && !GP[fr_cur].m[gp_cur].fs[0]) memcpy(GP[fr_cur].m[gp_cur].fs, fs_key, 8);
}
static void gp_submit_end(void) {
    if (GP[fr_cur].on) vkCmdWriteTimestamp(R.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, GP[fr_cur].qp, 1);
}
/* after FR[fr_cur]'s fence: its previous submit's timestamps are final */
static void gp_collect(void) {
    if (!GP[fr_cur].on) return;
    int n = GP[fr_cur].n;
    uint64_t t[2 + 2 * GP_MAX];
    if (vkGetQueryPoolResults(g_vkr.dev, GP[fr_cur].qp, 0, 2 + 2 * (uint32_t)n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    gp_last_gpu_ns = t[1] > t[0] ? (int64_t)((double)(t[1] - t[0]) * gp_tick_ns) : 0;
    if (!GP[fr_cur].full) return;
    GW.frame += (double)(t[1] - t[0]) * gp_tick_ns;
    if (gp_prev_end && t[0] > gp_prev_end && t[0] - gp_prev_end < (uint64_t)(1e9 / gp_tick_ns)) GW.gap += (double)(t[0] - gp_prev_end) * gp_tick_ns, GW.gaps++;
    gp_prev_end = t[1];
    for (int i = 0; i < n; i++) {
        double d = t[3 + 2 * i] > t[2 + 2 * i] ? (double)(t[3 + 2 * i] - t[2 + 2 * i]) * gp_tick_ns : 0;
        GW.sum[i] += d, GW.passes += d;
        GW.m[i] = GP[fr_cur].m[i];
    }
    if ((uint32_t)n > GW.maxn) GW.maxn = (uint32_t)n;
    GW.frames++;
}
static void gp_report(void) {
    if (!GW.frames) return;
    double f = GW.frames;
    /* the GPU clock where the kernel shows it (Samsung Exynos: /sys/kernel/gpu/gpu_clock in kHz) */
    int khz = 0;
    FILE *cf = fopen("/sys/kernel/gpu/gpu_clock", "r");
    if (cf) {
        if (fscanf(cf, "%d", &khz) != 1) khz = 0;
        fclose(cf);
    }
    som_log("vkr: gpu time per frame %.2f ms (in passes %.2f ms, %u passes), idle before it %.2f ms, over %u frames, gpu clock %d MHz",
            GW.frame / 1e6 / f, GW.passes / 1e6 / f, GW.maxn, GW.gaps ? GW.gap / 1e6 / GW.gaps : -1.0, GW.frames, khz / 1000);
    for (int k = 0; k < 14; k++) {
        int best = -1;
        for (uint32_t i = 0; i < GW.maxn; i++)
            if (GW.sum[i] > 0 && (best < 0 || GW.sum[i] > GW.sum[best])) best = (int)i;
        if (best < 0) break;
        const GpMeta *m = &GW.m[best];
        som_log("vkr: gpu pass #%d: %.2f ms  %ux%u colour %u (fmt %d) depth fmt %d, %u draws, cleared %#x, first fs %s", best,
                GW.sum[best] / 1e6 / f, m->w, m->h, m->nc, (int)m->c0, (int)m->d, m->draws, m->clr, m->fs);
        GW.sum[best] = -GW.sum[best]; /* taken */
    }
    memset(&GW, 0, sizeof GW);
}

/* begin the pass set up by pass_setup(1), with the whole-target clears that came first done as load ops */
static void pass_open(void) {
    if (R.in_pass != 2) return;
    uint32_t nv = (uint32_t)R.n_color + (R.depth ? 1u : 0u);
    VkClearValue cv[MAXI + 1];
    memset(cv, 0, sizeof cv);
    for (int i = 0; i < R.n_color; i++) cv[i] = R.pcv[i];
    if (R.depth) cv[R.n_color] = R.pcv[MAXI];
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = R.pclear ? rp_get(R.pclear) : R.rp;
    bi.framebuffer = R.pfb;
    bi.renderArea = (VkRect2D){{0, 0}, {R.rt_w, R.rt_h}};
    bi.clearValueCount = R.pclear ? nv : 0;
    bi.pClearValues = cv;
    pass_no++;
    gp_pass_begin(R.pclear);
    vkCmdBeginRenderPass(R.cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    if (R.pclear) PS.n_lclear++;
    R.in_pass = 1;
    R.pclear = 0;
    cap_pass();
}

static int pass_setup(int lazy);
static int begin_pass(void) {
    if (R.in_pass == 2) pass_open();
    if (R.in_pass) return 1;
    return pass_setup(0);
}

/* lazy: set the pass up but begin it only at its first non-clear command (pass_open) */
static int pass_setup(int lazy) {
    if (R.in_pass) return 1;
    if (!R.n_color && !R.depth) return 0;
    Img *ref = R.n_color && R.color[0] ? R.color[0] : R.depth;
    if (!ref) return 0;
    VkImageView views[MAXI + 1];
    uint32_t nv = 0;
    for (int i = 0; i < R.n_color; i++) {
        if (!R.color[i]) return 0;
        if (R.color[i]->drop) { /* a reduced texture (Texture size) cannot be a render target: should not happen */
            static int warned;
            if (warned++ < 8) som_log("vkr: texture size: reduced texture serial %llu bound as a render target: pass skipped",
                                      (unsigned long long)R.color[i]->serial);
            return 0;
        }
        img_init(R.color[i]);
        R.color[i]->is_rt = 1;
        views[nv++] = sub_view(R.color[i], R.color_level[i], R.color_layer[i]);
    }
    if (R.depth) {
        img_init(R.depth);
        R.depth->is_rt = 1;
        views[nv++] = sub_view(R.depth, 0, 0);
    }
    /* the framebuffer may not be larger than any attachment: take the smallest (a depth buffer smaller than the colour
     * target otherwise makes the render area run past it) */
    R.rt_w = mip(ref->w, R.color_level[0]);
    R.rt_h = mip(ref->h, R.color_level[0]);
    for (int i = 0; i < R.n_color; i++) {
        uint32_t w = mip(R.color[i]->w, R.color_level[i]), h = mip(R.color[i]->h, R.color_level[i]);
        if (w < R.rt_w) R.rt_w = w;
        if (h < R.rt_h) R.rt_h = h;
    }
    if (R.depth) {
        if (R.depth->w < R.rt_w) R.rt_w = R.depth->w;
        if (R.depth->h < R.rt_h) R.rt_h = R.depth->h;
    }
    {   /* Handheld profile: the game renders its 1280x720 picture into the top-left of a 1920x1080 window texture and
         * only that crop is shown (vkr_blit_present). A pass into a window texture then covers just the crop: on a tiler
         * the render area is what gets loaded, shaded and stored (the final 1080p pass was ~10 ms of GPU time on the
         * Mali-G68). Docked, or with a crop not at the origin, nothing changes. */
        extern int som_handheld_profile(void);
        int cx = __atomic_load_n(&present_crop[0], __ATOMIC_RELAXED), cy = __atomic_load_n(&present_crop[1], __ATOMIC_RELAXED);
        int cw = __atomic_load_n(&present_crop[2], __ATOMIC_RELAXED), ch = __atomic_load_n(&present_crop[3], __ATOMIC_RELAXED);
        int all_window = R.n_color > 0;
        for (int i = 0; i < R.n_color; i++) all_window &= R.color[i] && R.color[i]->presented && R.color_level[i] == 0;
        if (all_window && !cx && !cy && cw > 0 && ch > 0 && ((uint32_t)cw < R.rt_w || (uint32_t)ch < R.rt_h) && som_handheld_profile() &&
            !(vkr_flags() & 0x1000000)) { /* vkr_flags 0x1000000: full-size window passes, for comparing */
            if ((uint32_t)cw < R.rt_w) R.rt_w = (uint32_t)cw;
            if ((uint32_t)ch < R.rt_h) R.rt_h = (uint32_t)ch;
        }
    }
    PS.n_pass++;
    if (R.dbg) {
        char pl[300];
        int n = snprintf(pl, sizeof pl, "pass %ux%u:", R.rt_w, R.rt_h);
        for (int i = 0; i < R.n_color && n < 250; i++)
            n += snprintf(pl + n, sizeof pl - (size_t)n, " c%d %p fmt %d %ux%u L%u M%u lvl %u layer %u", i, (void *)R.color[i],
                          (int)R.color[i]->fmt, R.color[i]->w, R.color[i]->h, R.color[i]->layers, R.color[i]->levels,
                          R.color_level[i], R.color_layer[i]);
        if (R.depth && n < 280)
            snprintf(pl + n, sizeof pl - (size_t)n, " ds %p fmt %d %ux%u", (void *)R.depth, (int)R.depth->fmt, R.depth->w, R.depth->h);
        recent_note(pl);
    }
    VkRenderPass rp = rp_get(0);
    /* framebuffers are kept until one of their images is freed (img_reap) */
    uint64_t fh = (uint64_t)(uintptr_t)rp ^ ((uint64_t)R.rt_w << 20) ^ R.rt_h;
    for (uint32_t k = 0; k < nv; k++) fh = (fh ^ (uint64_t)(uintptr_t)views[k]) * 0x9E3779B97F4A7C15ull;
    FbEnt **fbucket = &fbs[fh >> 54], *fe;
    for (fe = *fbucket; fe; fe = fe->next)
        if (fe->rp == rp && fe->nv == nv && fe->w == R.rt_w && fe->h == R.rt_h && !memcmp(fe->v, views, nv * sizeof views[0])) break;
    if (!fe) {
        VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = rp;
        fi.attachmentCount = nv;
        fi.pAttachments = views;
        fi.width = R.rt_w;
        fi.height = R.rt_h;
        fi.layers = 1;
        fe = calloc(1, sizeof *fe);
        fe->rp = rp, fe->nv = nv, fe->w = R.rt_w, fe->h = R.rt_h;
        memcpy(fe->v, views, nv * sizeof views[0]);
        VK_CHECK(vkCreateFramebuffer(g_vkr.dev, &fi, NULL, &fe->fb));
        fe->next = *fbucket;
        *fbucket = fe;
        PS.n_fb++;
    }
    R.rp = rp;
    R.pfb = fe->fb;
    R.pclear = 0;
    R.in_pass = 2;
    if (!lazy) pass_open();
    return 1;
}


/* ---- image lifetime: the game finalizes textures (nvnTextureFinalize) and Unity churns through them, so images are
 * freed again. The GPU may still use one for up to NFRAME submits, so a released serial waits REAP_DELAY submits in a
 * queue and is destroyed at the next submit_begin (submit_mu held, nothing recording). The Img struct itself is kept
 * (serial 0) because img_by_id may still point at it. */
#define REAP_DELAY (NFRAME + 1)
typedef struct Reap { uint64_t serial, due; } Reap;
static Reap *reap_q;
static size_t reap_n, reap_cap;
static pthread_mutex_t reap_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t submit_count;

void vkr_texture_release(const tex_t *t) {
    if (!t || !t->serial) return;
    pthread_mutex_lock(&reap_mu);
    if (reap_n == reap_cap) {
        reap_cap = reap_cap ? reap_cap * 2 : 256;
        reap_q = realloc(reap_q, reap_cap * sizeof *reap_q);
    }
    reap_q[reap_n++] = (Reap){t->serial, submit_count + REAP_DELAY};
    pthread_mutex_unlock(&reap_mu);
}

static void img_reap(void) {
    pthread_mutex_lock(&reap_mu);
    size_t keep = 0, nd = 0;
    uint64_t dead[1024];
    for (size_t k = 0; k < reap_n; k++) {
        if (reap_q[k].due > submit_count || nd == 1024 || reap_q[k].serial == __atomic_load_n(&present_serial_now, __ATOMIC_ACQUIRE))
            reap_q[keep++] = reap_q[k];
        else
            dead[nd++] = reap_q[k].serial;
    }
    reap_n = keep;
    pthread_mutex_unlock(&reap_mu);
    if (!nd) return;
    Img *imgs[1024];
    size_t ni = 0;
    pthread_mutex_lock(&img_mu);
    for (size_t k = 0; k < nd; k++) {
        for (Img **pp = &img_tab[dead[k] & 4095]; *pp; pp = &(*pp)->next)
            if ((*pp)->serial == dead[k]) {
                Img *i = *pp;
                *pp = i->next;
                i->next = NULL;
                imgs[ni++] = i;
                break;
            }
    }
    pthread_mutex_unlock(&img_mu);
    /* framebuffers that use any view of a dying image */
    for (int b = 0; b < 1024; b++) {
        FbEnt **pp = &fbs[b];
        while (*pp) {
            FbEnt *fe = *pp;
            int hit = 0;
            for (uint32_t v = 0; v < fe->nv && !hit; v++)
                for (size_t k = 0; k < ni && !hit; k++) {
                    if (fe->v[v] == imgs[k]->view) hit = 1;
                    for (SubView *e = imgs[k]->sv; e && !hit; e = e->next)
                        if (e->v == fe->v[v]) hit = 1;
                }
            if (hit) {
                *pp = fe->next;
                vkDestroyFramebuffer(g_vkr.dev, fe->fb, NULL);
                free(fe);
            } else
                pp = &fe->next;
        }
    }
    for (size_t k = 0; k < ni; k++) {
        Img *i = imgs[k];
        for (SubView *e = i->sv; e;) {
            SubView *n = e->next;
            vkDestroyImageView(g_vkr.dev, e->v, NULL);
            free(e);
            e = n;
        }
        i->sv = NULL;
        for (TexView *e = i->tv; e;) {
            TexView *n = e->next;
            if (e->v != i->view) vkDestroyImageView(g_vkr.dev, e->v, NULL);
            free(e);
            e = n;
        }
        i->tv = NULL;
        vkDestroyImageView(g_vkr.dev, i->view, NULL);
        vkDestroyImage(g_vkr.dev, i->image, NULL);
        vkFreeMemory(g_vkr.dev, i->mem, NULL);
        g_img_live--, g_img_freed++;
        g_img_bytes -= i->bytes;
        i->serial = 0;
        i->view = VK_NULL_HANDLE, i->image = VK_NULL_HANDLE, i->mem = VK_NULL_HANDLE;
        i->t = NULL;
    }
}

/* ---- pipelines */
/* p stays NULL while the pipeline is compiling on a worker (pending = 1); the worker publishes it with a release
 * store and clears pending */
typedef struct PipeEnt { uint64_t key; VkPipeline p; volatile int pending; struct PipeEnt *next; } PipeEnt;
static PipeEnt *pipes[4096];

/* The driver's compiled pipelines, kept in <data>/pipeline.cache across runs: without it every pipeline is compiled
 * again at its first draw in every run (tens of ms each on the Adreno: the hitches when a new effect first shows). */
static VkPipelineCache g_pcache;
static unsigned g_pcache_new;   /* pipelines created since the last save */
static uint64_t g_pcache_last;  /* time of the last creation */
static volatile int g_pcache_saving;

/* one cache per driver (gpu_driver.c): a driver ignores another's data, and saving would overwrite it */
extern char g_gpu_driver_name[128];
static void pcache_path(char *path, size_t cap) {
    if (g_gpu_driver_name[0]) snprintf(path, cap, "%s/pipeline.%s.cache", som_data_dir(), g_gpu_driver_name);
    else snprintf(path, cap, "%s/pipeline.cache", som_data_dir());
}

static void pcache_init(void) {
    char path[600];
    pcache_path(path, sizeof path);
    size_t n = 0;
    void *data = NULL;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && (data = malloc((size_t)sz)) && fread(data, 1, (size_t)sz, f) == (size_t)sz) n = (size_t)sz;
        fclose(f);
    }
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, n, data};
    /* the driver checks the header (vendor, device, cache UUID) and ignores data it did not write */
    if (vkCreatePipelineCache(g_vkr.dev, &ci, NULL, &g_pcache) != VK_SUCCESS) {
        ci.initialDataSize = 0, ci.pInitialData = NULL;
        VK_CHECK(vkCreatePipelineCache(g_vkr.dev, &ci, NULL, &g_pcache));
        n = 0;
    }
    free(data);
    som_log("vkr: pipeline cache: %zu bytes loaded", n);
}

static void *pcache_save_thread(void *arg) {
    (void)arg;
    size_t n = 0;
    static size_t saved; /* pipelines that came out of the cache add nothing: no write then */
    if (vkGetPipelineCacheData(g_vkr.dev, g_pcache, &n, NULL) == VK_SUCCESS && n && n != saved) {
        saved = n;
        void *d = malloc(n);
        if (d && vkGetPipelineCacheData(g_vkr.dev, g_pcache, &n, d) == VK_SUCCESS) {
            char path[600], tmp[620];
            pcache_path(path, sizeof path);
            snprintf(tmp, sizeof tmp, "%s.tmp", path);
            FILE *f = fopen(tmp, "wb");
            if (f) {
                int ok = fwrite(d, 1, n, f) == n;
                fclose(f);
                if (ok) rename(tmp, path);
                som_log("vkr: pipeline cache: %zu bytes saved", n);
            }
        }
        free(d);
    }
    g_pcache_saving = 0;
    return NULL;
}

/* submit end: save once new pipelines have stopped coming for 3 s (on its own thread: the write takes a while) */
static void pcache_tick(uint64_t now) {
    if (!g_pcache_new || g_pcache_saving || now - g_pcache_last < 3000000000ull) return;
    g_pcache_new = 0;
    g_pcache_saving = 1;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, pcache_save_thread, NULL)) g_pcache_saving = 0;
    pthread_attr_destroy(&at);
}
typedef struct PlEnt { VkDescriptorSetLayout a, b; VkPipelineLayout l; struct PlEnt *next; } PlEnt;
static PlEnt *pls_tab[1024];

static VkPipelineLayout pl_get(VkDescriptorSetLayout a, VkDescriptorSetLayout b) {
    PlEnt **pls_b = &pls_tab[(((uint64_t)(uintptr_t)a * 31) ^ (uint64_t)(uintptr_t)b) * 0x9E3779B97F4A7C15ull >> 54];
#define pls (*pls_b)
    for (PlEnt *e = pls; e; e = e->next)
        if (e->a == a && e->b == b) return e->l;
    VkDescriptorSetLayout sl[2] = {a, b};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 2;
    li.pSetLayouts = sl;
    PlEnt *e = calloc(1, sizeof *e);
    e->a = a, e->b = b;
    VK_CHECK(vkCreatePipelineLayout(g_vkr.dev, &li, NULL, &e->l));
    e->next = pls;
    pls = e;
    return e->l;
#undef pls
}

static VkBlendFactor bf_of(int f) {
    switch (f) {
    case NVN_BLEND_FUNC_ZERO: return VK_BLEND_FACTOR_ZERO;
    case NVN_BLEND_FUNC_ONE: return VK_BLEND_FACTOR_ONE;
    case NVN_BLEND_FUNC_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case NVN_BLEND_FUNC_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case NVN_BLEND_FUNC_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case NVN_BLEND_FUNC_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case NVN_BLEND_FUNC_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case NVN_BLEND_FUNC_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp be_of(int e) {
    switch (e) {
    case NVN_BLEND_EQUATION_SUB: return VK_BLEND_OP_SUBTRACT;
    case NVN_BLEND_EQUATION_REVERSE_SUB: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case NVN_BLEND_EQUATION_MIN: return VK_BLEND_OP_MIN;
    case NVN_BLEND_EQUATION_MAX: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}
static VkStencilOp so_of(int o) {
    switch (o) {
    case NVN_STENCIL_OP_ZERO: return VK_STENCIL_OP_ZERO;
    case NVN_STENCIL_OP_REPLACE: return VK_STENCIL_OP_REPLACE;
    case NVN_STENCIL_OP_INCR: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case NVN_STENCIL_OP_DECR: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case NVN_STENCIL_OP_INVERT: return VK_STENCIL_OP_INVERT;
    case NVN_STENCIL_OP_INCR_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case NVN_STENCIL_OP_DECR_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static VkPrimitiveTopology topo_of(int p) {
    switch (p) {
    case NVN_DRAW_PRIMITIVE_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case NVN_DRAW_PRIMITIVE_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case NVN_DRAW_PRIMITIVE_TRIANGLES: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    default: return (VkPrimitiveTopology)-1;
    }
}

static VkFormat vtx_fmt(int nvn) {
    static const VkFormat t[0x6D] = {
        [0x01] = VK_FORMAT_R8_UNORM, [0x02] = VK_FORMAT_R8_SNORM, [0x03] = VK_FORMAT_R8_UINT, [0x04] = VK_FORMAT_R8_SINT,
        [0x05] = VK_FORMAT_R16_SFLOAT, [0x06] = VK_FORMAT_R16_UNORM, [0x07] = VK_FORMAT_R16_SNORM, [0x08] = VK_FORMAT_R16_UINT,
        [0x09] = VK_FORMAT_R16_SINT, [0x0A] = VK_FORMAT_R32_SFLOAT, [0x0B] = VK_FORMAT_R32_UINT, [0x0C] = VK_FORMAT_R32_SINT,
        [0x0D] = VK_FORMAT_R8G8_UNORM, [0x0E] = VK_FORMAT_R8G8_SNORM, [0x0F] = VK_FORMAT_R8G8_UINT, [0x10] = VK_FORMAT_R8G8_SINT,
        [0x11] = VK_FORMAT_R16G16_SFLOAT, [0x12] = VK_FORMAT_R16G16_UNORM, [0x13] = VK_FORMAT_R16G16_SNORM,
        [0x14] = VK_FORMAT_R16G16_UINT, [0x15] = VK_FORMAT_R16G16_SINT, [0x16] = VK_FORMAT_R32G32_SFLOAT,
        [0x17] = VK_FORMAT_R32G32_UINT, [0x18] = VK_FORMAT_R32G32_SINT,
        [0x19] = VK_FORMAT_R8G8B8A8_UNORM, [0x1A] = VK_FORMAT_R8G8B8A8_SNORM, [0x1B] = VK_FORMAT_R8G8B8A8_UINT, [0x1C] = VK_FORMAT_R8G8B8A8_SINT,
        [0x1D] = VK_FORMAT_R16G16B16A16_SFLOAT, [0x1E] = VK_FORMAT_R16G16B16A16_UNORM, [0x1F] = VK_FORMAT_R16G16B16A16_SNORM,
        [0x20] = VK_FORMAT_R16G16B16A16_UINT, [0x21] = VK_FORMAT_R16G16B16A16_SINT,
        [0x22] = VK_FORMAT_R32G32B32_SFLOAT, [0x23] = VK_FORMAT_R32G32B32_UINT, [0x24] = VK_FORMAT_R32G32B32_SINT,
        [0x25] = VK_FORMAT_R8G8B8A8_UNORM, [0x26] = VK_FORMAT_R8G8B8A8_SNORM, [0x27] = VK_FORMAT_R8G8B8A8_UINT, [0x28] = VK_FORMAT_R8G8B8A8_SINT,
        [0x29] = VK_FORMAT_R16G16B16A16_SFLOAT, [0x2A] = VK_FORMAT_R16G16B16A16_UNORM, [0x2B] = VK_FORMAT_R16G16B16A16_SNORM,
        [0x2C] = VK_FORMAT_R16G16B16A16_UINT, [0x2D] = VK_FORMAT_R16G16B16A16_SINT, [0x2E] = VK_FORMAT_R32G32B32A32_SFLOAT,
        [0x2F] = VK_FORMAT_R32G32B32A32_UINT, [0x30] = VK_FORMAT_R32G32B32A32_SINT, [0x3D] = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        [0x3E] = VK_FORMAT_A2B10G10R10_UINT_PACK32, [0x3F] = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
    };
    return nvn > 0 && nvn < 0x6D ? t[nvn] : VK_FORMAT_UNDEFINED;
}

/* ---- background pipeline compiles
 * A pipeline the driver has not seen before costs 30-120 ms to compile on the Adreno (the hub's camera hitches, new
 * effects). With async on, a missing pipeline is handed to a worker and the draws that need it are left out until it is
 * ready (usually a frame or three later); the frame itself keeps its pace. Shader modules, render passes and layouts are
 * never destroyed, so a job's handles stay valid. <data>/sync_pipes switches back to compiling in the frame. */
typedef struct PipeJob {
    struct PipeJob *next;
    PipeEnt *e;
    char vk[17], fk[17];
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb[16];
    VkVertexInputAttributeDescription va[16];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportDepthClipControlCreateInfoEXT dcc;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba[MAXI];
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyn[7];
    VkPipelineDynamicStateCreateInfo dy;
    VkGraphicsPipelineCreateInfo pi;
} PipeJob;

#define PIPE_WORKERS 3
static int g_pipe_async = -1;
static pthread_mutex_t pq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pq_cv = PTHREAD_COND_INITIALIZER;
static PipeJob *pq_head, *pq_tail;
static volatile int pq_busy; /* jobs queued or compiling */

/* point the create info at the job's own copies (it was filled by value) and compile */
static VkPipeline pipe_compile(PipeJob *j, uint64_t *took) {
    j->vi.pVertexBindingDescriptions = j->vb, j->vi.pVertexAttributeDescriptions = j->va;
    j->vp.pNext = j->vp.pNext ? &j->dcc : NULL;
    j->cb.pAttachments = j->cba;
    j->dy.pDynamicStates = j->dyn;
    j->pi.pStages = j->st, j->pi.pVertexInputState = &j->vi, j->pi.pInputAssemblyState = &j->ia, j->pi.pViewportState = &j->vp;
    j->pi.pRasterizationState = &j->rs, j->pi.pMultisampleState = &j->ms, j->pi.pDepthStencilState = &j->ds;
    j->pi.pColorBlendState = &j->cb, j->pi.pDynamicState = &j->dy;
    VkPipeline p = VK_NULL_HANDLE;
    uint64_t t0 = now_ns();
    /* a driver crash inside the compile (Mali-G68: SPIR2LIR::on_AccessChain) is only attributable if this line was logged */
    if (vkr_flags() & 0x400000) som_log("vkr: compiling pipeline vs %s fs %s", j->vk, j->fk);
    VkResult r = vkCreateGraphicsPipelines(g_vkr.dev, g_pcache, 1, &j->pi, NULL, &p);
    uint64_t dt = now_ns() - t0;
    *took = dt;
    __atomic_add_fetch(&g_pcache_new, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pcache_last, t0 + dt, __ATOMIC_RELAXED);
    if (dt > 20000000ull) {
        static int nslow;
        if (__atomic_fetch_add(&nslow, 1, __ATOMIC_RELAXED) < 200)
            som_log("vkr: pipeline compile took %.1f ms%s (vs %s fs %s)", (double)dt / 1e6, g_pipe_async ? " on a worker" : "", j->vk, j->fk);
    }
    if (r != VK_SUCCESS) {
        som_log("vkr: vkCreateGraphicsPipelines failed %d (vs %s fs %s)", (int)r, j->vk, j->fk);
        p = VK_NULL_HANDLE;
    }
    return p;
}

static void *pipe_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&pq_mu);
        while (!pq_head) pthread_cond_wait(&pq_cv, &pq_mu);
        PipeJob *j = pq_head;
        pq_head = j->next;
        if (!pq_head) pq_tail = NULL;
        pthread_mutex_unlock(&pq_mu);
        uint64_t took;
        VkPipeline p = pipe_compile(j, &took);
        __atomic_store_n(&j->e->p, p, __ATOMIC_RELEASE);
        __atomic_store_n(&j->e->pending, 0, __ATOMIC_RELEASE);
        __atomic_sub_fetch(&pq_busy, 1, __ATOMIC_RELEASE);
        free(j);
    }
    return NULL;
}

static int pipe_async_on(void) {
    if (g_pipe_async >= 0) return g_pipe_async;
    char p[600];
    snprintf(p, sizeof p, "%s/sync_pipes", som_data_dir());
    g_pipe_async = access(p, F_OK) != 0;
    int started = 0;
    for (int i = 0; g_pipe_async && i < PIPE_WORKERS; i++) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (!pthread_create(&th, &at, pipe_worker, NULL)) {
            pthread_setname_np(th, "som-pipe");
            started++;
        }
        pthread_attr_destroy(&at);
    }
    if (g_pipe_async && !started) g_pipe_async = 0;
    som_log("vkr: pipelines compile %s", g_pipe_async ? "on background workers (draws wait for them; touch <data>/sync_pipes to compile in the frame)" : "in the frame");
    return g_pipe_async;
}

static int g_pipe_late; /* set by pipeline_for when the pipeline exists but is still compiling */

/* Pipeline for the current state and the two shaders; the key hashes everything that goes into it. */
static VkPipeline pipeline_for(Shader *vs, Shader *fs, int prim, VkPipelineLayout layout, uint32_t used_streams) {
    g_pipe_late = 0;
    uint64_t h = 1469598103934665603ull;
#define MIX(p, n) do { const uint8_t *q_ = (const uint8_t *)(p); for (size_t k_ = 0; k_ < (n); k_++) h = (h ^ q_[k_]) * 1099511628211ull; } while (0)
    MIX(vs->key, 40); MIX(fs->key, 40); MIX(&prim, 4); MIX(&R.rp, sizeof R.rp);
    MIX(R.blend, sizeof R.blend); MIX(&R.cstate, sizeof R.cstate); MIX(&R.chmask, sizeof R.chmask); MIX(&R.ms, sizeof R.ms);
    MIX(&R.poly, sizeof R.poly); MIX(&R.ds, sizeof R.ds); MIX(R.vattr, sizeof R.vattr); MIX(&used_streams, 4);
    for (int i = 0; i < 16; i++) { MIX(&R.vstream[i], sizeof R.vstream[i]); }
    MIX(&R.n_color, 4);
    int flags = vkr_flags();
    MIX(&flags, 4);
    int minus_w = nvn_depth_minus_w();
    MIX(&minus_w, 4);
#undef MIX
    PipeEnt **bucket = &pipes[(h ^ h >> 32) & 4095];
    for (PipeEnt *e = *bucket; e; e = e->next)
        if (e->key == h) {
            if (__atomic_load_n(&e->pending, __ATOMIC_ACQUIRE)) {
                g_pipe_late = 1;
                return VK_NULL_HANDLE;
            }
            return __atomic_load_n(&e->p, __ATOMIC_ACQUIRE);
        }

    /* experiment 65536: replace every fragment shader by solid magenta (separates "nothing rasterized" from
     * "fragment output lost") */
    static VkShaderModule dbg_fs;
    if ((flags & 65536) && !dbg_fs) {
        static const uint32_t code[] = {
#include "dbg_frag.inc"
        };
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof code, code};
        vkCreateShaderModule(g_vkr.dev, &ci, NULL, &dbg_fs);
    }
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs->module, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
         (flags & 65536) && dbg_fs ? dbg_fs : fs->module, "main", NULL}};
    VkVertexInputBindingDescription vb[16];
    VkVertexInputAttributeDescription va[16];
    uint32_t nb = 0, na = 0;
    for (int s = 0; s < 16; s++)
        if (used_streams >> s & 1)
            vb[nb++] = (VkVertexInputBindingDescription){(uint32_t)s, R.vstream[s].stride, R.vstream[s].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
    for (int l = 0; l < 16; l++) {
        va_st a;
        memcpy(&a, &R.vattr[l], sizeof a);
        VkFormat f = a.fmt ? vtx_fmt((int)a.fmt) : VK_FORMAT_UNDEFINED;
        if (a.fmt && f == VK_FORMAT_UNDEFINED) {
            static uint8_t warned[256];
            if (!warned[a.fmt & 0xff]++) som_log("vkr: vertex attribute format %#x has no Vulkan format: attribute %d dropped", (unsigned)a.fmt, l);
        }
        if (f == VK_FORMAT_UNDEFINED || !(used_streams >> a.stream & 1)) continue;
        va[na++] = (VkVertexInputAttributeDescription){(uint32_t)l, a.stream, f, a.offset};
    }
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = nb;
    vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = na;
    vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topo_of(prim);
    VkPipelineViewportDepthClipControlCreateInfoEXT dcc = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    dcc.negativeOneToOne = VK_TRUE;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.pNext = g_vkr.depth_clip_control && minus_w ? &dcc : NULL;
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = R.poly.mode == NVN_POLYGON_MODE_LINE ? VK_POLYGON_MODE_LINE : R.poly.mode == NVN_POLYGON_MODE_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL;
    rs.cullMode = R.poly.cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT : R.poly.cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT : R.poly.cull == NVN_FACE_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
    rs.frontFace = R.poly.front == NVN_FRONT_FACE_CW ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.depthBiasEnable = R.poly.offset_en != 0;
    if (vkr_flags() & 256) rs.depthBiasEnable = VK_FALSE; /* experiment */
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = R.ds.dt;
    ds.depthWriteEnable = R.ds.dw;
    if ((vkr_flags() & 64) && R.n_color == 0) ds.depthTestEnable = ds.depthWriteEnable = VK_FALSE; /* experiment */
    ds.depthCompareOp = cmp_of((int)R.ds.df);
    ds.stencilTestEnable = R.ds.st;
    /* experiments: 4096 no culling, 8192 depth test always passes, 16384 no stencil test */
    if (vkr_flags() & 4096) rs.cullMode = VK_CULL_MODE_NONE;
    if (vkr_flags() & 8192) ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    /* experiment 1048576: the main pass's EQUAL test against the depth prepass becomes GREATER_OR_EQUAL (reversed Z),
     * 2097152: it becomes ALWAYS. If dark pixels in a cross-fade go away, prepass and main-pass depths differ. */
    if ((vkr_flags() & 1048576) && ds.depthCompareOp == VK_COMPARE_OP_EQUAL) ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    if ((vkr_flags() & 2097152) && ds.depthCompareOp == VK_COMPARE_OP_EQUAL) ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    if (vkr_flags() & 16384) ds.stencilTestEnable = VK_FALSE;
    ds.front = (VkStencilOpState){so_of((int)R.ds.fail_f), so_of((int)R.ds.zpass_f), so_of((int)R.ds.zfail_f), cmp_of((int)R.ds.sf_f), 0xff, 0xff, 0};
    ds.back = (VkStencilOpState){so_of((int)R.ds.fail_b), so_of((int)R.ds.zpass_b), so_of((int)R.ds.zfail_b), cmp_of((int)R.ds.sf_b), 0xff, 0xff, 0};
    VkPipelineColorBlendAttachmentState cba[MAXI];
    for (int i = 0; i < R.n_color; i++) {
        const blend_st *b = &R.blend[i];
        cba[i] = (VkPipelineColorBlendAttachmentState){
            (R.cstate.blend_en >> i & 1) ? VK_TRUE : VK_FALSE, bf_of((int)b->src_rgb), bf_of((int)b->dst_rgb), be_of((int)b->eq_rgb),
            bf_of((int)b->src_a), bf_of((int)b->dst_a), be_of((int)b->eq_a), (VkColorComponentFlags)((R.chmask.mask >> (4 * i)) & 0xf)};
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = (uint32_t)R.n_color;
    cb.pAttachments = cba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 7, dyn};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = layout;
    pi.renderPass = R.rp;
    PipeJob *j = calloc(1, sizeof *j);
    memcpy(j->vk, vs->key, 16), memcpy(j->fk, fs->key, 16);
    memcpy(j->st, st, sizeof st), memcpy(j->vb, vb, sizeof vb), memcpy(j->va, va, sizeof va);
    j->vi = vi, j->ia = ia, j->dcc = dcc, j->vp = vp, j->rs = rs, j->ms = ms, j->ds = ds;
    memcpy(j->cba, cba, sizeof cba), memcpy(j->dyn, dyn, sizeof dyn);
    j->cb = cb, j->dy = dy, j->pi = pi;
    PipeEnt *e = calloc(1, sizeof *e);
    e->key = h;
    VkPipeline p = VK_NULL_HANDLE;
    PF_T0();
    PS.n_pipe++;
    int queued = 0;
    if (pipe_async_on()) {
        /* already in the driver's cache: cheap, take it now (no pop-in for anything seen in an earlier run) */
        VkResult r = VK_PIPELINE_COMPILE_REQUIRED_EXT;
        /* even cache hits cost ~0.25 ms each and a new area can need a hundred at once: past 4 ms in this submit the
         * rest go to the workers as well (they come out of the cache there, a frame later) */
        if (g_vkr.pipe_cache_control && PS.t[PF_PIPE] < 4000000ull) {
            j->pi.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT;
            VkGraphicsPipelineCreateInfo tpi = j->pi;
            tpi.pStages = st, tpi.pVertexInputState = &vi, tpi.pInputAssemblyState = &ia, tpi.pViewportState = &vp;
            tpi.pRasterizationState = &rs, tpi.pMultisampleState = &ms, tpi.pDepthStencilState = &ds, tpi.pColorBlendState = &cb;
            tpi.pDynamicState = &dy;
            r = vkCreateGraphicsPipelines(g_vkr.dev, g_pcache, 1, &tpi, NULL, &p);
            j->pi.flags &= ~(VkPipelineCreateFlags)VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT;
        }
        if (r == VK_SUCCESS && p) {
            free(j);
            j = NULL;
        } else {
            p = VK_NULL_HANDLE;
            e->pending = 1;
            j->e = e;
            __atomic_add_fetch(&pq_busy, 1, __ATOMIC_RELAXED);
            pthread_mutex_lock(&pq_mu);
            if (pq_tail) pq_tail->next = j;
            else pq_head = j;
            pq_tail = j;
            pthread_cond_signal(&pq_cv);
            pthread_mutex_unlock(&pq_mu);
            queued = 1;
            g_pipe_late = 1;
        }
    } else {
        uint64_t took;
        p = pipe_compile(j, &took);
        free(j);
    }
    PS.t[PF_PIPE] += now_ns() - pf_t0_;
    if (!queued) e->p = p; /* a queued entry's p belongs to the worker from here on */
    e->next = *bucket;
    *bucket = e;
    return p;
}

/* ---- draws */
/* the scissor clamped to the render area: a scissor left over from a bigger target let fragments (and clears, up to
 * 16384x16384) run past the end of a smaller attachment */
static VkRect2D scissor_rect(void) {
    int64_t x0 = 0, y0 = 0, x1 = R.rt_w, y1 = R.rt_h;
    if (R.scissor_set) {
        x0 = R.sc[0], y0 = R.sc[1], x1 = (int64_t)R.sc[0] + R.sc[2], y1 = (int64_t)R.sc[1] + R.sc[3];
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > R.rt_w) x1 = R.rt_w;
        if (y1 > R.rt_h) y1 = R.rt_h;
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;
    }
    return (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
}

static void set_dynamic(void) {
    float x = R.has_vp ? R.vp[0] : 0, y = R.has_vp ? R.vp[1] : 0, w = R.has_vp ? R.vp[2] : (float)R.rt_w, h = R.has_vp ? R.vp[3] : (float)R.rt_h;
    float dn = R.depth_range[0], df = R.depth_range[1];
    if ((vkr_flags() & 2048) && dn == 0.0f && df == 0.0f) df = 1.0f; /* experiment */
    VkViewport v = {x, y + h, w, -h, dn, df};
    vkCmdSetViewport(R.cb, 0, 1, &v);
    VkRect2D sc = scissor_rect();
    vkCmdSetScissor(R.cb, 0, 1, &sc);
    vkCmdSetBlendConstants(R.cb, R.blend_const);
    vkCmdSetStencilReference(R.cb, VK_STENCIL_FACE_FRONT_BIT, R.sten_ref[0]);
    vkCmdSetStencilReference(R.cb, VK_STENCIL_FACE_BACK_BIT, R.sten_ref[1]);
    vkCmdSetStencilCompareMask(R.cb, VK_STENCIL_FACE_FRONT_BIT, R.sten_vmask[0]);
    vkCmdSetStencilCompareMask(R.cb, VK_STENCIL_FACE_BACK_BIT, R.sten_vmask[1]);
    vkCmdSetStencilWriteMask(R.cb, VK_STENCIL_FACE_FRONT_BIT, R.sten_wmask[0]);
    vkCmdSetStencilWriteMask(R.cb, VK_STENCIL_FACE_BACK_BIT, R.sten_wmask[1]);
    vkCmdSetDepthBias(R.cb, R.poly_off[1], 0.0f, R.poly_off[0]);
}

static Img *dummy_image(void) {
    if (dummy_img) return dummy_img;
    static tex_t t;
    t.d = (nvn_texdesc){NVN_TEXTURE_TARGET_2D, 0x25, 0, 1, 1, 1, 1, 1, 0};
    t.serial = ~0ull - 1;
    dummy_img = img_get(&t);
    if (dummy_img) {
        img_init(dummy_img);
        dummy_img->loaded = 1;
    }
    return dummy_img;
}

/* shadow (depth-compare) lookups need a depth view: a 1x1 D32 cleared to 1.0 (nothing in shadow) */
static Img *dummy_depth_img;
static Img *dummy_depth_image(void) {
    if (dummy_depth_img) return dummy_depth_img;
    static tex_t t;
    t.d = (nvn_texdesc){NVN_TEXTURE_TARGET_2D, 0x33, 0, 1, 1, 1, 1, 1, 0};
    t.serial = ~0ull - 2;
    dummy_depth_img = img_get(&t);
    if (dummy_depth_img) {
        img_init(dummy_depth_img);
        VkClearDepthStencilValue one = {1.0f, 0};
        VkImageSubresourceRange r = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(R.cb, dummy_depth_img->image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &r);
        barrier_all(R.cb);
        dummy_depth_img->loaded = 1;
    }
    return dummy_depth_img;
}

/* The CPU rewrote a texture's storage (WriteTexels: a movie frame each frame): upload it again at its next sample. */
void vkr_tex_written(const tex_t *t) {
    static unsigned n, total;
    int sampled = 0;
    pthread_mutex_lock(&img_mu);
    for (Img *i = img_tab[t->serial & 4095]; i; i = i->next)
        if (i->serial == t->serial && !i->is_rt) i->loaded = 0, sampled = 1;
    pthread_mutex_unlock(&img_mu);
    total++;
    if (t->d.width >= 256 && (n++ < 12 || total % 500 == 0))
        som_log("vkr: texture serial %llu %ux%u fmt %#x mem %p rewritten by the CPU (%s sampled yet; %u rewrites)", (unsigned long long)t->serial,
                t->d.width, t->d.height, t->d.format, (void *)t->mem, sampled ? "was" : "never", total);
}

/* upload a texture's storage the first time it is sampled, and again after the CPU rewrote it */
static void img_load(Img *i) {
    if (i->loaded || i->is_rt) return;
    i->loaded = 1;
    end_pass();
    if (vkr_flags() & 2) { img_init(i); return; }
    PF_T0();
    PS.n_tex++;
    const tex_t *t = i->t;
    if (!t->mem || nvn_tex_is_linear(&t->d)) {
        if (!t->mem) return;
    }
    img_init(i);
    const nvn_texdesc *d = &t->d;
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    if (!bpb) return;
    VkBufferImageCopy regs[256]; /* a cube map with a full mip chain is 6 x 12 regions and more */
    uint32_t nr = 0;
    for (uint32_t layer = 0; layer < i->layers; layer++)
        for (uint32_t level = 0; level < i->levels; level++) {
            uint32_t w = mip(i->w, level), h = mip(i->h, level), dep = i->depth > 1 ? mip(i->depth, level) : 1;
            uint32_t row = (w + (uint32_t)bw - 1) / (uint32_t)bw * (uint32_t)bpb, rows = (h + (uint32_t)bh - 1) / (uint32_t)bh;
            uint64_t sz = (uint64_t)row * rows * dep, off;
            /* a BC texture decoded on the CPU (i->bc): the ring gets the decoded texels, the blocks go to a scratch buffer */
            uint64_t up = i->bc ? (uint64_t)w * h * dep * bc_fallback_bpp(i->bc) : sz;
            static uint8_t *bc_tmp;
            static uint64_t bc_cap;
            if (i->bc && sz > bc_cap) {
                free(bc_tmp);
                bc_cap = sz;
                bc_tmp = malloc(bc_cap);
                if (!bc_tmp) bc_cap = 0;
            }
            uint8_t *dst = ring_alloc(up, 16, &off);
            if (!dst || nr >= 256 || (i->bc && !bc_tmp)) {
                som_log("vkr: upload ring full for texture serial %llu (%u regions): uploaded again at its next use", (unsigned long long)t->serial, nr);
                i->loaded = 0; /* the next submit starts with an empty ring */
                goto done;
            }
            if (!i->bc) {
                if (nvn_deswizzle(d, level + i->drop, layer, t->mem, dst, row)) memset(dst, 0, sz);
            } else if (nvn_deswizzle(d, level + i->drop, layer, t->mem, bc_tmp, row)) {
                memset(dst, 0, up);
            } else {
                bc_decode_level(i->bc, bc_tmp, row, w, h, dep, dst);
                /* <data>/bc_dump: write the first decoded level 0 of up to 6 textures per BC format to <data>/bcdump/ for
                 * inspection on the PC (raw texels of the decoded format) */
                static int dump_on = -1, dumped[256];
                if (dump_on < 0) {
                    char p[600];
                    snprintf(p, sizeof p, "%s/bc_dump", som_data_dir());
                    dump_on = access(p, F_OK) == 0;
                    if (dump_on) snprintf(p, sizeof p, "%s/bcdump", som_data_dir()), mkdir(p, 0755);
                }
                if (dump_on && level == 0 && layer == 0 && w >= 64 && dumped[i->bc & 255] < 6) {
                    dumped[i->bc & 255]++;
                    char p[700];
                    snprintf(p, sizeof p, "%s/bcdump/%llu_vk%d_to%d_%ux%u.raw", som_data_dir(), (unsigned long long)t->serial, (int)i->bc,
                             (int)i->fmt, w, h);
                    FILE *fp = fopen(p, "wb");
                    if (fp) fwrite(dst, 1, (size_t)up, fp), fclose(fp);
                }
            }
            PS.b_tex += up;
            regs[nr++] = (VkBufferImageCopy){off, 0, 0, {i->aspect, level, layer, 1}, {0, 0, 0}, {w, h, dep}};
        }
done:
    if (nr) vkCmdCopyBufferToImage(R.cb, ring_buf, i->image, VK_IMAGE_LAYOUT_GENERAL, nr, regs);
    barrier_all(R.cb);
    PF_T1(PF_TEX);
}

static uint64_t ubo_bytes(int st, int idx, VkBuffer *buf, uint64_t *off, uint64_t *range) {
    static uint8_t zeros[256];
    const p_bind_buf *b = &R.ubo[st][idx];
    uint64_t n = b->size > 65536 ? 65536 : b->size;
    uint8_t *p;
    *buf = ring_buf;
    if (b->addr && n && dd_find(b->addr, n, DD_UBO, off)) {
        *range = *off + 65536 <= RING_BYTES ? 65536 : n;
        PS.n_dedup++;
        return 1;
    }
    if (!b->addr || !n || !nvn_pool_find(b->addr, n)) {
        static int warned;
        if (b->addr && n && warned++ < 10)
            som_log("vkr: uniform buffer stage %d slot %d at 0x%llx (%llu bytes) is in no memory pool: zeros", st, idx,
                    (unsigned long long)b->addr, (unsigned long long)b->size);
        p = ring_alloc(256, 256, off);
        if (p) memcpy(p, zeros, 256);
        *range = 256;
    } else {
        p = ring_alloc(n, 256, off);
        if (p) memcpy(p, (const void *)(uintptr_t)b->addr, n), dd_add(b->addr, n, DD_UBO, *off);
        PS.b_ubo += n;
        *range = n;
    }
    if (p && *off + 65536 <= RING_BYTES) *range = 65536; /* shaders may read past the bound size: stay inside the ring */
    *buf = ring_buf;
    return p != NULL;
}

/* the last few binds, printed when the debug flush catches a GPU fault */
static char g_recent[16][300];
static unsigned g_nrecent;
static void recent_note(const char *l) { snprintf(g_recent[g_nrecent++ % 16], sizeof g_recent[0], "%s", l); }

static int g_debug_flush = 1; /* only with vkr_flags bit 8: submits and waits after every draw to pin GPU faults */
static void flush_debug(const char *what, const char *vs, const char *fs, int count, int indexed, int prim) {
    if (!g_debug_flush || !(vkr_flags() & 8)) return;
    end_pass();
    vkEndCommandBuffer(R.cb);
    R.recording = 0;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &R.cb;
    pthread_mutex_lock(&g_vkr.qmu);
    VkResult r = vkQueueSubmit(g_vkr.queue, 1, &si, R.fence);
    pthread_mutex_unlock(&g_vkr.qmu);
    struct timespec t0_, t1_;
    clock_gettime(CLOCK_MONOTONIC, &t0_);
    if (r == VK_SUCCESS) {
        r = vkWaitForFences(g_vkr.dev, 1, &R.fence, VK_TRUE, 10000000000ull);
        vkResetFences(g_vkr.dev, 1, &R.fence);
    }
    if (r == VK_SUCCESS) { /* a fault may only show at the next submit: probe now so it is blamed on this draw */
        VkSubmitInfo e = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        pthread_mutex_lock(&g_vkr.qmu);
        r = vkQueueSubmit(g_vkr.queue, 1, &e, VK_NULL_HANDLE);
        if (r == VK_SUCCESS) r = vkQueueWaitIdle(g_vkr.queue);
        pthread_mutex_unlock(&g_vkr.qmu);
        if (r == VK_SUCCESS) { /* the kernel reports faults a little later: look again */
            struct timespec d = {0, 30000000};
            nanosleep(&d, NULL);
            pthread_mutex_lock(&g_vkr.qmu);
            r = vkQueueSubmit(g_vkr.queue, 1, &e, VK_NULL_HANDLE);
            if (r == VK_SUCCESS) r = vkQueueWaitIdle(g_vkr.queue);
            pthread_mutex_unlock(&g_vkr.qmu);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1_);
    if (r != VK_SUCCESS)
        som_log("vkr: the failing wait took %lld ms", (long long)((t1_.tv_sec - t0_.tv_sec) * 1000 + (t1_.tv_nsec - t0_.tv_nsec) / 1000000));
    if (r != VK_SUCCESS) {
        som_log("vkr: GPU fault after %s: vs %s fs %s count %d indexed %d prim %d (result %d)", what, vs, fs, count, indexed, prim, (int)r);
        for (unsigned k = g_nrecent > 16 ? g_nrecent - 16 : 0; k < g_nrecent; k++) som_log("  recent: %s", g_recent[k % 16]);
        g_debug_flush = 0;
    }
    cmd_begin_fwd();
}

static void skip(int line) {
    static int seen[64], n;
    R.draws_skipped++;
    for (int i = 0; i < n; i++) if (seen[i] == line) return;
    if (n < 64) seen[n++] = line;
    som_log("vkr: draw skipped at vkr.c:%d", line);
}

/* per-submit caches of the draw path (reset in vkr_submit_begin) */
static int sup_ok, zero_ok;
static float sup_w, sup_h;
static uint64_t sup_off, zero_off;
#define DSET_SLOTS 16384
static struct { uint64_t h1, h2; VkDescriptorSet set; uint32_t stamp; } dsets[DSET_SLOTS];
static uint32_t dset_stamp = 1;

/* 2048 zero bytes in the ring: stands in for every buffer the game has not bound */
static int zero_block(uint64_t *off) {
    if (!zero_ok) {
        uint8_t *p = ring_alloc(2048, 256, &zero_off);
        if (!p) return 0;
        memset(p, 0, 2048);
        zero_ok = 1;
    }
    *off = zero_off;
    return 1;
}

static char last_vk[40], last_fk[40];
static char skip_list[16][24];
static int nskip;
/* start of a submit: re-read <data>/skip_shaders every 2 s */
static void skip_refresh(void) {
    static uint64_t next;
    uint64_t now = now_ns();
    if (now < next) return;
    next = now + 2000000000ull;
    char p[600];
    snprintf(p, sizeof p, "%s/skip_shaders", som_data_dir());
    FILE *f = fopen(p, "r");
    int n = 0;
    while (f && n < 16 && fscanf(f, "%23s", skip_list[n]) == 1) n++;
    if (f) fclose(f);
    if (n != nskip) som_log("vkr: skipping draws of %d shader prefixes", n);
    nskip = n;
}
static void do_draw_inner(int prim, int indexed, int count, int first, int base_vertex, int base_instance, int instances, uint64_t ibuf, int itype) {
    R.draws++;
    if (!begin_pass()) { skip(__LINE__); return; }
    if ((vkr_flags() & 32) && R.n_color == 0) { skip(__LINE__); return; } /* experiment: no depth-only draws */
    /* The shaders of the two stages. Resolving one (key text, cache lookups) is only repeated when the tracker
     * (gfx_null.c) says something its key depends on changed: another program, attribute or target types, the type
     * of a bound texture. */
    static struct { int valid; uint32_t gen; char key[40]; Shader *sh; const uint8_t *ctl, *code; uint32_t len; } sc[2];
    for (int s = 0; s < 2; s++) {
        uint32_t g = ts_stage_gen(s);
        if (sc[s].valid && sc[s].gen == g) continue;
        sc[s].valid = 0;
        sc[s].gen = g;
        if (!ts_shader_key(s, sc[s].key, &sc[s].ctl, &sc[s].code, &sc[s].len)) { skip(__LINE__); return; }
        sc[s].sh = shader_load(sc[s].key, s, ts_stage_plain(s));
        sc[s].valid = sc[s].sh != NULL; /* a missing shader is looked for again */
    }
    const char *vk = sc[0].key, *fk = sc[1].key;
    memcpy(last_vk, vk, 40), memcpy(last_fk, fk, 40);
    /* debug: <data>/skip_shaders lists shader key prefixes (vertex or fragment, one per line, re-read every 2 s); their
     * draws are skipped, to bisect which pass breaks an image */
    for (int i = 0; i < nskip; i++)
        if (!strncmp(vk, skip_list[i], strlen(skip_list[i])) || !strncmp(fk, skip_list[i], strlen(skip_list[i]))) return;
    Shader *vs = sc[0].sh, *fs = sc[1].sh;
    const Img *sampled[40];
    int n_sampled = 0;
    int quads = prim == NVN_DRAW_PRIMITIVE_QUADS;
    if (quads) prim = NVN_DRAW_PRIMITIVE_TRIANGLES;
    if (!vs || !fs || vs->unsupported || fs->unsupported || topo_of(prim) == (VkPrimitiveTopology)-1) {
        static int n;
        if (n++ < 8) som_log("vkr: draw not possible: vs %s fs %s unsupported %d/%d prim %d", vk, fk, vs ? vs->unsupported : -1, fs ? fs->unsupported : -1, prim);
        skip(__LINE__);
        return;
    }

    /* shader code pointers for the c1 constants */
    const uint8_t *ctl[2] = {sc[0].ctl, sc[1].ctl}, *cd[2] = {sc[0].code, sc[1].code};
    uint32_t cl[2] = {sc[0].len, sc[1].len};

    VkDescriptorSet sets[2];
    Shader *sh[2] = {vs, fs};
    for (int s = 0; s < 2; s++) {
        VkWriteDescriptorSet w[40];
        VkDescriptorBufferInfo bi[20];
        VkDescriptorImageInfo ii[20];
        uint32_t nw = 0, nbi = 0, nii = 0;
        /* binding 0: support buffer (the same for every draw into a viewport of the same size) */
        {
            float rw = (float)(R.has_vp ? R.vp[2] : R.rt_w), rh = (float)(R.has_vp ? R.vp[3] : R.rt_h);
            if (!sup_ok || sup_w != rw || sup_h != rh) {
                uint64_t off;
                uint8_t *p = ring_alloc(1408, 256, &off);
                if (!p) { skip(__LINE__); return; }
                memset(p, 0, 1408);
                float inv[4] = {rw > 0 ? 2.0f / rw : 0, rh > 0 ? 2.0f / rh : 0, 0, 0}, size[4] = {rw, rh, 0, 0}, one = 1.0f, ps = 1.0f;
                memcpy(p + 144, inv, 16);
                memcpy(p + 160, size, 16);
                memcpy(p + 1392, &ps, 4);
                for (int k = 0; k < 73; k++) memcpy(p + 192 + 16 * k, &one, 4);
                sup_ok = 1, sup_w = rw, sup_h = rh, sup_off = off;
            }
            bi[nbi] = (VkDescriptorBufferInfo){ring_buf, sup_off, 1408};
            w[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bi[nbi], NULL};
            nbi++;
        }
        for (int k = 0; k < sh[s]->ncb && nbi < 20; k++) {
            int slot = sh[s]->cb[k].slot;
            uint64_t off = 0, range = 256;
            VkBuffer buf = ring_buf;
            int ok = 1, zero = 0;
            if (slot == 1 && cd[s]) {
                uint32_t c1;
                memcpy(&c1, ctl[s] + 0x700, 4);
                uint64_t n = cl[s] > c1 ? cl[s] - c1 : 0;
                if (n > 65536) n = 65536;
                if (!n) zero = 1;
                else if (!dd_find((uint64_t)(uintptr_t)(cd[s] + c1), n, DD_C1, &off)) {
                    uint8_t *p = ring_alloc(n, 256, &off);
                    ok = p != NULL;
                    if (p) memcpy(p, cd[s] + c1, n), dd_add((uint64_t)(uintptr_t)(cd[s] + c1), n, DD_C1, off);
                }
                range = n ? n : 256;
            } else if (slot >= 3 && slot - 3 < 16) {
                ok = (int)ubo_bytes(s == 0 ? 0 : 1, slot - 3, &buf, &off, &range);
            } else {
                /* c0 is the NVN driver buffer: storage buffer slot i is described at 0x110 + 16 i (address, size).
                 * The game's shaders read the address and bounds-check every access against the size, so a zero
                 * descriptor made every storage read return 0 (the battle scene's instanced particles then built a
                 * zero transform and wrote NaN, which bloom spread into a black and white image). */
                int described = 0;
                for (int j = 0; slot == 0 && j < sh[s]->nsb; j++) {
                    int sl = sh[s]->sb[j].slot;
                    const p_bind_buf *b = sl >= 0 && sl < 16 ? &R.ssbo[s == 0 ? 0 : 1][sl] : NULL;
                    described |= b && b->addr && b->size;
                }
                range = 2048;
                if (!described) zero = 1;
                else {
                    uint8_t *p = ring_alloc(2048, 256, &off);
                    ok = p != NULL;
                    if (p) memset(p, 0, 2048);
                    for (int j = 0; p && j < sh[s]->nsb; j++) {
                        int sl = sh[s]->sb[j].slot;
                        const p_bind_buf *b = sl >= 0 && sl < 16 ? &R.ssbo[s == 0 ? 0 : 1][sl] : NULL;
                        if (!b || !b->addr || !b->size) continue;
                        uint64_t addr = b->addr;
                        uint32_t size = b->size > 0xffffffffull ? 0xffffffffu : (uint32_t)b->size;
                        memcpy(p + 0x110 + 16 * sl, &addr, 8);
                        memcpy(p + 0x118 + 16 * sl, &size, 4);
                    }
                }
            }
            if (zero) ok = zero_block(&off);
            if (!ok) { skip(__LINE__); return; }
            bi[nbi] = (VkDescriptorBufferInfo){buf, off, range};
            w[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, VK_NULL_HANDLE, sh[s]->cb[k].binding, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bi[nbi], NULL};
            nbi++;
        }
        /* storage buffers: the bound range is copied into the ring like a uniform buffer (read-only: shader writes to
         * it do not reach guest memory) */
        for (int k = 0; k < sh[s]->nsb && nbi < 20; k++) {
            int slot = sh[s]->sb[k].slot;
            const p_bind_buf *b = slot >= 0 && slot < 16 ? &R.ssbo[s == 0 ? 0 : 1][slot] : NULL;
            uint64_t n = b && b->addr && b->size ? b->size : 0, off, range = 256;
            if (n > (4u << 20)) {
                static int warned;
                if (warned++ < 4) som_log("vkr: storage buffer of %llu bytes cut to 4 MB", (unsigned long long)n);
                n = 4u << 20;
            }
            /* the shaders are translated with sbuf_align 64 (Ryujinx's unaligned path): they address the buffer from its
             * guest address rounded down to 64 (offset = address - (base & -64)), so the copy starts there. Copying from
             * the exact address shifted every access of a buffer not 64-aligned: the battle scene's instanced particles
             * read garbage transforms and wrote NaN, which bloom spread into a black and white image. */
            uint64_t lead = n ? (b->addr & 63) : 0;
            if (n && dd_find(b->addr, n, DD_SB, &off)) {
                range = n + lead;
                PS.n_dedup++;
            } else if (n && nvn_pool_find(b->addr, n)) {
                uint8_t *p = ring_alloc(n + lead, 256, &off);
                if (!p) { skip(__LINE__); return; }
                if (lead && nvn_pool_find(b->addr - lead, n + lead)) memcpy(p, (const void *)(uintptr_t)(b->addr - lead), n + lead);
                else memset(p, 0, lead), memcpy(p + lead, (const void *)(uintptr_t)b->addr, n);
                PS.b_sb += n;
                dd_add(b->addr, n, DD_SB, off);
                range = n + lead;
            } else if (!zero_block(&off)) { skip(__LINE__); return; }
            bi[nbi] = (VkDescriptorBufferInfo){ring_buf, off, range};
            w[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, VK_NULL_HANDLE, sh[s]->sb[k].binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[nbi], NULL};
            nbi++;
        }
        for (int k = 0; k < sh[s]->ntx && nii < 20; k++) {
            const TexRes *tr = &sh[s]->tx[k];
            int stg = s == 0 ? 0 : 1;
            uint64_t h = tr->slot >= 0 && tr->slot < 32 ? R.texh[stg][tr->slot] : 0;
            const tex_t *t = h ? tex_by_id[h & 0xfffff] : NULL;
            Img *im = NULL;
            if (t) {
                im = img_by_id[h & 0xfffff];
                if (!im || im->serial != t->serial) im = img_by_id[h & 0xfffff] = img_get(t);
            }
            if (im && (vkr_flags() & 1) && im->fmt >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && im->fmt <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) im = NULL;
            if (im && (vkr_flags() & 16) && im->is_depth) im = NULL; /* experiment: no sampled depth textures */
            int shadow = (tr->type & 0x400) != 0; /* SamplerType.Shadow = 1 << 10 */
            if (shadow && im && !im->is_depth) im = NULL; /* a colour image cannot be depth-compared */
            if (!im) im = shadow ? dummy_depth_image() : dummy_image();
            if (!im) { skip(__LINE__); return; }
            img_init(im);
            img_load(im);
            uint64_t sh_h = tr->sampler_slot >= 0 && tr->sampler_slot < 32 && R.samph[stg][tr->sampler_slot] ? R.samph[stg][tr->sampler_slot] : h;
            int sid = (int)(sh_h >> 20);
            /* A separate sampler handle is the sampler id << 20 (nvnDeviceGetSeparateSamplerHandle), like the sampler
             * part of a combined handle. Reading the low 20 bits gave sampler 0 for every separately sampled texture:
             * its clamp wrap smeared the tiling floor and wall textures of the maps into streaks. */
            if (tr->sampler_slot >= 0 && R.samph[stg][tr->sampler_slot]) sid = (int)(R.samph[stg][tr->sampler_slot] >> 20 & 0xfff);
            static int nlog;
            if (R.dbg || nlog < 40) {
                char bl[300];
                snprintf(bl, sizeof bl, "vkr: tex bind stage %d binding %u slot %d: handle %#llx img %p fmt %d %ux%u d%u L%u M%u target %d type %u sampler %d rt %d loaded %d",
                         stg, tr->binding, tr->slot, (unsigned long long)h, (void *)im, (int)im->fmt, im->w, im->h, im->depth,
                         im->layers, im->levels, im->t->d.target, tr->type, sid, im->is_rt, im->loaded);
                recent_note(bl);
                if (nlog++ < 40)
                    som_log("vkr: tex bind stage %d binding %u slot %d: handle %#llx id %llu img %p fmt %d %ux%u d%u L%u M%u target %d shader type %u sampler %d is_rt %d loaded %d",
                            stg, tr->binding, tr->slot, (unsigned long long)h, (unsigned long long)(h & 0xfffff), (void *)im, (int)im->fmt, im->w, im->h,
                            im->depth, im->layers, im->levels, im->t->d.target, tr->type, sid, im->is_rt, im->loaded);
            }
            if (n_sampled < 40) sampled[n_sampled++] = im;
            if (cap_left == 1) {
                const samp_par *sp = sid >= 0 && sid < 4096 && samp_have[sid] ? &samp_by_id[sid] : NULL;
                som_log("captex s%d b%u slot %d: serial %llu fmt %d %ux%u L%u M%u rt %d type %#x%s sampler %d (min %d mag %d wrap %d %d cmp %d/%d aniso %g lod %g..%g)",
                        stg, tr->binding, tr->slot, (unsigned long long)im->serial, (int)im->fmt, im->w, im->h, im->layers, im->levels, im->is_rt,
                        tr->type, shadow ? " shadow" : "", sid, sp ? sp->minf : -1, sp ? sp->magf : -1, sp ? sp->ws : -1, sp ? sp->wt : -1,
                        sp ? sp->cmp_mode : -1, sp ? sp->cmp_func : -1, sp ? sp->aniso : 0.f, sp ? sp->lod_min : 0.f, sp ? sp->lod_max : 0.f);
            }
            VkImageView tvw = t && im->t == t ? tex_view(im, &view_by_id[h & 0xfffff]) : im->view;
            ii[nii] = (VkDescriptorImageInfo){sampler_for(sid, shadow, !im->linear_ok), tvw, VK_IMAGE_LAYOUT_GENERAL};
            w[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, VK_NULL_HANDLE, tr->binding, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[nii], NULL, NULL};
            nii++;
        }
        /* The set: one with exactly these contents made earlier in this submit is used again (materials shared by
         * many draws), else a new one is allocated and written. */
        {
            uint64_t h1 = (uint64_t)(uintptr_t)sh[s]->layout, h2 = ~h1;
#define DMIX(v) do { uint64_t v_ = (uint64_t)(v); h1 = (h1 ^ v_) * 0x9E3779B97F4A7C15ull; h1 ^= h1 >> 32; h2 = (h2 + v_) * 0xC2B2AE3D27D4EB4Full; h2 ^= h2 >> 29; } while (0)
            for (uint32_t k = 0; k < nw; k++) {
                DMIX(w[k].dstBinding | (uint64_t)w[k].descriptorType << 32);
                if (w[k].pBufferInfo) {
                    DMIX(w[k].pBufferInfo->offset);
                    DMIX(w[k].pBufferInfo->range);
                } else {
                    DMIX((uintptr_t)w[k].pImageInfo->sampler);
                    DMIX((uintptr_t)w[k].pImageInfo->imageView);
                }
            }
#undef DMIX
            uint32_t slot = (uint32_t)(h1 >> 40) & (DSET_SLOTS - 1), tries = 0;
            for (; tries < 8; tries++, slot = (slot + 1) & (DSET_SLOTS - 1)) {
                if (dsets[slot].stamp != dset_stamp) break; /* free */
                if (dsets[slot].h1 == h1 && dsets[slot].h2 == h2) break;
            }
            if (tries < 8 && dsets[slot].stamp == dset_stamp) {
                sets[s] = dsets[slot].set;
                PS.n_dedup++;
            } else {
                PF_T0();
                VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, R.dpool, 1, &sh[s]->layout};
                VkResult ar = vkAllocateDescriptorSets(g_vkr.dev, &ai, &sets[s]);
                if (ar != VK_SUCCESS) { skip(__LINE__); return; }
                for (uint32_t k = 0; k < nw; k++) w[k].dstSet = sets[s];
                vkUpdateDescriptorSets(g_vkr.dev, nw, w, 0, NULL);
                PF_T1(PF_DESC);
                PS.n_set++;
                if (tries < 8) dsets[slot].stamp = dset_stamp, dsets[slot].h1 = h1, dsets[slot].h2 = h2, dsets[slot].set = sets[s];
            }
        }
    }

    /* vertex streams used by the attributes */
    uint32_t used = 0;
    for (int l = 0; l < 16; l++) {
        va_st a;
        memcpy(&a, &R.vattr[l], sizeof a);
        if (a.fmt && vtx_fmt((int)a.fmt) != VK_FORMAT_UNDEFINED) {
            used |= 1u << a.stream;
            if (!R.dbg) continue;
            char al[120];
            snprintf(al, sizeof al, "attr %d fmt %d stream %d offset %u -> vk %d", l, (int)a.fmt, (int)a.stream, (unsigned)a.offset,
                     (int)vtx_fmt((int)a.fmt));
            recent_note(al);
        }
    }
    VkBuffer vbufs[16];
    VkDeviceSize voffs[16];
    uint32_t firstb = 16, lastb = 0;
    for (int sidx = 0; sidx < 16; sidx++) {
        if (!(used >> sidx & 1)) continue;
        const NVNbufferRange *r = &R.vbuf[sidx];
        uint64_t n = r->size > (32u << 20) ? (32u << 20) : r->size, off;
        if (r->address && n && dd_find(r->address, n, DD_VB, &off)) PS.n_dedup++;
        else {
            if (!r->address || !n || !nvn_pool_find(r->address, n)) { skip(__LINE__); return; }
            uint8_t *p = ring_alloc(n, 16, &off);
            if (!p) { skip(__LINE__); return; }
            PF_T0();
            memcpy(p, (const void *)(uintptr_t)r->address, n);
            PF_T1(PF_COPY);
            PS.b_vb += n;
            dd_add(r->address, n, DD_VB, off);
        }
        vbufs[sidx] = ring_buf;
        voffs[sidx] = off;
        if (R.dbg) {
            char vl[200];
            snprintf(vl, sizeof vl, "vbuf stream %d addr %#llx size %llu stride %u divisor %u", sidx, (unsigned long long)r->address,
                     (unsigned long long)r->size, R.vstream[sidx].stride, R.vstream[sidx].divisor);
            recent_note(vl);
        }
        if ((uint32_t)sidx < firstb) firstb = (uint32_t)sidx;
        if ((uint32_t)sidx > lastb) lastb = (uint32_t)sidx;
    }
    if (!begin_pass()) { skip(__LINE__); return; } /* a first-time texture upload may have ended it */
    cap_draw(vk, fk);
    for (int k = 0; k < n_sampled; k++) cap_sample(sampled[k]);
    if (cap_left == 1) { /* the last captured submit: one line per draw */
        char tl[160];
        int tn = 0;
        tl[0] = 0;
        for (int k = 0; k < n_sampled && tn < 140; k++) tn += snprintf(tl + tn, sizeof tl - (size_t)tn, " %llu", (unsigned long long)sampled[k]->serial);
        char ul[200];
        int un = 0;
        ul[0] = 0;
        for (int k = 0; k < vs->ncb && un < 180; k++) {
            int sl = vs->cb[k].slot;
            const p_bind_buf *b = sl >= 3 && sl - 3 < 16 ? &R.ubo[0][sl - 3] : NULL;
            un += snprintf(ul + un, sizeof ul - (size_t)un, " c%d=%llu%s", sl, b ? (unsigned long long)b->size : 0ull,
                           b && b->addr && b->size && !nvn_pool_find(b->addr, b->size) ? "(no pool)" : "");
        }
        for (int sidx = 0; sidx < 16 && un < 180; sidx++)
            if (used >> sidx & 1)
                un += snprintf(ul + un, sizeof ul - (size_t)un, " vb%d=%llu/%u", sidx, (unsigned long long)R.vbuf[sidx].size, R.vstream[sidx].stride);
        char al2[300];
        int an = 0;
        al2[0] = 0;
        for (int l = 0; l < 16 && an < 280; l++) {
            va_st a;
            memcpy(&a, &R.vattr[l], sizeof a);
            if (a.fmt) an += snprintf(al2 + an, sizeof al2 - (size_t)an, " a%d=f%#x@%u/s%u->%d", l, (unsigned)a.fmt, (unsigned)a.offset, (unsigned)a.stream, (int)vtx_fmt((int)a.fmt));
        }
        som_log("capattr%s", al2);
        {
            VkRect2D sc = scissor_rect();
            som_log("capstate scissor %d,%d %ux%u (set %d) rt %ux%u chmask %#x blend_en %#x blend0 src %u dst %u eq %u alpha_func %u depth_range %.3g..%.3g polyoff %g %g %g en %d",
                    sc.offset.x, sc.offset.y, sc.extent.width, sc.extent.height, R.scissor_set, R.rt_w, R.rt_h, R.chmask.mask,
                    (unsigned)R.cstate.blend_en, (unsigned)R.blend[0].src_rgb, (unsigned)R.blend[0].dst_rgb, (unsigned)R.blend[0].eq_rgb,
                    (unsigned)R.cstate.alpha_func, R.depth_range[0], R.depth_range[1], R.poly_off[0], R.poly_off[1], R.poly_off[2], (int)R.poly.offset_en);
        }
        /* the vertex constant buffers (first 32 floats each) and attribute 0 of the first vertex */
        for (int k = 0; k < vs->ncb; k++) {
            int sl = vs->cb[k].slot;
            const p_bind_buf *b = sl >= 3 && sl - 3 < 16 ? &R.ubo[0][sl - 3] : NULL;
            if (!b || !b->addr || !b->size || !nvn_pool_find(b->addr, b->size)) continue;
            const float *fv = (const float *)(uintptr_t)b->addr;
            int nf = b->size / 4 < 32 ? (int)(b->size / 4) : 32;
            char fl[700];
            int fn = 0;
            for (int i = 0; i < nf && fn < 680; i++) fn += snprintf(fl + fn, sizeof fl - (size_t)fn, " %.4g", fv[i]);
            som_log("capub c%d:%s", sl, fl);
        }
        /* vertex storage buffers: binding, and (for Unity's 76-byte particle instance records indexed by c4 word 40
         * plus the instance) the first two records the draw reads */
        for (int k = 0; k < vs->nsb; k++) {
            int sl = vs->sb[k].slot;
            const p_bind_buf *b = sl >= 0 && sl < 16 ? &R.ssbo[0][sl] : NULL;
            if (!b) continue;
            const p_bind_buf *c4 = &R.ubo[0][1];
            int32_t base = 0;
            if (c4->addr && c4->size >= 164 && nvn_pool_find(c4->addr, 164)) memcpy(&base, (const void *)(uintptr_t)(c4->addr + 160), 4);
            int inpool = b->addr && b->size && nvn_pool_find(b->addr, b->size) != NULL;
            som_log("capsb slot %d addr %#llx size %llu in pool %d, c4 word 40 = %d (record bytes %lld..)", sl, (unsigned long long)b->addr,
                    (unsigned long long)b->size, inpool, base, (long long)base * 76);
            for (int r = 0; r < 2 && inpool; r++) {
                uint64_t o = (uint64_t)(int64_t)(base + r) * 76;
                if (base + r < 0 || o + 76 > b->size) { som_log("capsb record %d: out of range", base + r); continue; }
                const float *fv = (const float *)(uintptr_t)(b->addr + o);
                char fl[400];
                int fn = 0;
                for (int i = 0; i < 19 && fn < 380; i++) fn += snprintf(fl + fn, sizeof fl - (size_t)fn, " %.4g", fv[i]);
                som_log("capsb record %d:%s", base + r, fl);
            }
        }
        for (int k = 0; k < fs->ncb; k++) {
            int sl = fs->cb[k].slot;
            const p_bind_buf *b = sl >= 3 && sl - 3 < 16 ? &R.ubo[1][sl - 3] : NULL;
            if (!b || !b->addr || !b->size || !nvn_pool_find(b->addr, b->size)) {
                som_log("capfub c%d: %s", sl, sl == 1 ? "(shader constants)" : "(not bound)");
                continue;
            }
            const float *fv = (const float *)(uintptr_t)b->addr;
            int lim = b->size <= 512 ? 128 : 48; /* per-draw and shadow blocks whole, the big ones by their start */
            int nf = b->size / 4 < (uint64_t)lim ? (int)(b->size / 4) : lim, bad = 0;
            for (uint64_t i = 0; i < b->size / 4; i++) bad += fv[i] != fv[i] || fv[i] > 1e30f || fv[i] < -1e30f;
            char fl[2400];
            int fn = 0;
            for (int i = 0; i < nf && fn < 2380; i++) fn += snprintf(fl + fn, sizeof fl - (size_t)fn, " %.4g", fv[i]);
            som_log("capfub c%d (%llu bytes, %d nan/inf):%s", sl, (unsigned long long)b->size, bad, fl);
        }
        {
            va_st a;
            memcpy(&a, &R.vattr[0], sizeof a);
            const NVNbufferRange *r = &R.vbuf[a.stream];
            if (a.fmt == 0x22 && r->address && r->size >= a.offset + 12u) {
                const float *v = (const float *)(uintptr_t)(r->address + a.offset);
                som_log("capvtx %.4g %.4g %.4g", v[0], v[1], v[2]);
            }
        }
        som_log("capdraw %s/%s %s n %d inst %d prim %d vp %.0f,%.0f %.0fx%.0f depth t%d w%d f%d cull %d ssbo %d/%d vs-ubo%s tex%s", vk, fk,
                indexed ? "idx" : "arr", count, instances, prim, R.vp[0], R.vp[1], R.vp[2], R.vp[3], (int)R.ds.dt, (int)R.ds.dw,
                (int)R.ds.df, (int)R.poly.cull, vs->nsb, fs->nsb, ul, tl);
    }
    static struct { Shader *vs, *fs; int prim, minus_w; uint32_t used, gen; VkRenderPass rp; VkPipeline p; VkPipelineLayout l; } lp;
    int minus_w = nvn_depth_minus_w();
    if (!lp.p || lp.vs != vs || lp.fs != fs || lp.prim != prim || lp.used != used || lp.gen != R.pgen || lp.rp != R.rp || lp.minus_w != minus_w) {
        lp.l = pl_get(vs->layout, fs->layout);
        lp.p = pipeline_for(vs, fs, prim, lp.l, used);
        lp.vs = vs, lp.fs = fs, lp.prim = prim, lp.used = used, lp.gen = R.pgen, lp.rp = R.rp, lp.minus_w = minus_w;
    }
    VkPipelineLayout layout = lp.l;
    VkPipeline pipe = lp.p;
    if (!pipe) {
        /* still compiling on a worker: leave this draw out (lp.p stays NULL, so the next draw asks again) */
        if (g_pipe_late) { PS.n_late++, R.draws_skipped++; return; }
        skip(__LINE__);
        return;
    }
    /* experiment: -3 skips the draws of passes into a presented (window) image */
    if (g_skip_pass != -1 && (g_skip_pass == -2 || (pass_no >= g_skip_pass && pass_no <= g_skip_hi) ||
                              (g_skip_pass == -3 && R.n_color && R.color[0] && R.color[0]->presented))) {
        gp_first_draw(fs->key);
        return;
    }
    vkCmdBindPipeline(R.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    gp_first_draw(fs->key);
    set_dynamic();
    vkCmdBindDescriptorSets(R.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, sets, 0, NULL);
    for (uint32_t sidx = firstb; sidx <= lastb && used; sidx++)
        if (used >> sidx & 1) vkCmdBindVertexBuffers(R.cb, sidx, 1, &vbufs[sidx], &voffs[sidx]);
    uint32_t ninst = instances > 0 ? (uint32_t)instances : 1;
    if (quads) {
        /* quads become triangle pairs (0 1 2)(0 2 3) through a generated 32-bit index buffer */
        uint32_t nq = (uint32_t)count / 4, isz = itype == NVN_INDEX_TYPE_UNSIGNED_INT ? 4 : 2;
        uint64_t off;
        uint32_t *ix = (uint32_t *)ring_alloc((uint64_t)nq * 6 * 4, 16, &off);
        if (!ix) { skip(__LINE__); return; }
        const uint8_t *src = indexed ? (const uint8_t *)(uintptr_t)ibuf : NULL;
        if (indexed && (!ibuf || !nvn_pool_find(ibuf, (uint64_t)count * isz))) { skip(__LINE__); return; }
        for (uint32_t q = 0; q < nq; q++) {
            uint32_t v[4];
            for (int k = 0; k < 4; k++) {
                uint32_t i = q * 4 + (uint32_t)k;
                v[k] = indexed ? (isz == 4 ? ((const uint32_t *)src)[i] : ((const uint16_t *)src)[i]) : i;
            }
            static const int tri[6] = {0, 1, 2, 0, 2, 3};
            for (int k = 0; k < 6; k++) ix[q * 6 + (uint32_t)k] = v[tri[k]];
        }
        vkCmdBindIndexBuffer(R.cb, ring_buf, off, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(R.cb, nq * 6, ninst, 0, indexed ? base_vertex : first, (uint32_t)base_instance);
        return;
    }
    if (indexed) {
        uint32_t isz = itype == NVN_INDEX_TYPE_UNSIGNED_INT ? 4 : 2;
        if (itype == NVN_INDEX_TYPE_UNSIGNED_BYTE) { skip(__LINE__); return; }
        uint64_t n = (uint64_t)count * isz, off;
        if (ibuf && dd_find(ibuf, n, DD_IB, &off)) PS.n_dedup++;
        else {
            if (!ibuf || !nvn_pool_find(ibuf, n)) { skip(__LINE__); return; }
            uint8_t *p = ring_alloc(n, 16, &off);
            if (!p) { skip(__LINE__); return; }
            memcpy(p, (const void *)(uintptr_t)ibuf, n);
            PS.b_ib += n;
            dd_add(ibuf, n, DD_IB, off);
        }
        vkCmdBindIndexBuffer(R.cb, ring_buf, off, isz == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(R.cb, (uint32_t)count, ninst, 0, base_vertex, (uint32_t)base_instance);
    } else {
        vkCmdDraw(R.cb, (uint32_t)count, ninst, (uint32_t)first, (uint32_t)base_instance);
    }
}

static void do_draw(int prim, int indexed, int count, int first, int base_vertex, int base_instance, int instances, uint64_t ibuf, int itype) {
    uint64_t before = R.draws_skipped;
    if (R.dbg) {
        char dl[300];
        snprintf(dl, sizeof dl, "draw prim %d indexed %d count %d inst %d ibuf %#llx itype %d rt %ux%u ncol %d | dt %d dw %d df %d st %d | dr %g..%g vp %g,%g %gx%g | bias %d",
                 prim, indexed, count, instances, (unsigned long long)ibuf, itype, R.rt_w, R.rt_h, R.n_color, (int)R.ds.dt, (int)R.ds.dw,
                 (int)R.ds.df, (int)R.ds.st, R.depth_range[0], R.depth_range[1], R.has_vp ? R.vp[0] : -1.0, R.has_vp ? R.vp[1] : -1.0,
                 R.has_vp ? R.vp[2] : -1.0, R.has_vp ? R.vp[3] : -1.0, (int)R.poly.offset_en);
        recent_note(dl);
    }
    do_draw_inner(prim, indexed, count, first, base_vertex, base_instance, instances, ibuf, itype);
    if (R.draws_skipped == before && R.in_pass && (!(vkr_flags() & 128) || R.n_color == 0))
        flush_debug("draw", last_vk, last_fk, count, indexed, prim);
}

/* ---- clears */
/* the channels a colour format stores (NVN clear mask bits: 1 R, 2 G, 4 B, 8 A) */
static int fmt_chan_mask(VkFormat f) {
    if ((f >= VK_FORMAT_R8_UNORM && f <= VK_FORMAT_R8_SRGB) || (f >= VK_FORMAT_R16_UNORM && f <= VK_FORMAT_R16_SFLOAT) ||
        (f >= VK_FORMAT_R32_UINT && f <= VK_FORMAT_R32_SFLOAT))
        return 0x1;
    if ((f >= VK_FORMAT_R8G8_UNORM && f <= VK_FORMAT_R8G8_SRGB) || (f >= VK_FORMAT_R16G16_UNORM && f <= VK_FORMAT_R16G16_SFLOAT) ||
        (f >= VK_FORMAT_R32G32_UINT && f <= VK_FORMAT_R32G32_SFLOAT))
        return 0x3;
    if (f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 || f == VK_FORMAT_R5G6B5_UNORM_PACK16 ||
        f == VK_FORMAT_B5G6R5_UNORM_PACK16)
        return 0x7;
    return 0xf;
}

/* a clear before anything else in the pass that covers the whole target becomes the pass's load op */
static int clear_on_load(const VkClearRect *rc) {
    return R.in_pass == 2 && rc->rect.offset.x == 0 && rc->rect.offset.y == 0 && rc->rect.extent.width == R.rt_w &&
           rc->rect.extent.height == R.rt_h && !(vkr_flags() & 524288); /* 524288: experiment, always clear in the pass */
}

static void do_clear_color(const p_clear_color *c) {
    if (!pass_setup(1)) return;
    if (c->index < 0 || c->index >= R.n_color || !R.color[c->index]) return;
    int have = fmt_chan_mask(R.color[c->index]->fmt);
    if (cap_left == 1)
        som_log("capclear c%d=%llu(fmt %d) mask %#x rgba %g %g %g %g", c->index, (unsigned long long)R.color[c->index]->serial,
                (int)R.color[c->index]->fmt, c->mask, c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]);
    /* a mask that covers every channel the format stores is a full clear (Unity clears an alpha-less B10G11R11 target
     * with mask RGB: dropping it left the battle scene's sky uncleared, and stale NaNs there blew up the bloom) */
    if ((c->mask & have) != have) {
        static int n;
        if (n++ < 8) som_log("vkr: partial colour clear dropped: mask %#x on format %d", c->mask, (int)R.color[c->index]->fmt);
        return;
    }
    VkClearAttachment a = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)c->index, {.color = {{c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]}}}};
    VkClearRect rc = {scissor_rect(), 0, 1};
    R.clears++;
    if (clear_on_load(&rc)) {
        R.pclear |= 1u << c->index;
        R.pcv[c->index].color = a.clearValue.color;
        return;
    }
    begin_pass();
    if (rc.rect.extent.width && rc.rect.extent.height) vkCmdClearAttachments(R.cb, 1, &a, 1, &rc);
}

static void do_clear_ds(const p_clear_ds *c) {
    if (!pass_setup(1) || !R.depth) return;
    VkImageAspectFlags as = 0;
    if (c->depth_mask) as |= VK_IMAGE_ASPECT_DEPTH_BIT;
    /* a partial stencil write mask is not a full stencil clear: that one stays in the pass */
    if (c->stencil_mask && (R.depth->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) as |= VK_IMAGE_ASPECT_STENCIL_BIT;
    as &= R.depth->aspect;
    if (!as) return;
    VkClearAttachment a = {as, 0, {.depthStencil = {c->depth, (uint32_t)c->stencil}}};
    if (cap_left == 1) som_log("capclear ds=%llu(fmt %d) depth %g stencil %d masks %d %#x", (unsigned long long)R.depth->serial, (int)R.depth->fmt, c->depth, (int)c->stencil, (int)c->depth_mask, (unsigned)c->stencil_mask);
    VkClearRect rc = {scissor_rect(), 0, 1};
    if (clear_on_load(&rc) && (!(as & VK_IMAGE_ASPECT_STENCIL_BIT) || (c->stencil_mask & 0xff) == 0xff)) {
        if (as & VK_IMAGE_ASPECT_DEPTH_BIT) R.pclear |= 1u << 8, R.pcv[MAXI].depthStencil.depth = c->depth;
        if (as & VK_IMAGE_ASPECT_STENCIL_BIT) R.pclear |= 1u << 9, R.pcv[MAXI].depthStencil.stencil = (uint32_t)c->stencil;
        return;
    }
    begin_pass();
    if (rc.rect.extent.width && rc.rect.extent.height) vkCmdClearAttachments(R.cb, 1, &a, 1, &rc);
}

static void do_copy_b2t(const p_copy_b2t *c) {
    end_pass();
    Img *i = img_get(c->tex);
    if (!i || !c->src || !nvn_pool_find(c->src, 1)) return;
    img_init(i);
    i->loaded = 1;
    const NVNcopyRegion *r = &c->r;
    {
        static unsigned n;
        if (r->width >= 480 && n++ < 12) som_log("vkr: buffer-to-texture copy %dx%d into serial %llu (%ux%u fmt %#x)", r->width, r->height,
                              (unsigned long long)c->tex->serial, c->tex->d.width, c->tex->d.height, c->tex->d.format);
    }
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    if (!bpb) return;
    uint32_t w = (uint32_t)r->width, h = (uint32_t)r->height, dep = r->depth > 0 ? (uint32_t)r->depth : 1;
    uint32_t row = ((w + (uint32_t)bw - 1) / (uint32_t)bw) * (uint32_t)bpb, rows = (h + (uint32_t)bh - 1) / (uint32_t)bh;
    uint64_t stride = R.copy_stride ? R.copy_stride : row, off;
    uint8_t *dst = ring_alloc((uint64_t)row * rows * dep, 16, &off);
    if (!dst) return;
    const uint8_t *src = (const uint8_t *)(uintptr_t)c->src;
    for (uint32_t z = 0; z < dep; z++)
        for (uint32_t y = 0; y < rows; y++) memcpy(dst + ((uint64_t)z * rows + y) * row, src + ((uint64_t)z * rows + y) * stride, row);
    /* array layers and cube faces are Vulkan layers (z of the region); only a 3D texture has depth. Cube maps were
     * treated like 3D: every face went to face 0 at a z offset, an invalid copy that left five faces undefined. */
    int array = c->tex->d.target != NVN_TEXTURE_TARGET_3D && i->depth == 1 && i->layers > 1;
    if (i->drop) { /* a reduced texture (Texture size) written by a copy: should not happen */
        static int warned;
        if (warned++ < 8) som_log("vkr: texture size: buffer-to-texture copy into reduced serial %llu: skipped", (unsigned long long)c->tex->serial);
        return;
    }
    uint32_t level = c->has_view && c->view.set_levels ? c->view.base_level : 0, layer = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    if (level >= i->levels || (array ? (uint32_t)r->zoffset + layer + dep : layer + 1) > i->layers) {
        static int warned;
        if (warned++ < 8) som_log("vkr: buffer-to-texture copy outside serial %llu (level %u of %u, layer %u+%d+%u of %u): skipped",
                                  (unsigned long long)c->tex->serial, level, i->levels, layer, r->zoffset, dep, i->layers);
        return;
    }
    {
        static unsigned ncube;
        if (i->layers > 1 && ncube++ < 12) som_log("vkr: buffer-to-texture copy into layered serial %llu target %d: level %u layer %u z %d depth %u %dx%d",
                                                   (unsigned long long)c->tex->serial, c->tex->d.target, level, layer, r->zoffset, dep, r->width, r->height);
    }
    VkBufferImageCopy rg = {off, 0, 0, {i->aspect, level, array ? (uint32_t)r->zoffset + layer : layer, array ? dep : 1},
                            {r->xoffset, r->yoffset, array ? 0 : r->zoffset}, {w, h, array ? 1 : dep}};
    vkCmdCopyBufferToImage(R.cb, ring_buf, i->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    barrier_all(R.cb);
}

/* nvnCommandBufferCopyTextureToTexture (and Downsample): a copy when the sizes match and nothing is mirrored or
 * filtered, else a blit. The movie player writes each frame into staging textures with WriteTexels and copies them
 * into the textures its shader samples, so the source's CPU data is uploaded first. */
static void do_copy_t2t(const p_copy_t2t *c) {
    end_pass();
    Img *s = c->src ? img_get(c->src) : NULL, *d = c->dst ? img_get(c->dst) : NULL;
    if (!s || !d || s == d) return;
    if (s->drop || d->drop) { /* a reduced texture (Texture size) in a texture copy: should not happen */
        static int warned;
        if (warned++ < 8) som_log("vkr: texture size: texture copy with reduced serial %llu: skipped", (unsigned long long)(s->drop ? s->serial : d->serial));
        return;
    }
    img_init(s);
    img_load(s);
    img_init(d);
    d->loaded = 1; /* its contents now come from the GPU, not from its storage */
    const NVNcopyRegion *sr = &c->sr, *dr = &c->dr;
    int s_arr = c->src->d.target == NVN_TEXTURE_TARGET_2D_ARRAY || c->src->d.target == NVN_TEXTURE_TARGET_1D_ARRAY ||
                c->src->d.target == NVN_TEXTURE_TARGET_CUBEMAP || c->src->d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    int d_arr = c->dst->d.target == NVN_TEXTURE_TARGET_2D_ARRAY || c->dst->d.target == NVN_TEXTURE_TARGET_1D_ARRAY ||
                c->dst->d.target == NVN_TEXTURE_TARGET_CUBEMAP || c->dst->d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    uint32_t s_level = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, d_level = c->has_dv && c->dv.set_levels ? c->dv.base_level : 0;
    uint32_t s_layer = c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0, d_layer = c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0;
    if (s_level >= s->levels || d_level >= d->levels) return;
    uint32_t sd = sr->depth > 0 ? (uint32_t)sr->depth : 1, dd = dr->depth > 0 ? (uint32_t)dr->depth : 1;
    if (s_arr) s_layer += (uint32_t)sr->zoffset;
    if (d_arr) d_layer += (uint32_t)dr->zoffset;
    uint32_t nl = s_arr ? sd : 1;
    if (s_layer + nl > s->layers || d_layer + (d_arr ? dd : 1) > d->layers) return;
    VkImageAspectFlags asp = s->aspect & d->aspect;
    if (!asp) return;
    {
        static unsigned n;
        if (n++ < 8)
            som_log("vkr: texture copy %dx%d serial %llu (fmt %d) -> %dx%d serial %llu (fmt %d) flags %#x", sr->width, sr->height,
                    (unsigned long long)c->src->serial, (int)s->fmt, dr->width, dr->height, (unsigned long long)c->dst->serial,
                    (int)d->fmt, c->flags);
    }
    int s_ms = 0, d_ms = 0; /* multisampled textures are single-sample images (see image creation) */
    int same = sr->width == dr->width && sr->height == dr->height && sd == dd && !(c->flags & 0xD);
    if (same && s_ms && !d_ms && s->fmt == d->fmt && !s->is_depth) {
        VkImageResolve rs = {{asp, s_level, s_layer, nl}, {sr->xoffset, sr->yoffset, 0}, {asp, d_level, d_layer, nl},
                             {dr->xoffset, dr->yoffset, 0}, {(uint32_t)sr->width, (uint32_t)sr->height, 1}};
        vkCmdResolveImage(R.cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rs);
    } else if (s_ms || d_ms) {
        static int warned;
        if (!warned++) som_log("vkr: multisample texture copy skipped");
        return;
    } else if (same && s->fmt == d->fmt) {
        VkImageCopy ic = {{asp, s_level, s_layer, nl}, {sr->xoffset, sr->yoffset, s_arr ? 0 : sr->zoffset},
                          {asp, d_level, d_layer, nl}, {dr->xoffset, dr->yoffset, d_arr ? 0 : dr->zoffset},
                          {(uint32_t)sr->width, (uint32_t)sr->height, s_arr ? 1 : sd}};
        vkCmdCopyImage(R.cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    } else if (!s->is_depth && !d->is_depth) {
        VkFormatProperties sp, dp;
        vkGetPhysicalDeviceFormatProperties(g_vkr.phys, s->fmt, &sp);
        vkGetPhysicalDeviceFormatProperties(g_vkr.phys, d->fmt, &dp);
        if (!(sp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(dp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
            static int warned;
            if (warned++ < 4) som_log("vkr: texture copy fmt %d -> %d needs a blit the formats do not support", (int)s->fmt, (int)d->fmt);
            return;
        }
        int32_t x0 = dr->xoffset, x1 = dr->xoffset + dr->width, y0 = dr->yoffset, y1 = dr->yoffset + dr->height;
        if (c->flags & 4) { int32_t t = x0; x0 = x1; x1 = t; }
        if (c->flags & 8) { int32_t t = y0; y0 = y1; y1 = t; }
        VkImageBlit b = {{asp, s_level, s_layer, nl},
                         {{sr->xoffset, sr->yoffset, s_arr ? 0 : sr->zoffset}, {sr->xoffset + sr->width, sr->yoffset + sr->height, s_arr ? 1 : sr->zoffset + (int32_t)sd}},
                         {asp, d_level, d_layer, nl},
                         {{x0, y0, d_arr ? 0 : dr->zoffset}, {x1, y1, d_arr ? 1 : dr->zoffset + (int32_t)dd}}};
        vkCmdBlitImage(R.cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       (c->flags & 1) && s->linear_ok ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    } else {
        static int warned;
        if (!warned++) som_log("vkr: depth texture copy between different sizes or formats skipped");
        return;
    }
    barrier_all(R.cb);
}

/* ---------------------------------------------------------------- submit */
static void cmd_begin_fwd(void) { extern void vkr_cmd_begin_pub(void); vkr_cmd_begin_pub(); }
static void cmd_begin(void) {
    if (R.recording) return;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(R.cb, 0);
    VK_CHECK(vkBeginCommandBuffer(R.cb, &bi));
    R.recording = 1;
}

void vkr_cmd_begin_pub(void) { cmd_begin(); }

void vkr_submit_begin(void) {
    static int nb;
    if (!vkr_ready()) return;
    if (nb++ < 12) som_log("vkr: begin #%d", nb);
    pthread_mutex_lock(&submit_mu);
    if (nb == 1) R.depth_range[0] = 0.0f, R.depth_range[1] = 1.0f; /* NVN default; GPU state persists across submits */
    R.active = 1;
    ring_off = 0;
    R.draws = R.draws_skipped = R.clears = 0;
    R.in_pass = 0;
    R.n_color = 0;
    R.depth = NULL;
    R.scissor_set = 0;
    {
        int before = flags_cur;
        R.dbg = (flags_refresh() & 8) != 0;
        if (flags_cur != before) R.pgen++;
    }
    skip_refresh();
    memset(&PS, 0, sizeof PS);
    pf_begin = now_ns();
    /* the other frame context; its previous submit (two submits ago) must be done before its memory is reused */
    fr_cur = (fr_cur + 1) % NFRAME;
    if (FR[fr_cur].pending) {
        PF_T0();
        vkWaitForFences(g_vkr.dev, 1, &FR[fr_cur].fence, VK_TRUE, UINT64_MAX);
        PF_T1(PF_WAIT);
        vkResetFences(g_vkr.dev, 1, &FR[fr_cur].fence);
        FR[fr_cur].pending = 0;
        gp_collect();
    }
    {
        /* ADPF: the frame that just ended (from the previous submit's start to this one), its CPU part (without the
         * fence wait it began with) and the GPU time of the latest finished submit */
        static uint64_t prev_begin, prev_wait;
        if (prev_begin && perf_hint_active()) {
            int64_t total = (int64_t)(pf_begin - prev_begin);
            perf_hint_frame((int64_t)prev_begin, total, total - (int64_t)prev_wait, gp_last_gpu_ns);
        }
        prev_begin = pf_begin, prev_wait = PS.t[PF_WAIT];
    }
    R.cb = FR[fr_cur].cb, R.fence = FR[fr_cur].fence, R.dpool = FR[fr_cur].dpool;
    ring_buf = FR[fr_cur].ring_buf, ring_mem = FR[fr_cur].ring_mem, ring_ptr = FR[fr_cur].ring_ptr;
    vkResetDescriptorPool(g_vkr.dev, R.dpool, 0);
    submit_count++;
    img_reap();
    dset_stamp++;
    dd_reset();
    sup_ok = zero_ok = 0;
    cap_check();
    cmd_begin();
    gp_submit_begin();
}

/* ---- profiler report */
static void pf_line(char *o, size_t cap, const PfStat *s, double div) {
    snprintf(o, cap, "tex %.2f pipe %.2f shader %.2f desc %.2f copy %.2f | passes %.1f (cleared on load %.1f) new pipes %.2f (late draws %.1f) tex loads %.2f sets %.0f reused %.0f | MB ubo %.2f vb %.2f ib %.2f sb %.2f tex %.2f",
             (double)s->t[PF_TEX] / 1e6 / div, (double)s->t[PF_PIPE] / 1e6 / div, (double)s->t[PF_SHADER] / 1e6 / div,
             (double)s->t[PF_DESC] / 1e6 / div, (double)s->t[PF_COPY] / 1e6 / div, s->n_pass / div, s->n_lclear / div, s->n_pipe / div, s->n_late / div, s->n_tex / div,
             s->n_set / div, s->n_dedup / div, (double)s->b_ubo / 1048576.0 / div, (double)s->b_vb / 1048576.0 / div, (double)s->b_ib / 1048576.0 / div,
             (double)s->b_sb / 1048576.0 / div, (double)s->b_tex / 1048576.0 / div);
}
static void pf_end(uint64_t now, uint64_t nsub) {
    uint64_t total = now - pf_begin, replay = total - PS.t[PF_WAIT];
    uint64_t guest = PW.last_end && pf_begin > PW.last_end ? pf_begin - PW.last_end : 0;
    PW.last_end = now;
    if (!PW.start) PW.start = now;
    for (int k = 0; k < PF_N; k++) PW.s.t[k] += PS.t[k];
    PW.s.b_ubo += PS.b_ubo, PW.s.b_vb += PS.b_vb, PW.s.b_ib += PS.b_ib, PW.s.b_sb += PS.b_sb, PW.s.b_tex += PS.b_tex;
    PW.s.n_pass += PS.n_pass, PW.s.n_pipe += PS.n_pipe, PW.s.n_tex += PS.n_tex, PW.s.n_shader += PS.n_shader, PW.s.n_set += PS.n_set;
    PW.s.n_dedup += PS.n_dedup, PW.s.n_late += PS.n_late, PW.s.n_lclear += PS.n_lclear;
    PW.submits++, PW.draws += R.draws, PW.t_replay += replay, PW.t_guest += guest;
    if (total > PW.max_total) PW.max_total = total;
    char l[500];
    if (total > 40000000ull && PW.slow++ < 25) {
        pf_line(l, sizeof l, &PS, 1.0);
        som_log("vkr: slow submit #%llu: %.1f ms = replay %.1f + gpu wait %.1f (guest before it %.1f), draws %llu: %s", (unsigned long long)nsub,
                (double)total / 1e6, (double)replay / 1e6, (double)PS.t[PF_WAIT] / 1e6, (double)guest / 1e6, (unsigned long long)R.draws, l);
    }
    if (now - PW.start < 5000000000ull) return;
    double n = (double)PW.submits, secs = (double)(now - PW.start) / 1e9;
    uint64_t presents = __atomic_exchange_n(&PW.presents, 0, __ATOMIC_RELAXED), gap = __atomic_exchange_n(&PW.max_gap, 0, __ATOMIC_RELAXED);
    uint32_t o50 = __atomic_exchange_n(&PW.over50, 0, __ATOMIC_RELAXED), o100 = __atomic_exchange_n(&PW.over100, 0, __ATOMIC_RELAXED);
    pf_line(l, sizeof l, &PW.s, n);
    som_log("vkr: perf %.1f s: %.1f frames/s (longest %.0f ms, %u over 50 ms, %u over 100 ms), %.1f submits/s, %u slow | per submit ms: replay %.2f gpu wait %.2f guest %.2f (max total %.1f), draws %.0f: %s",
            secs, (double)presents / secs, (double)gap / 1e6, o50, o100, n / secs, PW.slow, (double)PW.t_replay / 1e6 / n,
            (double)PW.s.t[PF_WAIT] / 1e6 / n, (double)PW.t_guest / 1e6 / n, (double)PW.max_total / 1e6, (double)PW.draws / n, l);
    gp_report();
    tel_perf_window(secs, (double)presents, (double)gap / 1e6, o50, o100);
    uint64_t le = PW.last_end, lp = PW.last_present;
    memset(&PW, 0, sizeof PW);
    PW.last_end = le, PW.last_present = lp, PW.start = now;
}

volatile uint64_t g_vkr_last_submit_ns; /* stall detection (nvn_glue.c watchdog) */
/* the steps of vkr_submit_end, per submit (logged every 5 s): end_pass, vkEndCommandBuffer, waiting for the queue lock
 * (shared with the window thread's present), vkQueueSubmit */
static uint64_t se_t[4], se_n;

void vkr_submit_end(void) {
    if (!R.active) return;
    uint64_t se0 = now_ns();
    end_pass();
    se_t[0] += now_ns() - se0;
    static int ne;
    if (ne++ < 12) som_log("vkr: end #%d (submitting)", ne);
    cap_copy();
    /* debug: every 100th submit, read the presented image back and log its colour statistics */
    Img *pi = NULL;
    uint64_t pb_off = 0;
    uint8_t *pb = NULL;
    uint64_t pts = __atomic_load_n(&present_serial_now, __ATOMIC_ACQUIRE);
    if (pts && (ne == 4 || ne == 8 || ne == 12 || ne == 20)) {
        for (Img *k = img_tab[pts & 4095]; k; k = k->next)
            if (k->serial == pts) pi = k;
        if (pi && pi->inited && !pi->is_depth && pi->w * pi->h * 4ull < RING_BYTES / 2) {
            pb = ring_alloc((uint64_t)pi->w * pi->h * 4, 16, &pb_off);
            if (pb) {
                barrier_all(R.cb);
                VkBufferImageCopy rg = {pb_off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {pi->w, pi->h, 1}};
                vkCmdCopyImageToBuffer(R.cb, pi->image, VK_IMAGE_LAYOUT_GENERAL, ring_buf, 1, &rg);
            }
        }
    }
    gp_submit_end();
    uint64_t se1 = now_ns();
    VK_CHECK(vkEndCommandBuffer(R.cb));
    R.recording = 0;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &R.cb;
    uint64_t se2 = now_ns();
    pthread_mutex_lock(&g_vkr.qmu);
    uint64_t se3 = now_ns();
    VkResult r = vkQueueSubmit(g_vkr.queue, 1, &si, R.fence);
    pthread_mutex_unlock(&g_vkr.qmu);
    {
        uint64_t se4 = now_ns();
        se_t[1] += se2 - se1, se_t[2] += se3 - se2, se_t[3] += se4 - se3, se_n++;
        static uint64_t se_log;
        if (se4 - se_log >= 5000000000ull && se_n) {
            som_log("vkr: submit end per submit: end pass %.2f ms, end command buffer %.2f ms, queue lock wait %.2f ms, vkQueueSubmit %.2f ms",
                    se_t[0] / 1e6 / se_n, se_t[1] / 1e6 / se_n, se_t[2] / 1e6 / se_n, se_t[3] / 1e6 / se_n);
            memset(se_t, 0, sizeof se_t), se_n = 0, se_log = se4;
        }
    }
    if (ne <= 12) som_log("vkr: submitted -> %d", (int)r);
    /* Not waited for (see FR) unless something reads the results back right away: a frame capture, the start-up
     * readback, the debug flush; vkr_flags 131072 waits always (the old behaviour, to compare). */
    int sync = cap_left || nsnap || pb || R.dbg || (vkr_flags() & 131072);
    if (r == VK_SUCCESS && sync) {
        PF_T0();
        vkWaitForFences(g_vkr.dev, 1, &R.fence, VK_TRUE, UINT64_MAX);
        PF_T1(PF_WAIT);
        vkResetFences(g_vkr.dev, 1, &R.fence);
    } else if (r == VK_SUCCESS) {
        FR[fr_cur].pending = 1;
    } else {
        som_log("vkr: vkQueueSubmit failed %d", (int)r);
    }
    cap_write();
    if (pb && pi) {
        uint64_t sum[4] = {0}, nz = 0, n = (uint64_t)pi->w * pi->h;
        for (uint64_t i = 0; i < n; i++) {
            for (int c = 0; c < 4; c++) sum[c] += pb[i * 4 + (uint64_t)c];
            nz += (pb[i * 4] | pb[i * 4 + 1] | pb[i * 4 + 2]) != 0;
        }
        som_log("vkr: presented image %ux%u fmt %d: mean rgba %.1f %.1f %.1f %.1f, non-black pixels %llu of %llu", pi->w, pi->h, (int)pi->fmt,
                (double)sum[0] / (double)n, (double)sum[1] / (double)n, (double)sum[2] / (double)n, (double)sum[3] / (double)n,
                (unsigned long long)nz, (unsigned long long)n);
    }
    static uint64_t nsub, tot_draws, tot_skipped, tot_clears;
    tot_draws += R.draws, tot_skipped += R.draws_skipped, tot_clears += R.clears;
    static uint64_t last_status_ns;
    {
        uint64_t now = now_ns();
        pf_end(now, nsub + 1);
        pcache_tick(now);
        g_vkr_last_submit_ns = now;
        if (now - last_status_ns >= 5000000000ull) {
            last_status_ns = now;
            som_log("vkr: status submits %llu draws %llu; images live %llu (%llu MB) freed %llu; upload ring peak %.1f MB of %u, %llu refused",
                    (unsigned long long)nsub + 1, (unsigned long long)tot_draws, (unsigned long long)g_img_live, (unsigned long long)(g_img_bytes >> 20),
                    (unsigned long long)g_img_freed, ring_peak / 1048576.0, RING_BYTES >> 20, (unsigned long long)ring_full);
            ring_peak = ring_full = 0;
        }
    }
    if (++nsub <= 3 || nsub % 200 == 0)
        som_log("vkr: submit #%llu: draws %llu (skipped %llu), clears %llu; totals draws %llu skipped %llu clears %llu", (unsigned long long)nsub,
                (unsigned long long)R.draws, (unsigned long long)R.draws_skipped, (unsigned long long)R.clears, (unsigned long long)tot_draws,
                (unsigned long long)tot_skipped, (unsigned long long)tot_clears);
    R.active = 0;
    pthread_mutex_unlock(&submit_mu);
}

void vkr_op(const op_hdr *h, const void *a) {
    if (!R.active) return;
    if ((vkr_flags() & 8) && h->op != OP_DRAW && h->op != OP_DRAW_IDX && h->op != OP_BIND_TEX && h->op != OP_BIND_SAMP &&
        h->op != OP_BIND_STATE && h->op != OP_BIND_UBO && h->op != OP_BIND_VBUF && h->op != OP_BIND_VATTR &&
        h->op != OP_BIND_VSTREAM && h->op != OP_BIND_PROGRAM && h->op != OP_STENCIL && h->op != OP_POLY_OFFSET &&
        h->op != OP_VIEWPORTS && h->op != OP_SCISSOR && h->op != OP_REPORT_COUNTER) {
        char ol[64];
        snprintf(ol, sizeof ol, "op %d", (int)h->op);
        recent_note(ol);
    }
    switch (h->op) {
    case OP_SET_RT: {
        const p_set_rt *r = a;
        end_pass();
        R.pgen++;
        R.n_color = r->n > MAXI ? MAXI : r->n;
        for (int i = 0; i < R.n_color; i++) {
            R.color[i] = r->color[i] ? img_get(r->color[i]) : NULL;
            R.color_level[i] = r->has_cview[i] ? r->cview[i].base_level : 0;
            R.color_layer[i] = r->has_cview[i] ? r->cview[i].min_layer : 0;
        }
        R.depth = r->depth ? img_get(r->depth) : NULL;
        R.has_vp = 0;
        R.scissor_set = 0;
        break;
    }
    case OP_CLEAR_COLOR: do_clear_color(a); break;
    case OP_CLEAR_DS: do_clear_ds(a); break;
    case OP_VIEWPORTS: {
        const p_viewports *v = a;
        if (v->n > 0 && v->first == 0) {
            memcpy(R.vp, v->v[0], 16); /* SetViewports carries x, y, w, h only; depth comes from OP_DEPTH_RANGE */
            R.has_vp = 1;
        }
        break;
    }
    case OP_DEPTH_RANGE: {
        const p_depth_range *d = a;
        R.depth_range[0] = d->n, R.depth_range[1] = d->f;
        break;
    }
    case OP_SCISSOR: {
        const p_scissor *s = a;
        R.sc[0] = s->x, R.sc[1] = s->y, R.sc[2] = s->w, R.sc[3] = s->h;
        R.scissor_set = 1;
        break;
    }
    case OP_UPDATE_UB: dd_written(((const p_update_ub *)a)->dst, ((const p_update_ub *)a)->size); break;
    case OP_COPY_B2B: dd_written(((const p_copy_b2b *)a)->dst, ((const p_copy_b2b *)a)->size); break;
    case OP_CLEAR_BUF: dd_written(((const p_clear_buf *)a)->addr, ((const p_clear_buf *)a)->size); break;
    case OP_BIND_STATE: {
        const p_bind_state *s = a;
        R.pgen++;
        switch (s->kind) {
        case 0: { blend_st b; memcpy(&b, s->bytes, sizeof b); if (b.target < 8) R.blend[b.target] = b; break; }
        case 1: memcpy(&R.cstate, s->bytes, sizeof R.cstate); break;
        case 2: memcpy(&R.chmask, s->bytes, sizeof R.chmask); break;
        case 3: memcpy(&R.ms, s->bytes, sizeof R.ms); break;
        case 4: memcpy(&R.poly, s->bytes, sizeof R.poly); break;
        case 5: memcpy(&R.ds, s->bytes, sizeof R.ds); break;
        default: break;
        }
        break;
    }
    case OP_BLEND_COLOR: memcpy(R.blend_const, ((const p_blend_color *)a)->rgba, 16); break;
    case OP_BIND_VATTR: {
        const p_bind_vattr *v = a;
        R.pgen++;
        for (int i = 0; i < 16; i++) R.vattr[(v->first + i) & 15] = i < v->n ? v->st[i] : 0;
        break;
    }
    case OP_BIND_VSTREAM: {
        const p_bind_vstream *v = a;
        R.pgen++;
        for (int i = 0; i < v->n && v->first + i < 16; i++) R.vstream[v->first + i] = v->st[i];
        break;
    }
    case OP_BIND_VBUF: {
        const p_bind_vbuf *v = a;
        for (int i = 0; i < v->n && v->first + i < 16; i++) R.vbuf[v->first + i] = v->r[i];
        break;
    }
    case OP_BIND_UBO: {
        const p_bind_buf *b = a;
        if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) R.ubo[b->stage][b->index] = *b;
        break;
    }
    case OP_BIND_SSBO: {
        const p_bind_buf *b = a;
        if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) R.ssbo[b->stage][b->index] = *b;
        break;
    }
    case OP_BIND_TEX: case OP_BIND_SAMP: {
        const p_bind_handles *b = a;
        if (b->stage < 0 || b->stage >= 6) break;
        for (int i = 0; i < b->n && b->first + i < 32; i++) (h->op == OP_BIND_TEX ? R.texh : R.samph)[b->stage][b->first + i] = b->h[i];
        break;
    }
    case OP_STENCIL: {
        const p_stencil *s = a;
        for (int f = 0; f < 2; f++) {
            if (!(s->face == NVN_FACE_FRONT_AND_BACK || s->face == (f ? NVN_FACE_BACK : NVN_FACE_FRONT))) continue;
            if (s->which == 0) R.sten_vmask[f] = (uint32_t)s->value_mask;
            else if (s->which == 1) R.sten_wmask[f] = (uint32_t)s->mask;
            else R.sten_ref[f] = (uint32_t)s->ref;
        }
        break;
    }
    case OP_POLY_OFFSET: {
        const p_poly_offset *p = a;
        R.poly_off[0] = p->factor, R.poly_off[1] = p->units, R.poly_off[2] = p->clamp;
        break;
    }
    case OP_COPY_STRIDE: R.copy_stride = (uint64_t)((const p_copy_stride *)a)->stride; break;
    case OP_COPY_B2T: do_copy_b2t(a); break;
    case OP_COPY_T2T: do_copy_t2t(a); break;
    case OP_DRAW: {
        const p_draw *d = a;
        do_draw(d->prim, 0, d->count, d->first, 0, d->base_instance, d->instances, 0, 0);
        break;
    }
    case OP_DRAW_IDX: {
        const p_draw_idx *d = a;
        do_draw(d->prim, 1, d->count, 0, d->base_vertex, d->base_instance, d->instances, d->ibuf, d->itype);
        break;
    }
    default: break;
    }
}

/* ---------------------------------------------------------------- present */

/* times of the last presents, for vkr_frame_stats */
#define PT_N 256
static uint64_t pt_ring[PT_N];
static unsigned pt_n;
static pthread_mutex_t pt_mu = PTHREAD_MUTEX_INITIALIZER;

void vkr_frame_stats(VkrFrameStats *o) {
    memset(o, 0, sizeof *o);
    uint64_t now = now_ns();
    pthread_mutex_lock(&pt_mu);
    unsigned n = pt_n < PT_N ? pt_n : PT_N;
    uint64_t newest = n ? pt_ring[(pt_n - 1) % PT_N] : 0, oldest = newest, maxgap = 0;
    unsigned frames = 0;
    for (unsigned k = 1; k < n; k++) { /* gaps between consecutive presents that ended within the last second */
        uint64_t b = pt_ring[(pt_n - k) % PT_N], a = pt_ring[(pt_n - k - 1) % PT_N];
        if (now - b > 1000000000ull) break;
        if (b - a > maxgap) maxgap = b - a;
        oldest = a;
        frames++;
    }
    pthread_mutex_unlock(&pt_mu);
    /* a present still awaited counts as a frame time too: a freeze shows while it lasts */
    uint64_t pending = newest && now > newest ? now - newest : 0;
    if (pending > maxgap && pending > 100000000ull) maxgap = pending;
    o->frames = frames;
    if (frames && newest > oldest) {
        o->avg_ms = (float)((double)(newest - oldest) / 1e6 / frames);
        o->fps = (float)(frames * 1e9 / (double)(newest - oldest + (pending > 100000000ull ? pending : 0)));
    }
    o->max_ms = (float)((double)maxgap / 1e6);
}

int vkr_frame_times(float *out, int max) {
    pthread_mutex_lock(&pt_mu);
    int have = (int)(pt_n < PT_N ? pt_n : PT_N) - 1;
    int cnt = have < max ? have : max;
    if (cnt < 0) cnt = 0;
    for (int k = 0; k < cnt; k++) { /* k = 0 is the newest gap */
        uint64_t b = pt_ring[(pt_n - 1 - k) % PT_N], a = pt_ring[(pt_n - 2 - k) % PT_N];
        out[cnt - 1 - k] = (float)((double)(b - a) / 1e6);
    }
    pthread_mutex_unlock(&pt_mu);
    return cnt;
}

static int present_crop[4]; /* written before present_serial_now (release), read after it (acquire) */

void vkr_present(const tex_t *t, const int crop[4]) {
    for (int k = 0; k < 4; k++) __atomic_store_n(&present_crop[k], crop ? crop[k] : 0, __ATOMIC_RELAXED);
    pthread_mutex_lock(&img_mu);
    for (Img *i = img_tab[t->serial & 4095]; i; i = i->next)
        if (i->serial == t->serial) i->presented = 1;
    pthread_mutex_unlock(&img_mu);
    __atomic_store_n(&present_serial_now, t->serial, __ATOMIC_RELEASE);
    __atomic_store_n(&present_tex, t, __ATOMIC_RELEASE);
    present_serial++;
    pthread_mutex_lock(&pt_mu);
    pt_ring[pt_n++ % PT_N] = now_ns();
    pthread_mutex_unlock(&pt_mu);
    {   /* frame pacing as the player sees it: the time between presents */
        uint64_t now = now_ns(), last = PW.last_present, gap = last ? now - last : 0;
        PW.last_present = now;
        __atomic_fetch_add(&PW.presents, 1, __ATOMIC_RELAXED);
        if (gap > __atomic_load_n(&PW.max_gap, __ATOMIC_RELAXED)) __atomic_store_n(&PW.max_gap, gap, __ATOMIC_RELAXED);
        {
            /* a frame well over the target period: log what the guest was doing. The threshold is 1.5 periods (25 ms at
             * 60 fps, 50 ms at 30) and at most 4 reports a second: on a slow device (the Galaxy A17) a fixed 24 ms flagged
             * nearly every frame, and formatting/writing 3-5 log lines per frame cost ~4% of the replay thread. */
            extern void som_stall_mark(const char *label);
            extern int som_target_fps(void);
            int fps = som_target_fps();
            uint64_t limit = fps > 0 ? 1500000000ull / (uint64_t)fps : 24000000ull;
            static uint64_t win_start;
            static unsigned win_n;
            if (now - win_start > 1000000000ull) win_start = now, win_n = 0;
            if (gap > limit && win_n < 4) {
                char lb[64];
                snprintf(lb, sizeof lb, "long frame %.0f ms", (double)gap / 1e6);
                win_n++;
                som_stall_mark(lb);
            } else {
                som_stall_mark(NULL);
            }
        }
        if (gap > 50000000ull) __atomic_fetch_add(&PW.over50, 1, __ATOMIC_RELAXED);
        if (gap > 100000000ull) __atomic_fetch_add(&PW.over100, 1, __ATOMIC_RELAXED);
    }
}

/* the part of the screen the picture was last shown in, as fractions (x, y, w, h): the whole screen unless letterboxed */
static volatile float present_rect[4] = {0, 0, 1, 1}; /* written by the window thread, read by the event loop: a torn rectangle only misplaces one touch */
void vkr_present_rect(float r[4]) {
    for (int k = 0; k < 4; k++) r[k] = present_rect[k];
}

int vkr_blit_present(VkCommandBuffer cb, VkImage dst, VkExtent2D ext) {
    uint64_t ps = __atomic_load_n(&present_serial_now, __ATOMIC_ACQUIRE);
    if (!ps || !g_vkr.ready || (vkr_flags() & 4)) return 0;
    pthread_mutex_lock(&img_mu);
    Img *i = NULL;
    for (Img *k = img_tab[ps & 4095]; k; k = k->next)
        if (k->serial == ps) i = k;
    pthread_mutex_unlock(&img_mu);
    if (!i || !i->inited || i->is_depth) return 0;
    /* the window's crop: the handheld profile's 1280x720 picture sits in the top-left of a 1920x1080 texture. Applied on
     * the handheld profile only; docked (the Adreno devices) the whole texture is shown exactly as before, and a docked
     * crop smaller than the texture is only logged. */
    extern int som_handheld_profile(void);
    int32_t x0 = 0, y0 = 0, x1 = (int32_t)i->w, y1 = (int32_t)i->h;
    int cx = __atomic_load_n(&present_crop[0], __ATOMIC_RELAXED), cy = __atomic_load_n(&present_crop[1], __ATOMIC_RELAXED);
    int cw = __atomic_load_n(&present_crop[2], __ATOMIC_RELAXED), ch = __atomic_load_n(&present_crop[3], __ATOMIC_RELAXED);
    int valid = cw > 0 && ch > 0 && cx >= 0 && cy >= 0 && (uint32_t)(cx + cw) <= i->w && (uint32_t)(cy + ch) <= i->h;
    int partial = valid && (cx || cy || (uint32_t)cw != i->w || (uint32_t)ch != i->h);
    if (partial && som_handheld_profile()) {
        x0 = cx, y0 = cy, x1 = cx + cw, y1 = cy + ch;
    } else if (partial) {
        static int logged;
        if (!logged++) som_log("vkr: docked window crop %d,%d %dx%d of %ux%u (not applied: whole image shown)", cx, cy, cw, ch, i->w, i->h);
    }
    {
        static int last[4];
        if (last[0] != cx || last[1] != cy || last[2] != cw || last[3] != ch) {
            last[0] = cx, last[1] = cy, last[2] = cw, last[3] = ch;
            som_log("vkr: window crop %d,%d %dx%d of %ux%u -> shown %dx%d", cx, cy, cw, ch, i->w, i->h, x1 - x0, y1 - y0);
        }
    }
    /* where on the screen: all of it (Stretch to fill), or the largest centred rectangle of the picture's own shape
     * (Aspect ratio 16:9; the rest stays the clear colour, black: main.c) */
    extern int som_keep_aspect(void);
    int32_t dx0 = 0, dy0 = 0, dx1 = (int32_t)ext.width, dy1 = (int32_t)ext.height;
    if (som_keep_aspect()) {
        uint64_t pw = (uint64_t)(x1 - x0), ph = (uint64_t)(y1 - y0);
        if (pw * ext.height > ph * ext.width) { /* wider than the screen: bars above and below */
            int32_t h = (int32_t)(ext.width * ph / pw);
            dy0 = ((int32_t)ext.height - h) / 2, dy1 = dy0 + h;
        } else { /* narrower: bars left and right */
            int32_t w = (int32_t)(ext.height * pw / ph);
            dx0 = ((int32_t)ext.width - w) / 2, dx1 = dx0 + w;
        }
    }
    present_rect[0] = (float)dx0 / (float)ext.width;
    present_rect[1] = (float)dy0 / (float)ext.height;
    present_rect[2] = (float)(dx1 - dx0) / (float)ext.width;
    present_rect[3] = (float)(dy1 - dy0) / (float)ext.height;
    VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{x0, y0, 0}, {x1, y1, 1}},
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx0, dy0, 0}, {dx1, dy1, 1}}};
    /* nearest at 1:1 (the Adreno handhelds' 1920x1080 panels: unchanged); linear when the picture is scaled */
    int scaled = x1 - x0 != dx1 - dx0 || y1 - y0 != dy1 - dy0;
    VkFilter filt = scaled && i->linear_ok ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkCmdBlitImage(cb, i->image, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, filt);
    return 1;
}
