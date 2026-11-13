// The null graphics back end: no GPU. Every submit completes at
// once and advances the fence value, pools always initialise, presents are only counted, texel reads return zeros.
// A submit performs the CPU-visible side effects of its op streams in order: event signals, counter reports (timestamp
// ticks, counter 0), buffer-to-buffer copies and uniform updates.
// It is the CPU-side gate for the NVN front end. Also the back-end choice (gfx_backend) and the statistics (gfx_stats).
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "gfx.h"
#include "vkr.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_tex.h"

GfxStats gfx_counters;

GfxStats gfx_stats(void) {
    GfxStats s;
    s.frames = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    s.nvn_calls = __atomic_load_n(&gfx_counters.nvn_calls, __ATOMIC_RELAXED);
    s.nvn_distinct = __atomic_load_n(&gfx_counters.nvn_distinct, __ATOMIC_RELAXED);
    s.unimplemented = __atomic_load_n(&gfx_counters.unimplemented, __ATOMIC_RELAXED);
    s.validation_errors = __atomic_load_n(&gfx_counters.validation_errors, __ATOMIC_RELAXED);
    s.shader_failures = __atomic_load_n(&gfx_counters.shader_failures, __ATOMIC_RELAXED);
    s.draws_skipped = __atomic_load_n(&gfx_counters.draws_skipped, __ATOMIC_RELAXED);
    return s;
}

// TOTK_GPU=null|vulkan, default vulkan (rt_config); chosen once.
const GfxBackend *gfx_backend(void) {
    static const GfxBackend *chosen;
    const GfxBackend *b = __atomic_load_n(&chosen, __ATOMIC_ACQUIRE);
    if (b) return b;
    const char *gpu = rt_config()->gpu;
    (void)gpu;
    b = &gfx_null_backend; /* the Vulkan back end replaces this once it is ported */
    __atomic_store_n(&chosen, b, __ATOMIC_RELEASE);
    return b;
}

static uint64_t fence;

static int null_pool_init(pool_t *p) { (void)p; return 1; }
static void null_pool_fini(pool_t *p) { (void)p; }
static int null_pool_map_virtual(pool_t *v, int n, const void *requests) { (void)v, (void)n, (void)requests; return 1; }
static int tex_type_of(int target, int samples);
static uint8_t tex_types[1 << 20];
static void null_texture_register(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || id >= (1 << 20) || !t) return;
    int target = v && v->set_target ? v->target : t->d.target;
    tex_types[id] = (uint8_t)tex_type_of(target, t->d.samples);
    vkr_texture_register(id, t, v);
}
static void null_sampler_register(int id, const samp_par *s) { vkr_sampler_register(id, s); }
static void null_texture_release(const tex_t *t) { vkr_texture_release(t); }
static void null_program_release(const prog_t *p) { (void)p; }

// The CPU-visible side effects of one op stream, in stream order (MNG nvn_mtl.m mtl_exec and the commit handler:
// 10618-10642, 1677-1678, 1960-1985). Everything else (draws, dispatches, clears, texture copies) has no effect here.
// Addresses of CPU-invisible pools (the GPU window) hold no host memory, so those writes are dropped.
static void *null_cpu_addr(uint64_t addr, uint64_t size, const char *what) {
    if (nvn_gpu_only(addr)) return NULL;
    if (!nvn_pool_find(addr, size ? size : 1)) rt_fatal("gfx: null back end: %s at %#llx (%llu bytes) is in no memory pool", what,
                                                        (unsigned long long)addr, (unsigned long long)size);
    return (void *)(uintptr_t)addr;
}

// NVNcounterData = {counter, timestamp}. Timestamps are Tegra 19.2 MHz ticks, the base of GetSystemTick; every counter
// type the null GPU cannot measure (all but the timestamps, samples passed included) reports 0.
static void null_report(uint64_t addr) {
    NVNcounterData *d = null_cpu_addr(addr, sizeof *d, "counter report");
    if (!d) return;
    uint64_t rec[2] = {0, (uint64_t)((unsigned __int128)kern_now_ns() * 12 / 625)};
    memcpy(d, rec, sizeof rec);
}


