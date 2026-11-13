/* On-device shader translation into one cache file.
 *
 * libxlate.so is the PC translator (work\xlate_build\xlate, Ryujinx-based) built with .NET NativeAOT for
 * linux-bionic-arm64 (work\xlate_aot\build_aot.ps1). Its output is byte-identical to the PC's ahead-of-time set.
 * A stage translates in about 1.5 ms on the RP6. On the first start a background pass (sc_prepare_start, at the
 * end of this file) translates every stage in the game; a draw that comes before it translates its own stage.
 *
 * The .NET runtime only ever runs on one host thread made here (8 MB stack, host TLS): guest threads keep
 * tpidr_el0 pointing at their GuestCtx and may run on small guest stacks, neither of which the runtime expects.
 *
 * <data>/shaders.cache:
 *   header  64 bytes: "SOMSHC01", u32 version, u32 0, char id[48] (translator build id + host answers)
 *   records append-only: u32 'SCRE', u16 key length, u16 stage, u32 spv length, u32 res length, u32 crc32 of
 *           (key, spv, res), then key, spv, res.
 * At start-up the file is indexed and cut at the first damaged record (a crash mid-append costs that one shader).
 * A different id (new translator, other host answers) starts the cache afresh. */
#include "shader_cache.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "unity_scan.h"
#include "vkr.h"

extern void som_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern const char *som_data_dir(void);
extern const char *som_game_dir(void); /* services_fs.c: where exefs/ and romfs.bin are */

/* Host answers for the translator: softfloat64, storage buffer alignment, subgroup size, clip distances. The cache header
 * records them, so another GPU starts a fresh cache; on the Adreno 740 the header stays "sf64 sb64 sg64" as before.
 * - sbuf_align is NOT a device property here: vkr.c binds each storage buffer from its address rounded down to 64
 *   (b->addr & 63), and the shaders must count offsets from the same place. It stays 64 (both GPUs' minimum divides it).
 * - subgroup comes from the device (Adreno 740: 64, Mali Valhall: 16).
 * - a device without shaderClipDistance (Mali-G68) gets "clip_distance": false and " nc" in the header.
 * - Shader precision (sidebar, game_patches.c): Fast sends "reduced": true and adds " rp"; Fastest also sends
 *   "half": true and adds " hp". Each mode has its own file (shaders-fast.cache, shaders-half.cache), so switching
 *   back and forth keeps every cache. */
extern int som_shader_precision(void);
static unsigned hs_sbuf_align = 64, hs_subgroup = 64;
static int hs_no_clip, hs_reduced, hs_half;
static int hs_half_mask = -1; /* <data>/half_mask (debugging Fastest): which op classes go fp16 (translator HalfClass); -1 all */
static void cache_path(char *path, size_t n) {
    if (hs_half && hs_half_mask != -1) { /* a debug mask gets its own file: the 48-byte header id has no room for it */
        snprintf(path, n, "%s/shaders-half-m%x.cache", som_data_dir(), (unsigned)hs_half_mask);
        return;
    }
    snprintf(path, n, "%s/%s", som_data_dir(), hs_half ? "shaders-half.cache" : hs_reduced ? "shaders-fast.cache" : "shaders.cache");
}
static void host_answers_init(void) {
    /* the Vulkan device may still be coming up: wait a little, then fall back to the Adreno values */
    for (int i = 0; i < 300 && !__atomic_load_n(&g_vkr.ready, __ATOMIC_ACQUIRE); i++) usleep(10000);
    if (__atomic_load_n(&g_vkr.ready, __ATOMIC_ACQUIRE)) {
        if (g_vkr.subgroup_size) hs_subgroup = g_vkr.subgroup_size;
        hs_no_clip = !g_vkr.clip_distance;
    }
}
#define REC_MAGIC 0x45524353u         /* "SCRE" */
#define HDR_SIZE 64
#define REC_HDR 20