/* ---- draw-state tracker and shader-input recorder ----
 * Follows the state the translator's "answers" depend on (vertex attribute types, colour target types, bound texture
 * types) and writes each new (shader, answers) pair to <data>/shaders/<hash>_<ahash>.ans for offline translation.
 * .ans: u32 nvn stage, tex_type[32], img_type[32], img_format[32], attr_type[32], frag_out_type[8], shared u32. */
extern uint64_t som_shader_hash(const uint8_t *ctl, const uint8_t *code, uint32_t len);
extern const char *som_data_dir(void);

enum { T_FLOAT, T_SINT, T_UINT };
static int fmt_type(int f, int vertex) {
    static const uint8_t ui[] = {0x03, 0x08, 0x0b, 0x0f, 0x14, 0x17, 0x1b, 0x20, 0x23, 0x27, 0x2c, 0x2f, 0x3e, 0x6f, 0x74, 0x77};
    static const uint8_t si[] = {0x04, 0x09, 0x0c, 0x10, 0x15, 0x18, 0x1c, 0x21, 0x24, 0x28, 0x2d, 0x30, 0x70, 0x75, 0x78};
    static const uint8_t vui[] = {0x55, 0x5b, 0x61, 0x67}, vsi[] = {0x56, 0x5c, 0x62, 0x68, 0x6a};
    for (unsigned i = 0; i < sizeof ui; i++) if (ui[i] == f) return T_UINT;
    for (unsigned i = 0; i < sizeof si; i++) if (si[i] == f) return T_SINT;
    if (vertex) {
        for (unsigned i = 0; i < sizeof vui; i++) if (vui[i] == f) return T_UINT;
        for (unsigned i = 0; i < sizeof vsi; i++) if (vsi[i] == f) return T_SINT;
    }
    return T_FLOAT;
}

static int tex_type_of(int target, int samples) {
    int ms = samples > 1;
    switch (target) {
    case NVN_TEXTURE_TARGET_1D: return 1;
    case NVN_TEXTURE_TARGET_1D_ARRAY: return 2;
    case NVN_TEXTURE_TARGET_BUFFER: return 3;
    case NVN_TEXTURE_TARGET_2D_ARRAY: return ms ? 7 : 5;
    case NVN_TEXTURE_TARGET_2D_MULTISAMPLE: return 6;
    case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: return 7;
    case NVN_TEXTURE_TARGET_3D: return 8;
    case NVN_TEXTURE_TARGET_CUBEMAP: return 9;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: return 10;
    default: return ms ? 6 : 4;
    }
}

static uint8_t tex_types[1 << 20]; /* by texture pool id */

enum { ST_VS = 0, ST_FS = 1, ST_CS = 5, ST_N = 6 };
static struct {
    struct { uint64_t hash; const uint8_t *ctl, *code; uint32_t len; } prog[ST_N];
    uint8_t attr[16];
    uint8_t color_type[8];
    uint8_t tex[ST_N][32], img[ST_N][32];
} ts;

/* Bumped whenever something a stage's shader key depends on changes; the renderer resolves a stage's shader again
 * only then (vkr.c do_draw_inner). */
static uint32_t ts_gen[ST_N];
uint32_t ts_stage_gen(int stage) { return stage >= 0 && stage < ST_N ? ts_gen[stage] : 0; }

static pthread_mutex_t rec_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t rec_seen[32768];
static int rec_n;

/* the stage's .ans record for the current draw state; returns the answers hash */
static uint64_t fill_answers(int st, uint8_t buf[TS_ANS_SIZE]) {
    memset(buf, 0, TS_ANS_SIZE);
    uint32_t stage = (uint32_t)st, shared = 0;
    memcpy(buf, &stage, 4);
    uint8_t *tt = buf + 4, *it = tt + 32, *imf = it + 32, *at = imf + 32, *fo = at + 32;
    memcpy(tt, ts.tex[st], 32);
    memcpy(it, ts.img[st], 32);
    if (st == ST_VS) memcpy(at, ts.attr, 16);
    if (st == ST_FS) memcpy(fo, ts.color_type, 8);
    memcpy(fo + 8, &shared, 4);
    uint64_t h = ts.prog[st].hash;
    for (unsigned i = 0; i < TS_ANS_SIZE; i++) h = (h ^ buf[i]) * 1099511628211ull;
    return h;
}