typedef int (*xl_translate_fn)(const char *, const uint8_t *, int, const uint8_t *, int, uint8_t **, int *, char **, char **);
typedef void (*xl_free_fn)(void *);
typedef const char *(*xl_version_fn)(void);

typedef struct {
    char key[40];
    uint64_t off; /* of the record's key in the file */
    uint32_t spv_len, res_len;
} Entry;

static struct {
    pthread_once_t once;
    int ok, fd;
    uint64_t end;
    Entry *tab; /* open addressing, cap a power of two */
    size_t cap, n;
    pthread_mutex_t mu; /* index, file and translation (one at a time) */
    xl_translate_fn translate;
    xl_free_fn xfree;
    xl_version_fn version;
    /* translation thread: runs fn(arg) for a caller that waits */
    pthread_mutex_t qmu;
    pthread_cond_t qcv;
    void (*fn)(void *);
    void *arg;
    int done;
    unsigned hits, translated, failed;
    double ms;
} sc = {.once = PTHREAD_ONCE_INIT, .fd = -1, .mu = PTHREAD_MUTEX_INITIALIZER, .qmu = PTHREAD_MUTEX_INITIALIZER,
        .qcv = PTHREAD_COND_INITIALIZER};

static uint32_t crc32_upd(uint32_t c, const void *p, size_t n) {
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            t[i] = v;
        }
    const uint8_t *b = p;
    c = ~c;
    while (n--) c = t[(c ^ *b++) & 0xff] ^ (c >> 8);
    return ~c;
}

static uint64_t key_hash(const char *k) {
    uint64_t h = 1469598103934665603ull;
    while (*k) h = (h ^ (uint8_t)*k++) * 1099511628211ull;
    return h;
}

static Entry *find(const char *key) {
    if (!sc.cap) return NULL;
    for (size_t i = key_hash(key) & (sc.cap - 1);; i = (i + 1) & (sc.cap - 1)) {
        if (!sc.tab[i].key[0]) return NULL;
        if (!strcmp(sc.tab[i].key, key)) return &sc.tab[i];
    }
}

static void insert(const char *key, uint64_t off, uint32_t spv_len, uint32_t res_len) {
    if ((sc.n + 1) * 2 > sc.cap) {
        size_t ocap = sc.cap, ncap = sc.cap ? sc.cap * 2 : 8192;
        Entry *old = sc.tab;
        sc.tab = calloc(ncap, sizeof *sc.tab), sc.cap = ncap, sc.n = 0;
        for (size_t i = 0; i < ocap; i++)
            if (old[i].key[0]) insert(old[i].key, old[i].off, old[i].spv_len, old[i].res_len);
        free(old);
    }
    size_t i = key_hash(key) & (sc.cap - 1);
    while (sc.tab[i].key[0] && strcmp(sc.tab[i].key, key)) i = (i + 1) & (sc.cap - 1);
    if (!sc.tab[i].key[0]) sc.n++;
    snprintf(sc.tab[i].key, sizeof sc.tab[i].key, "%s", key);
    sc.tab[i].off = off, sc.tab[i].spv_len = spv_len, sc.tab[i].res_len = res_len;
}

/* ---- translation thread */
static void *xlate_thread(void *unused) {
    (void)unused;
    pthread_mutex_lock(&sc.qmu);
    for (;;) {
        while (!sc.fn || sc.done) pthread_cond_wait(&sc.qcv, &sc.qmu);
        void (*fn)(void *) = sc.fn;
        void *arg = sc.arg;
        pthread_mutex_unlock(&sc.qmu);
        fn(arg);
        pthread_mutex_lock(&sc.qmu);
        sc.done = 1;
        pthread_cond_broadcast(&sc.qcv);
    }
    return NULL;
}

/* runs fn(arg) on the translation thread and waits; callers hold sc.mu, so there is one request at a time */
static void on_xlate_thread(void (*fn)(void *), void *arg) {
    pthread_mutex_lock(&sc.qmu);
    sc.fn = fn, sc.arg = arg, sc.done = 0;
    pthread_cond_broadcast(&sc.qcv);
    while (!sc.done) pthread_cond_wait(&sc.qcv, &sc.qmu);
    sc.fn = NULL;
    pthread_mutex_unlock(&sc.qmu);
}