int ts_stage_answers(int stage, uint8_t buf[TS_ANS_SIZE], const char *key) {
    if (stage < 0 || stage >= ST_N || !ts.prog[stage].hash) return 0;
    uint64_t h = fill_answers(stage, buf);
    char k[40];
    snprintf(k, sizeof k, "%016llx_%016llx", (unsigned long long)ts.prog[stage].hash, (unsigned long long)h);
    return !strcmp(k, key);
}

static int record_stage(int st, char *keyout) {
    if (!ts.prog[st].hash) return 0;
    if (!keyout && !som_shader_capture_on()) return 1;
    uint8_t buf[TS_ANS_SIZE];
    uint64_t h = fill_answers(st, buf);
    if (keyout) snprintf(keyout, 40, "%016llx_%016llx", (unsigned long long)ts.prog[st].hash, (unsigned long long)h);
    if (!som_shader_capture_on()) return 1; /* .ans files only for the PC tools (sync_shaders.sh) */
    pthread_mutex_lock(&rec_mu);
    for (int i = 0; i < rec_n; i++)
        if (rec_seen[i] == h) { pthread_mutex_unlock(&rec_mu); return 1; }
    if (rec_n < 32768) rec_seen[rec_n++] = h;
    int total = rec_n;
    pthread_mutex_unlock(&rec_mu);
    char path[600];
    snprintf(path, sizeof path, "%s/shaders/%016llx_%016llx.ans", som_data_dir(), (unsigned long long)ts.prog[st].hash,
             (unsigned long long)h);
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    fwrite(buf, 1, sizeof buf, f);
    fclose(f);
    if (total <= 5 || total % 50 == 0) som_log("shader answers #%d: stage %d -> %s", total, st, path);
    return 1;
}

int ts_shader_key(int stage, char key[40], const uint8_t **ctl, const uint8_t **code, uint32_t *len) {
    if (stage < 0 || stage >= ST_N || !record_stage(stage, key)) return 0;
    *ctl = ts.prog[stage].ctl, *code = ts.prog[stage].code, *len = ts.prog[stage].len;
    return 1;
}

/* 1 when the draw state asks nothing special of the stage's translation: float vertex attributes (VS), float colour
 * outputs (FS), no storage images. Such a stage uses the ahead-of-time translation named by its code hash alone
 * (tools/aot_shaders.py); anything else needs the variant translated for the exact state. */
int ts_stage_plain(int stage) {
    if (stage < 0 || stage >= ST_N) return 0;
    for (int i = 0; i < 32; i++) if (ts.img[stage][i]) return 0;
    if (stage == ST_VS) for (int i = 0; i < 16; i++) if (ts.attr[i] != T_FLOAT) return 0;
    if (stage == ST_FS) for (int i = 0; i < 8; i++) if (ts.color_type[i] != T_FLOAT) return 0;
    return 1;
}

/* translator texture type (tex_type_of) of the texture bound to a stage's slot; nothing bound counts as 2D */
int ts_tex_type(int stage, int slot) {
    if (stage < 0 || stage >= ST_N || slot < 0 || slot >= 32 || !ts.tex[stage][slot]) return 4;
    return ts.tex[stage][slot];
}

static void ts_bind_program(const p_bind_program *b) {
    static int nb;
    if (nb++ < 6) {
        som_log("bind program %p stages %#x nstage %u ctl714 %u", (const void *)b->prog, b->stages, b->prog ? b->prog->nstage : 0,
                b->prog && b->prog->nstage && b->prog->sd[0].control ? *(const uint32_t *)((const uint8_t *)b->prog->sd[0].control + 0x714) : 99u);
    }
    /* The hash of a program's stage reads all of its code: remembered per program serial (a program gets a new
     * serial whenever its shaders are set), since the game binds programs dozens of times a frame. */
    static struct { uint64_t serial, hash[6]; uint8_t have; } hc[4096];
    for (int bit = 0; bit < 6; bit++) {
        if (!(b->stages >> bit & 1)) continue;
        int st = bit;
        const prog_t *p = b->prog;
        uint64_t old = ts.prog[st].hash;
        const uint8_t *old_code = ts.prog[st].code;
        ts.prog[st].hash = 0;
        for (uint32_t i = 0; p && i < p->nstage && i < 6; i++) {
            const uint8_t *c = p->sd[i].control;
            if (!c || !p->sd[i].data) continue;
            uint32_t sst, len;
            memcpy(&sst, c + 0x714, 4), memcpy(&len, c + 0x704, 4);
            if ((int)sst != st || !len || !nvn_pool_find(p->sd[i].data, len)) continue;
            ts.prog[st].ctl = c;
            ts.prog[st].code = (const uint8_t *)(uintptr_t)p->sd[i].data;
            ts.prog[st].len = len;
            __typeof__(hc[0]) *e = &hc[p->serial & 4095];
            if (!p->serial || e->serial != p->serial) e->serial = p->serial, e->have = 0;
            if (!(e->have >> i & 1) || !p->serial) e->hash[i] = som_shader_hash(c, ts.prog[st].code, len), e->have |= (uint8_t)(1u << i);
            ts.prog[st].hash = e->hash[i];
        }
        if (ts.prog[st].hash != old || ts.prog[st].code != old_code) ts_gen[st]++;
    }
}

static void ts_bind_handles(const p_bind_handles *b, int image) {
    if (b->stage < 0 || b->stage >= ST_N) return;
    for (int i = 0; i < b->n && b->first + i < 32; i++) {
        uint8_t t = b->h[i] ? tex_types[b->h[i] & 0xfffff] : 0;
        uint8_t *slot = &(image ? ts.img : ts.tex)[b->stage][b->first + i];
        if (*slot != t) *slot = t, ts_gen[b->stage]++;
    }
}

static void ts_draw(void) {
    static int nd;
    if (nd++ < 3) som_log("draw #%d: vs hash %llx fs hash %llx", nd, (unsigned long long)ts.prog[ST_VS].hash, (unsigned long long)ts.prog[ST_FS].hash);
    record_stage(ST_VS, NULL);
    record_stage(ST_FS, NULL);
}

static uint64_t op_hist[64];
/* replay time per op type, and in vkr_submit_begin / vkr_submit_end (logged every 5 s with the buffer copies) */
static uint64_t op_ns[64], op_cnt[64], sub_begin_ns, sub_end_ns;
static inline uint64_t mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Skeleton "display": the last colour the game cleared each texture to, shown by main.c for the presented texture. */
#define CLR_SLOTS 256
static struct { uint64_t serial; float rgba[4]; } clr_tab[CLR_SLOTS];
static uint64_t cur_rt0;
float som_gfx_color[4];
uint64_t som_gfx_presents;

static void clr_set(uint64_t serial, const float *rgba) {
    int i = (int)(serial % CLR_SLOTS);
    clr_tab[i].serial = serial;
    memcpy(clr_tab[i].rgba, rgba, 16);
}

/* the shader-type tracker's part of an op (the shader keys depend on it) */
static void ts_track(const op_hdr *h, const void *a) {
    switch (h->op) {
    case OP_BIND_PROGRAM: ts_bind_program(a); break;
    case OP_BIND_VATTR: {
        const p_bind_vattr *v = a;
        for (int i = 0; i < 16; i++) {
            uint32_t w = i < v->n ? v->st[i] : 0;
            uint8_t ty = (uint8_t)(w ? fmt_type((int)(w & 0xff), 1) : 0);
            if (ts.attr[(v->first + i) & 15] != ty) ts.attr[(v->first + i) & 15] = ty, ts_gen[ST_VS]++;
        }
        break;
    }
    case OP_BIND_TEX: ts_bind_handles(a, 0); break;
    case OP_BIND_IMG: ts_bind_handles(a, 1); break;
    case OP_SET_RT: {
        const p_set_rt *r = a;
        for (int i = 0; i < 8; i++) {
            uint8_t ty = (uint8_t)(i < r->n && r->color[i] ? fmt_type(r->color[i]->d.format, 0) : 0);
            if (ts.color_type[i] != ty) ts.color_type[i] = ty, ts_gen[ST_FS]++;
        }
        break;
    }
    default: break;
    }
}