static void get_version(void *out) { *(const char **)out = sc.version(); }

/* ---- file */
static int pread_all(void *b, size_t n, uint64_t off) {
    uint8_t *p = b;
    while (n) {
        ssize_t r = pread(sc.fd, p, n, (off_t)off);
        if (r <= 0) return 0;
        p += r, n -= (size_t)r, off += (uint64_t)r;
    }
    return 1;
}

static int pwrite_all(const void *b, size_t n, uint64_t off) {
    const uint8_t *p = b;
    while (n) {
        ssize_t r = pwrite(sc.fd, p, n, (off_t)off);
        if (r <= 0) return 0;
        p += r, n -= (size_t)r, off += (uint64_t)r;
    }
    return 1;
}

static void open_cache(const char *id) {
    char path[600];
    cache_path(path, sizeof path);
    sc.fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (sc.fd < 0) {
        som_log("shader cache: cannot open %s: %s", path, strerror(errno));
        return;
    }
    struct stat st;
    fstat(sc.fd, &st);
    uint64_t size = (uint64_t)st.st_size;
    uint8_t hdr[HDR_SIZE] = {0}, want[HDR_SIZE] = {0};
    memcpy(want, "SOMSHC01", 8);
    uint32_t version = 1;
    memcpy(want + 8, &version, 4);
    snprintf((char *)want + 16, 48, "%s", id);
    if (size < HDR_SIZE || !pread_all(hdr, HDR_SIZE, 0) || memcmp(hdr, want, HDR_SIZE)) {
        if (size) som_log("shader cache: %s is from another translator or host (\"%.48s\"): starting afresh", path, (const char *)hdr + 16);
        if (ftruncate(sc.fd, 0) || !pwrite_all(want, HDR_SIZE, 0)) {
            som_log("shader cache: cannot write %s: %s", path, strerror(errno));
            close(sc.fd), sc.fd = -1;
            return;
        }
        size = HDR_SIZE;
    }
    /* index the records; the file ends at the first damaged one */
    uint64_t off = HDR_SIZE;
    uint8_t *buf = NULL;
    size_t bcap = 0;
    while (off + REC_HDR <= size) {
        uint32_t h[5];
        if (!pread_all(h, REC_HDR, off)) break;
        uint32_t klen = h[1] & 0xffff, slen = h[2], rlen = h[3];
        uint64_t body = (uint64_t)klen + slen + rlen;
        if (h[0] != REC_MAGIC || !klen || klen >= 40 || off + REC_HDR + body > size) break;
        if (body > bcap) free(buf), bcap = body, buf = malloc(bcap);
        if (!buf || !pread_all(buf, body, off + REC_HDR) || crc32_upd(0, buf, body) != h[4]) break;
        char key[40];
        memcpy(key, buf, klen), key[klen] = 0;
        insert(key, off + REC_HDR, slen, rlen);
        off += REC_HDR + body;
    }
    free(buf);
    if (off < size) {
        som_log("shader cache: damaged record at %llu of %llu bytes: cut there", (unsigned long long)off, (unsigned long long)size);
        if (ftruncate(sc.fd, (off_t)off)) som_log("shader cache: truncate failed: %s", strerror(errno));
    }
    sc.end = off;
    som_log("shader cache: %s: %zu shaders, %llu bytes, id \"%s\"", path, sc.n, (unsigned long long)off, id);
}

static void sc_init(void) {
    int prec = som_shader_precision(); /* set from the sidebar prefs before the game starts */
    hs_reduced = prec >= 1, hs_half = prec == 2;
    if (hs_half) {
        char mp[600];
        snprintf(mp, sizeof mp, "%s/half_mask", som_data_dir());
        FILE *mf = fopen(mp, "r");
        if (mf) {
            if (fscanf(mf, "%i", &hs_half_mask) != 1) hs_half_mask = -1;
            fclose(mf);
            som_log("shader cache: half_mask %#x", (unsigned)hs_half_mask);
        }
    }
    void *h = dlopen("libxlate.so", RTLD_NOW | RTLD_LOCAL);
    if (h) {
        sc.translate = (xl_translate_fn)dlsym(h, "xlate_translate");
        sc.xfree = (xl_free_fn)dlsym(h, "xlate_free");
        sc.version = (xl_version_fn)dlsym(h, "xlate_version");
    }
    char id[48];
    if (!sc.translate || !sc.xfree || !sc.version) {
        som_log("shader cache: libxlate.so not usable (%s): no on-device translation, cache read-only", h ? "exports missing" : dlerror());
        sc.translate = NULL;
        /* without a translator the cache can still be read, whatever build wrote it */
        snprintf(id, sizeof id, "?");
    } else {
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 8u << 20);
        pthread_t th;
        if (pthread_create(&th, &at, xlate_thread, NULL)) {
            som_log("shader cache: no translation thread");
            sc.translate = NULL;
        } else {
            pthread_setname_np(th, "som-xlate");
            pthread_detach(th);
        }
        pthread_attr_destroy(&at);
        const char *v = "?";
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (sc.translate) on_xlate_thread(get_version, &v);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        host_answers_init();
        snprintf(id, sizeof id, "%s sf64 sb%u sg%u%s%s%s", v, hs_sbuf_align, hs_subgroup, hs_no_clip ? " nc" : "", hs_reduced ? " rp" : "",
                 hs_half ? " hp" : "");
        som_log("shader cache: translator %s started in %.1f ms", v, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    }
    if (!sc.translate) {
        /* read-only: accept whatever header is there */
        char path[600];
        cache_path(path, sizeof path);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        char hdr[HDR_SIZE] = {0};
        if (fd >= 0 && read(fd, hdr, HDR_SIZE) == HDR_SIZE) snprintf(id, sizeof id, "%.47s", hdr + 16);
        if (fd >= 0) close(fd);
    }
    open_cache(id);
    sc.ok = sc.fd >= 0 || sc.translate;
}

/* ---- jobs (the fields tools/translate_captured.py and tools/aot_shaders.py send) */
static const char *const TEX[] = {NULL, "1d", "1darray", "buffer", "2d", "2darray", "2dms", "2dmsarray", "3d", "cube", "cubearray"};
static const char *const ATTR[] = {"float", "sint", "uint"};

static int put_tex_map(char *o, size_t n, const char *name, const uint8_t *t) {
    int w = snprintf(o, n, ",\"%s\":{", name), first = 1;
    for (int i = 0; i < 32; i++)
        if (t[i] > 0 && t[i] < 11) {
            if (!strcmp(name, "img")) w += snprintf(o + w, n - (size_t)w, "%s\"%d\":{\"type\":\"%s\",\"fmt\":\"R8G8B8A8Unorm\"}", first ? "" : ",", i, TEX[t[i]]);
            else w += snprintf(o + w, n - (size_t)w, "%s\"%d\":\"%s\"", first ? "" : ",", i, TEX[t[i]]);
            first = 0;
        }
    return w + snprintf(o + w, n - (size_t)w, "}");
}