/* where the replay is (for the look-ahead) */
static const uint64_t *rp_handles;
static int rp_n, rp_i;
static const op_hdr *rp_op;

/* Look-ahead (vkr.c pipeline_for, in-frame compile mode): the rest of the submit after the current op is run through
 * the renderer in scan mode, which only follows the state and queues the pipelines the draws will need. The tracker
 * is put back afterwards, so the real replay goes on exactly as before. */
void vkr_scan_begin(void);
void vkr_scan_end(void);
void gfx_scan_ahead(void) {
    if (!rp_op || !rp_handles) return;
    __typeof__(ts) ts_saved = ts;
    uint32_t gen_saved[ST_N];
    memcpy(gen_saved, ts_gen, sizeof ts_gen);
    vkr_scan_begin();
    const op_hdr *h = rp_op;
    int i = rp_i;
    for (;;) {
        if (h->op == OP_END) {
            if (++i >= rp_n) break;
            h = nvn_op_first(rp_handles[i]);
        } else {
            h = nvn_op_next(h);
        }
        if (h->op == OP_END) continue;
        const void *a = nvn_op_payload(h);
        ts_track(h, a);
        vkr_op(h, a);
    }
    vkr_scan_end();
    ts = ts_saved;
    memcpy(ts_gen, gen_saved, sizeof ts_gen);
}

static void null_replay(uint64_t handle) {
    for (const op_hdr *h = nvn_op_first(handle);; h = nvn_op_next(h)) {
        const void *a = nvn_op_payload(h);
        rp_op = h;
        if (h->op < 64) __atomic_fetch_add(&op_hist[h->op], 1, __ATOMIC_RELAXED);
        uint64_t t0 = mono_now();
        vkr_op(h, a);
        if (h->op < 64) op_ns[h->op] += mono_now() - t0, op_cnt[h->op]++;
        switch (h->op) {
        case OP_END: return;
        case OP_SIGNAL_EVENT: {
            const p_signal_event *e = a;
            if (e->mode == NVN_EVENT_SIGNAL_MODE_ADD) __atomic_fetch_add(e->val, e->value, __ATOMIC_RELEASE);
            else __atomic_store_n(e->val, e->value, __ATOMIC_RELEASE);
            break;
        }
        case OP_BIND_PROGRAM: ts_bind_program(a); break;
        case OP_BIND_VATTR: {
            const p_bind_vattr *v = a;
            for (int i = 0; i < 16; i++) {
                uint32_t w = i < v->n ? v->st[i] : 0;
                uint8_t ty = (uint8_t)(w ? fmt_type((int)(w & 0xff), 1) : 0);
                if (ts.attr[(v->first + i) & 15] != ty) ts.attr[(v->first + i) & 15] = ty, ts_gen[ST_VS]++;
            }
            break;
        }
        case OP_BIND_TEX: ts_bind_handles(a, 0); break;
        case OP_BIND_IMG: ts_bind_handles(a, 1); break;
        case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND: ts_draw(); break;
        case OP_SET_RT: {
            const p_set_rt *r = a;
            for (int i = 0; i < 8; i++) {
                uint8_t ty = (uint8_t)(i < r->n && r->color[i] ? fmt_type(r->color[i]->d.format, 0) : 0);
                if (ts.color_type[i] != ty) ts.color_type[i] = ty, ts_gen[ST_FS]++;
            }
            cur_rt0 = r->n > 0 && r->color[0] ? r->color[0]->serial : 0;
            break;
        }
        case OP_CLEAR_COLOR: {
            const p_clear_color *cc = a;
            if (cc->index == 0 && cur_rt0) clr_set(cur_rt0, cc->rgba);
            break;
        }
        case OP_REPORT_COUNTER: null_report(((const p_report_counter *)a)->addr); break;
        case OP_COPY_B2B: {
            const p_copy_b2b *b = a;
            void *s = null_cpu_addr(b->src, b->size, "buffer copy source"), *d = null_cpu_addr(b->dst, b->size, "buffer copy target");
            if (s && d) memmove(d, s, b->size);
            {   /* how much the CPU copies for the game's buffer copies (logged every 5 s with the census) */
                extern uint64_t g_b2b_bytes, g_b2b_n, g_b2b_max;
                g_b2b_bytes += b->size, g_b2b_n++;
                if (b->size > g_b2b_max) g_b2b_max = b->size;
            }
            break;
        }
        case OP_CLEAR_BUF: {
            const p_clear_buf *f = a;
            uint32_t *d = null_cpu_addr(f->addr, f->size, "buffer fill");
            for (uint64_t i = 0; d && i < f->size / 4; i++) d[i] = f->value;
            break;
        }
        case OP_UPDATE_UB: {
            const p_update_ub *u = a;
            void *d = null_cpu_addr(u->dst, u->size, "uniform update");
            if (d) memcpy(d, u->data, u->size);
            break;
        }
        default: break;
        }
    }
}

uint64_t g_b2b_bytes, g_b2b_n, g_b2b_max;

static uint64_t null_submit(const uint64_t *handles, int n) {
    uint64_t tb = mono_now();
    vkr_submit_begin();
    sub_begin_ns += mono_now() - tb;
    rp_handles = handles, rp_n = n;
    for (int i = 0; i < n; i++) rp_i = i, null_replay(handles[i]);
    rp_handles = NULL, rp_op = NULL;
    uint64_t te = mono_now();
    vkr_submit_end();
    sub_end_ns += mono_now() - te;
    {
        static uint64_t t0, subs;
        uint64_t now = mono_now();
        subs++;
        if (!t0) t0 = now;
        if (now - t0 >= 5000000000ull) {
            som_log("gfx: buffer copies (CPU) %.1f MB/s in %.0f copies/s, largest %llu KB", (double)g_b2b_bytes / 1e6 / ((now - t0) / 1e9),
                    (double)g_b2b_n / ((now - t0) / 1e9), (unsigned long long)(g_b2b_max >> 10));
            /* where replay time goes, per submit: submit begin/end and the six most expensive op types */
            char line[600];
            int o = snprintf(line, sizeof line, "gfx: replay per submit (%llu submits): begin %.2f ms, end %.2f ms;", (unsigned long long)subs,
                             sub_begin_ns / 1e6 / subs, sub_end_ns / 1e6 / subs);
            for (int k = 0; k < 6 && o < (int)sizeof line - 60; k++) {
                int best = -1;
                for (int i = 0; i < 64; i++)
                    if (op_ns[i] && (best < 0 || op_ns[i] > op_ns[best])) best = i;
                if (best < 0) break;
                o += snprintf(line + o, sizeof line - (size_t)o, " op %d: %.2f ms (%.0f x, %.1f us)", best, op_ns[best] / 1e6 / subs,
                              (double)op_cnt[best] / subs, op_ns[best] / 1e3 / op_cnt[best]);
                op_ns[best] = 0;
            }
            som_log("%s", line);
            memset(op_ns, 0, sizeof op_ns), memset(op_cnt, 0, sizeof op_cnt);
            sub_begin_ns = sub_end_ns = subs = 0;
            g_b2b_bytes = g_b2b_n = g_b2b_max = 0, t0 = now;
        }
    }
    static uint64_t nsub;
    uint64_t k = ++nsub;
    if (k == 1 || (k & (k - 1)) == 0) {
        char line[900];
        int o = snprintf(line, sizeof line, "gfx census: submit #%llu ops:", (unsigned long long)k);
        for (int i = 0; i < 64 && o < 850; i++)
            if (op_hist[i]) o += snprintf(line + o, sizeof line - (size_t)o, " %s=%llu", nvn_op_name((uint32_t)i), (unsigned long long)op_hist[i]);
        som_log("%s", line);
    }
    return __atomic_add_fetch(&fence, 1, __ATOMIC_ACQ_REL);
}

static int null_fence_wait(uint64_t value, uint64_t timeout_ns) {
    (void)timeout_ns;
    return value <= __atomic_load_n(&fence, __ATOMIC_ACQUIRE);   // a value never submitted is never reached
}

static uint64_t null_fence_done(void) { return __atomic_load_n(&fence, __ATOMIC_ACQUIRE); }