static int put_attr_map(char *o, size_t n, const char *name, const uint8_t *t, int count) {
    int w = snprintf(o, n, ",\"%s\":{", name), first = 1;
    for (int i = 0; i < count; i++)
        if (t[i] && t[i] < 3) w += snprintf(o + w, n - (size_t)w, "%s\"%d\":\"%s\"", first ? "" : ",", i, ATTR[t[i]]), first = 0;
    return w + snprintf(o + w, n - (size_t)w, "}");
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

/* job JSON for key; 0 when the key does not describe the stage bound now */
static int build_job(const char *key, int stage, const uint8_t *ctl, char *job, size_t n) {
    uint32_t nvn_stage;
    memcpy(&nvn_stage, ctl + 0x714, 4);
    const char *name = nvn_stage == 0 ? "vs" : nvn_stage == 1 ? "ps" : nvn_stage == 5 ? "cs" : NULL;
    if (!name) return 0;
    uint8_t tex[32] = {0}, img[32] = {0}, attr[32] = {0}, fout[8] = {0};
    uint32_t shared = 0;
    size_t kl = strlen(key);
    if (kl == 33 && key[16] == '_') {
        /* "<hash>_<answers hash>": the answers of the current draw state (the renderer made the key from it) */
        uint8_t a[4 + 32 * 4 + 8 + 4];
        if (!ts_stage_answers(stage, a, key)) return 0;
        memcpy(tex, a + 4, 32), memcpy(img, a + 36, 32), memcpy(attr, a + 100, 32), memcpy(fout, a + 132, 8);
        memcpy(&shared, a + 140, 4);
    } else if (kl > 18 && key[16] == '_' && key[17] == 't') {
        /* "<hash>_t" then (slot: 2 hex digits, type: 1 hex digit) per texture it asked about */
        for (const char *p = key + 18; p[0] && p[1] && p[2]; p += 3) {
            int s = hexval(p[0]) * 16 + hexval(p[1]), t = hexval(p[2]);
            if (s >= 0 && s < 32 && t > 0) tex[s] = (uint8_t)t;
        }
    } else if (kl != 16) {
        return 0;
    }
    int w = snprintf(job, n, "{\"key\":\"%s\",\"stage\":\"%s\",\"debug\":false", key, name);
    w += put_tex_map(job + w, n - (size_t)w, "tex", tex);
    w += put_tex_map(job + w, n - (size_t)w, "img", img);
    w += put_attr_map(job + w, n - (size_t)w, "attr", attr, 32);
    w += put_attr_map(job + w, n - (size_t)w, "fout", fout, 8);
    w += snprintf(job + w, n - (size_t)w, ",\"shared\":%u,\"softfloat64\":true,\"sbuf_align\":%u,\"subgroup\":%u", shared, hs_sbuf_align, hs_subgroup);
    if (hs_no_clip) w += snprintf(job + w, n - (size_t)w, ",\"clip_distance\":false");
    if (hs_reduced) w += snprintf(job + w, n - (size_t)w, ",\"reduced\":true");
    if (hs_half) w += snprintf(job + w, n - (size_t)w, ",\"half\":true");
    if (hs_half && hs_half_mask != -1) w += snprintf(job + w, n - (size_t)w, ",\"half_mask\":%d", hs_half_mask);
    if (nvn_stage == 1 && (ctl[0x718] & 1)) w += snprintf(job + w, n - (size_t)w, ",\"early_fragment_tests\":true");
    if (nvn_stage == 5) {
        uint32_t l[3];
        memcpy(l, ctl + 0x748, 12);
        w += snprintf(job + w, n - (size_t)w, ",\"local\":[%u,%u,%u]", l[0], l[1], l[2]);
    }
    snprintf(job + w, n - (size_t)w, "}");
    return (size_t)w + 1 < n;
}

typedef struct {
    const char *job;
    const uint8_t *code, *c1;
    int code_len, c1_len, rc;
    uint8_t *spv;
    int spv_len;
    char *res, *err;
} XJob;

static void run_job(void *p) {
    XJob *j = p;
    j->rc = sc.translate(j->job, j->code, j->code_len, j->c1, j->c1_len, &j->spv, &j->spv_len, &j->res, &j->err);
}

static double ms_since(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1e3 + (t1.tv_nsec - t0->tv_nsec) / 1e6;
}

/* Translates one stage under key. direct: on the calling thread (a host thread with a big stack: the prepare
 * workers); otherwise on the translation thread. Returns 1 with malloc'd *spv / *res. Needs no lock. */
static int translate_stage(const char *key, int stage, const uint8_t *ctl, const uint8_t *code, uint32_t len, int direct,
                           uint8_t **spv, size_t *spv_len, char **res) {
    uint32_t c1off, nvn_stage;
    memcpy(&c1off, ctl + 0x700, 4);
    memcpy(&nvn_stage, ctl + 0x714, 4);
    uint32_t skip = nvn_stage == 5 ? 0 : 0x30; /* graphics code: the translator wants the 0x50-byte SPH, past NVN's 0x30 */
    char job[2048];
    if (len <= skip || c1off > len || !build_job(key, stage, ctl, job, sizeof job)) {
        som_log("shader cache: no job for %s (stage %u, %u bytes)", key, nvn_stage, len);
        return 0;
    }
    XJob j = {job, code + skip, code + c1off, (int)(len - skip), (int)(len - c1off), 0, NULL, 0, NULL, NULL};
    if (direct) run_job(&j);
    else on_xlate_thread(run_job, &j);
    if (j.rc) {
        if (__atomic_add_fetch(&sc.failed, 1, __ATOMIC_RELAXED) <= 20) som_log("shader cache: translating %s failed: %s", key, j.err ? j.err : "?");
        if (j.err) sc.xfree(j.err);
        return 0;
    }
    size_t rl = strlen(j.res);
    *spv = malloc((size_t)j.spv_len), *res = malloc(rl + 1);
    memcpy(*spv, j.spv, (size_t)j.spv_len), memcpy(*res, j.res, rl + 1);
    *spv_len = (size_t)j.spv_len;
    sc.xfree(j.spv), sc.xfree(j.res);
    return 1;
}

/* appends a record and indexes it; sc.mu held */
static void append_locked(const char *key, int stage, const void *spv, size_t spv_len, const char *res) {
    if (sc.fd < 0 || find(key)) return;
    uint32_t klen = (uint32_t)strlen(key), rl = (uint32_t)strlen(res);
    uint32_t h[5] = {REC_MAGIC, klen | (uint32_t)(stage & 0xffff) << 16, (uint32_t)spv_len, rl, 0};
    h[4] = crc32_upd(crc32_upd(crc32_upd(0, key, klen), spv, spv_len), res, rl); /* = crc32 of the concatenation */
    if (pwrite_all(h, REC_HDR, sc.end) && pwrite_all(key, klen, sc.end + REC_HDR) && pwrite_all(spv, spv_len, sc.end + REC_HDR + klen) &&
        pwrite_all(res, rl, sc.end + REC_HDR + klen + spv_len)) {
        insert(key, sc.end + REC_HDR, (uint32_t)spv_len, rl);
        sc.end += REC_HDR + klen + spv_len + rl;
    } else {
        som_log("shader cache: append failed: %s", strerror(errno));
    }
}

/* copies of an entry's data; sc.mu held */
static int read_entry(const char *key, const Entry *e, uint8_t **spv, size_t *spv_len, char **res) {
    size_t kl = strlen(key);
    *spv = malloc(e->spv_len ? e->spv_len : 1), *res = malloc((size_t)e->res_len + 1);
    if (*spv && *res && pread_all(*spv, e->spv_len, e->off + kl) && pread_all(*res, e->res_len, e->off + kl + e->spv_len)) {
        (*res)[e->res_len] = 0;
        *spv_len = e->spv_len;
        return 1;
    }
    free(*spv), free(*res);
    return 0;
}

/* draw-time miss: the stage bound for the current draw; sc.mu held */
static int translate_now(const char *key, int stage, uint8_t **spv, size_t *spv_len, char **res) {
    char bound[40];
    const uint8_t *ctl, *code;
    uint32_t len;
    if (!ts_shader_key(stage, bound, &ctl, &code, &len) || strncmp(bound, key, 16)) {
        som_log("shader cache: %s is not the stage bound now (%.16s): not translated", key, bound);
        return 0;
    }
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (!translate_stage(key, stage, ctl, code, len, 0, spv, spv_len, res)) return 0;
    double ms = ms_since(&t0);
    sc.ms += ms;
    sc.translated++;
    if (sc.translated <= 5 || sc.translated % 100 == 0 || ms > 20)
        som_log("shader cache: translated %s at its first draw in %.1f ms (%u at draw time, %.0f ms in all, %u failed, %u hits)",
                key, ms, sc.translated, sc.ms, sc.failed, sc.hits);
    append_locked(key, stage, *spv, *spv_len, *res);
    return 1;
}

int sc_get(const char *key, int stage, uint8_t **spv, size_t *spv_len, char **res) {
    pthread_once(&sc.once, sc_init);
    if (!sc.ok) return 0;
    pthread_mutex_lock(&sc.mu);
    Entry *e = find(key);
    int r = e && read_entry(key, e, spv, spv_len, res);
    if (r) sc.hits++;
    if (!r && sc.translate) r = translate_now(key, stage, spv, spv_len, res);
    pthread_mutex_unlock(&sc.mu);
    return r;
}

/* ---- prepare all: every stage in the game, translated before it is drawn (unity_scan.c finds them).
 * Runs in the background from start-up (sc_prepare_start), on low-priority host threads, while the game boots.
 * A draw that comes first translates its stage itself, as without the pass. When done, a "!prepared" record holds
 * the romfs image's size and modification time, so later starts skip the scan. */
#define PREP_KEY "!prepared"

static struct {
    unsigned seen, fresh, cached, variants, skipped;
} prep;

/* the texture slots a translation asked about ("tex_queried": [..] in its res.json) */
static int tex_queried(const char *res, int slots[7]) {
    const char *q = strstr(res, "\"tex_queried\": [");
    int n = 0;
    for (q = q ? q + 16 : NULL; q && *q && *q != ']' && n < 7; q++)
        if (*q >= '0' && *q <= '9') {
            char *end;
            slots[n++] = (int)strtol(q, &end, 10);
            q = end - 1;
        }
    return n;
}

/* translates key unless cached; returns 1 with *res (malloc'd) when res is wanted and the stage translated */
static int prep_key(const char *key, int stage, const uint8_t *ctl, const uint8_t *code, uint32_t len, char **res_out) {
    uint8_t *spv;
    size_t sl;
    char *res;
    pthread_mutex_lock(&sc.mu);
    Entry *e = find(key);
    int have = e && (!res_out || read_entry(key, e, &spv, &sl, &res));
    pthread_mutex_unlock(&sc.mu);
    if (have) {
        __atomic_add_fetch(&prep.cached, 1, __ATOMIC_RELAXED);
        if (res_out) free(spv), *res_out = res;
        return 1;
    }
    if (!translate_stage(key, stage, ctl, code, len, 1, &spv, &sl, &res)) return 0;
    __atomic_add_fetch(&prep.fresh, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&sc.mu);
    append_locked(key, stage, spv, sl, res);
    pthread_mutex_unlock(&sc.mu);
    free(spv);
    if (res_out) *res_out = res;
    else free(res);
    return 1;
}

static void prep_stage(void *ctx, uint32_t nvn_stage, const uint8_t *ctl, const uint8_t *code, uint32_t len) {
    (void)ctx;
    static __thread int niced;
    if (!niced) niced = 1, setpriority(PRIO_PROCESS, (id_t)gettid(), 10); /* behind the game's own threads */
    __atomic_add_fetch(&prep.seen, 1, __ATOMIC_RELAXED);
    if (nvn_stage != 0 && nvn_stage != 1 && nvn_stage != 5) { /* geometry/tessellation: not translated */
        __atomic_add_fetch(&prep.skipped, 1, __ATOMIC_RELAXED);
        return;
    }
    char key[40];
    extern uint64_t som_shader_hash(const uint8_t *ctl, const uint8_t *code, uint32_t len);
    snprintf(key, sizeof key, "%016llx", (unsigned long long)som_shader_hash(ctl, code, len));
    char *res = NULL;
    if (!prep_key(key, (int)nvn_stage, ctl, code, len, nvn_stage <= 1 ? &res : NULL) || !res) return;
    /* The few stages that asked about a texture's type also get one variant per type of the asked slots
     * ("<hash>_t" + slot, type per slot; vkr.c shader_load builds the same name from the bound textures): every
     * combination for one or two slots, else one type for all (as tools/aot_shaders.py). */
    int slots[7], ns = tex_queried(res, slots);
    free(res);
    if (!ns) return;
    int combos = ns == 1 ? 10 : ns == 2 ? 100 : 10;
    for (int c = 0; c < combos; c++) {
        char vk[40];
        int w = snprintf(vk, sizeof vk, "%s_t", key);
        for (int i = 0; i < ns; i++) {
            int t = ns == 2 ? (i == 0 ? c / 10 : c % 10) + 1 : c + 1;
            w += snprintf(vk + w, sizeof vk - (size_t)w, "%02x%x", slots[i], t);
        }
        if (prep_key(vk, (int)nvn_stage, ctl, code, len, NULL)) __atomic_add_fetch(&prep.variants, 1, __ATOMIC_RELAXED);
    }
}

static void *prep_thread(void *unused) {
    (void)unused;
    pthread_once(&sc.once, sc_init);
    if (!sc.ok || !sc.translate || sc.fd < 0) return NULL;
    char romfs[600], nso[600], ident[128];
    snprintf(romfs, sizeof romfs, "%s/romfs.bin", som_game_dir());
    snprintf(nso, sizeof nso, "%s/exefs/main", som_game_dir());
    struct stat a, b;
    if (stat(romfs, &a) || stat(nso, &b)) {
        som_log("shader prepare: no %s or %s", romfs, nso);
        return NULL;
    }
    snprintf(ident, sizeof ident, "romfs %lld %lld main %lld", (long long)a.st_size, (long long)a.st_mtime, (long long)b.st_size);
    pthread_mutex_lock(&sc.mu);
    Entry *e = find(PREP_KEY);
    uint8_t *spv = NULL;
    size_t sl = 0;
    char *res = NULL;
    int done = e && read_entry(PREP_KEY, e, &spv, &sl, &res) && sl == strlen(ident) && !memcmp(spv, ident, sl);
    pthread_mutex_unlock(&sc.mu);
    if (done) som_log("shader prepare: already done for this game data (%s)", res);
    free(spv), free(res);
    if (done) return NULL;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    som_log("shader prepare: translating every shader in the game in the background (%zu cached so far)", sc.n);
    int ne = us_scan_nso(nso, prep_stage, NULL);
    UsStats st;
    int rc = us_scan_romfs(romfs, 3, prep_stage, NULL, &st);
    double ms = ms_since(&t0);
    char sum[256];
    snprintf(sum, sizeof sum, "{\"stages\": %u, \"translated\": %u, \"cached\": %u, \"variants\": %u, \"failed\": %u, \"ms\": %.0f}",
             prep.seen - prep.skipped, prep.fresh, prep.cached, prep.variants, sc.failed, ms);
    som_log("shader prepare: %s; %u holder files of %u, engine stages %d, %zu shaders in the cache", sum, st.holders,
            st.candidates, ne, sc.n);
    if (rc == 0 && ne >= 0) {
        pthread_mutex_lock(&sc.mu);
        append_locked(PREP_KEY, 0xffff, ident, strlen(ident), sum);
        fdatasync(sc.fd);
        pthread_mutex_unlock(&sc.mu);
    }
    return NULL;
}

void sc_prepare_start(void) {
    char p[600];
    snprintf(p, sizeof p, "%s/no_prepare", som_data_dir());
    if (!access(p, F_OK)) {
        som_log("shader prepare: off (no_prepare)");
        return;
    }
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    if (pthread_create(&th, &at, prep_thread, NULL)) som_log("shader prepare: no thread");
    else pthread_setname_np(th, "som-prep");
    pthread_attr_destroy(&at);
}