static void null_present(const tex_t *t, const int crop[4], uint64_t at_ns) {
    (void)at_ns;
    vkr_present(t, crop);
    int i = (int)(t->serial % CLR_SLOTS);
    if (clr_tab[i].serial == t->serial) memcpy(som_gfx_color, clr_tab[i].rgba, 16);
    uint64_t np = __atomic_add_fetch(&som_gfx_presents, 1, __ATOMIC_RELAXED);
    if (np <= 3 || np % 300 == 0)
        som_log("gfx: present #%llu serial %llu clear %s (%.2f %.2f %.2f)", (unsigned long long)np, (unsigned long long)t->serial,
                clr_tab[i].serial == t->serial ? "seen" : "none", som_gfx_color[0], som_gfx_color[1], som_gfx_color[2]);
    __atomic_fetch_add(&gfx_counters.frames, 1, __ATOMIC_RELAXED);
}

static void null_finish(void) {}

/* Read/WriteTexels move texels between the caller's rows and the texture's own storage (block-linear or pitch);
 * the Vulkan renderer uploads the storage again at the next sample (vkr_tex_written). The movie player writes its
 * decoded frames this way. */
int nvn_texels_box(const nvn_texdesc *d, unsigned level, unsigned layer, uint32_t x, uint32_t y, uint32_t z, uint32_t wb,
                   uint32_t h, void *storage, void *rows, size_t pitch, int to_linear);

static void texels_move(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *rows, uint64_t row, uint64_t img,
                        int to_linear) {
    int h = r->height > 0 ? r->height : 1, depth = r->depth > 0 ? r->depth : 1;
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!t->mem || !bpb) goto fail;
    unsigned level = v && v->set_levels ? v->base_level : 0, layer0 = v && v->set_layers ? v->min_layer : 0;
    int three = t->d.target == NVN_TEXTURE_TARGET_3D;
    uint32_t x = (uint32_t)(r->xoffset / bw) * (uint32_t)bpb, y = (uint32_t)(r->yoffset / bh);
    uint32_t wb = (uint32_t)((r->width + bw - 1) / bw) * (uint32_t)bpb, hb = (uint32_t)((h + bh - 1) / bh);
    for (int s = 0; s < depth; s++) {
        unsigned layer = three ? layer0 : layer0 + (unsigned)r->zoffset + (unsigned)s;
        uint32_t z = three ? (uint32_t)(r->zoffset + s) : 0;
        if (nvn_texels_box(&t->d, level, layer, x, y, z, wb, hb, t->mem, rows + (uint64_t)s * img, row, to_linear)) goto fail;
    }
    return;
fail: {
        static int warned;
        if (warned++ < 8)
            som_log("gfx: %sTexels on a %ux%u format %#x texture (region %d,%d %dx%d) not handled", to_linear ? "Read" : "Write",
                    t->d.width, t->d.height, t->d.format, r->xoffset, r->yoffset, r->width, r->height);
        if (to_linear)
            for (int s = 0; s < depth; s++) memset(rows + (uint64_t)s * img, 0, row * (uint64_t)h);
    }
}

static void null_read_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row,
                             uint64_t img) {
    texels_move(t, v, r, dst, row, img, 1);
}

static void null_write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src,
                              uint64_t row, uint64_t img) {
    texels_move(t, v, r, (uint8_t *)src, row, img, 0);
}

static void null_guest_written(uint64_t addr, uint64_t size) { (void)addr, (void)size; }
static void null_guest_read(uint64_t addr, uint64_t size) { (void)addr, (void)size; }
static void null_cpu_synced(uint64_t fence) { (void)fence; }

const GfxBackend gfx_null_backend = {
    .name = "null",
    .pool_init = null_pool_init,
    .pool_fini = null_pool_fini,
    .pool_map_virtual = null_pool_map_virtual,
    .texture_register = null_texture_register,
    .sampler_register = null_sampler_register,
    .texture_release = null_texture_release,
    .program_release = null_program_release,
    .submit = null_submit,
    .fence_wait = null_fence_wait,
    .fence_done = null_fence_done,
    .present = null_present,
    .finish = null_finish,
    .read_texels = null_read_texels,
    .write_texels = null_write_texels,
    .guest_written = null_guest_written,
    .guest_read = null_guest_read,
    .cpu_synced = null_cpu_synced,
};

