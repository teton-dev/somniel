/* C port of tools/unity_shaders.py + the file scan of tools/aot_shaders.py, for the on-device "prepare all" pass.
 *
 * romfs.bin   0x50-byte header (10 u64: header size, dir hash/meta, file hash/meta offsets and sizes, data offset);
 *             file meta entries: u32 parent, u32 sibling, u64 data, u64 size, u32 hash next, u32 name length, name
 *             padded to 4. Only names are needed (the extension decides whether a file can be a Unity container).
 * UnityFS     big-endian header, block table (LZ4), nodes; each node a serialized file or a resource (.resS).
 *             Only the blocks a node overlaps are decompressed, so the large texture resources are never touched.
 * Serialized  header, type table (type trees in bundles), object table. Shader is class 48; its compressedBlob holds
 *             per platform a list of LZ4 segments (offsets, compressedLengths, decompressedLengths).
 * Stage       NVN control section (magic 0x98761234, 0x880 bytes) directly followed by its code. */
#include "unity_scan.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern void som_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int som_lz4_decompress(const uint8_t *src, size_t src_size, uint8_t *dst, size_t dst_cap);

#define SHADER_CLASS 48
#define CTL 0x880

/* ---- bounds-checked reader; short = ran past the end (more data might help) */
typedef struct {
    const uint8_t *d;
    size_t n, p;
    int big, shrt;
} R;

static int need(R *r, size_t k) {
    if (r->shrt || r->p + k > r->n) {
        r->shrt = 1;
        return 0;
    }
    return 1;
}
static uint64_t rd(R *r, int k) {
    if (!need(r, (size_t)k)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < k; i++) v |= (uint64_t)r->d[r->p + (size_t)(r->big ? k - 1 - i : i)] << (8 * i);
    r->p += (size_t)k;
    return v;
}
static uint32_t u32(R *r) { return (uint32_t)rd(r, 4); }
static int32_t i32(R *r) { return (int32_t)rd(r, 4); }
static uint64_t u64(R *r) { return rd(r, 8); }
static void skip(R *r, size_t k) {
    if (need(r, k)) r->p += k;
}
static const char *strz(R *r) {
    if (r->shrt) return "";
    const uint8_t *z = r->p < r->n ? memchr(r->d + r->p, 0, r->n - r->p) : NULL;
    if (!z) {
        r->shrt = 1;
        return "";
    }
    const char *s = (const char *)r->d + r->p;
    r->p = (size_t)(z - r->d) + 1;
    return s;
}
static void align(R *r, size_t a) { r->p = (r->p + a - 1) & ~(a - 1); }

static int pread_all(int fd, void *b, size_t n, uint64_t off) {
    uint8_t *p = b;
    while (n) {
        ssize_t k = pread(fd, p, n, (off_t)off);
        if (k <= 0) return 0;
        p += k, n -= (size_t)k, off += (uint64_t)k;
    }
    return 1;
}

/* ---- stages in a decompressed segment */
static unsigned stages_in(const uint8_t *seg, size_t len, us_stage_fn fn, void *ctx) {
    static const uint8_t magic[4] = {0x34, 0x12, 0x76, 0x98};
    unsigned n = 0;
    size_t pos = 0;
    while (pos + 4 <= len) {
        const uint8_t *m = memmem(seg + pos, len - pos, magic, 4);
        if (!m) break;
        size_t at = (size_t)(m - seg);
        pos = at + 4;
        if (at + CTL > len) continue;
        uint32_t c1, length, stage;
        memcpy(&c1, seg + at + 0x700, 4), memcpy(&length, seg + at + 0x704, 4), memcpy(&stage, seg + at + 0x714, 4);
        if (stage > 5 || !length || length > len - at - CTL || c1 > length) continue;
        fn(ctx, stage, seg + at, seg + at + CTL, length);
        n++;
        pos = at + CTL + length;
    }
    return n;
}

/* ---- serialized files */
typedef struct {
    uint8_t level, array, align;
    int32_t size;
    const char *name;
} TNode;

typedef struct {
    int cls;
    TNode *tree; /* NULL without type trees */
    int nn;
} SType;

typedef struct {
    R r;
    int version, has_trees, ntypes;
    SType *types;
    uint64_t data_off;
} Ser;

static void ser_free(Ser *s) {
    for (int i = 0; i < s->ntypes; i++) free(s->types[i].tree);
    free(s->types);
}

/* header and type table; want_trees 0 only lists class ids. Returns 1, 0 (not a serialized file), -1 (short). */
static int ser_open(Ser *s, const uint8_t *d, size_t n, int want_trees) {
    memset(s, 0, sizeof *s);
    R *r = &s->r;
    r->d = d, r->n = n, r->big = 1;
    u32(r), u32(r);
    s->version = (int)u32(r);
    s->data_off = u32(r);
    int big = (int)rd(r, 1);
    skip(r, 3);
    if (s->version >= 22) u32(r), u64(r), s->data_off = u64(r), u64(r);
    if (r->shrt) return -1;
    if (s->version < 17 || s->version > 22) return 0;
    r->big = big;
    strz(r);
    i32(r);
    s->has_trees = (int)rd(r, 1);
    int nt = i32(r);
    if (r->shrt) return -1;
    if (nt < 0 || nt > 100000) return 0;
    s->types = calloc((size_t)nt ? (size_t)nt : 1, sizeof *s->types);
    for (int t = 0; t < nt; t++) {
        SType *ty = &s->types[t];
        ty->cls = i32(r);
        skip(r, 3);
        if (ty->cls == 114) skip(r, 16);
        skip(r, 16);
        if (s->has_trees) {
            int nn = i32(r), sb = i32(r);
            if (r->shrt) break;
            if (nn < 0 || sb < 0 || nn > 1000000) { s->ntypes = t; return 0; }
            size_t recs = r->p;
            skip(r, 32 * (size_t)nn);
            size_t strs = r->p;
            skip(r, (size_t)sb);
            if (want_trees && !r->shrt) {
                ty->tree = malloc(sizeof(TNode) * (size_t)(nn ? nn : 1));
                ty->nn = nn;
                R q = {d, n, recs, r->big, 0};
                for (int k = 0; k < nn; k++) {
                    rd(&q, 2);
                    TNode *tn = &ty->tree[k];
                    tn->level = (uint8_t)rd(&q, 1);
                    tn->array = (uint8_t)(rd(&q, 1) & 1);
                    u32(&q);
                    uint32_t no = u32(&q);
                    tn->size = i32(&q);
                    i32(&q);
                    tn->align = (u32(&q) & 0x4000) != 0;
                    u64(&q);
                    /* local names only; common-string names (high bit) never match the fields wanted here */
                    tn->name = !(no & 0x80000000u) && no < (uint32_t)sb && memchr(d + strs + no, 0, (size_t)sb - no)
                                   ? (const char *)d + strs + no : "";
                }
            }
            if (s->version >= 21) {
                int deps = i32(r);
                if (deps < 0) { s->ntypes = t + 1; return 0; }
                skip(r, 4 * (size_t)deps);
            }
        }
        s->ntypes = t + 1;
        if (r->shrt) break;
    }
    return r->shrt ? -1 : 1;
}

static int ser_has_shader(const uint8_t *d, size_t n) {
    Ser s;
    int rc = ser_open(&s, d, n, 0);
    int has = 0;
    for (int i = 0; i < s.ntypes; i++) has |= s.types[i].cls == SHADER_CLASS;
    ser_free(&s);
    if (has) return 1;
    return rc < 0 ? -1 : 0;
}

/* The Shader type tree of the first bundle that has one; data.unity3d has no type trees and uses it. */
static struct {
    pthread_mutex_t mu;
    TNode *tree;
    int nn;
    char *names;
} g_tree = {PTHREAD_MUTEX_INITIALIZER, NULL, 0, NULL};

static void keep_tree(const TNode *t, int nn) {
    pthread_mutex_lock(&g_tree.mu);
    if (!g_tree.tree) {
        size_t total = 0;
        for (int i = 0; i < nn; i++) total += strlen(t[i].name) + 1;
        TNode *c = malloc(sizeof *c * (size_t)nn);
        char *names = malloc(total ? total : 1), *p = names;
        for (int i = 0; i < nn; i++) {
            c[i] = t[i];
            size_t l = strlen(t[i].name) + 1;
            memcpy(p, t[i].name, l);
            c[i].name = p;
            p += l;
        }
        g_tree.names = names, g_tree.nn = nn;
        __atomic_store_n(&g_tree.tree, c, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_tree.mu);
}

/* ---- type-tree walk of one Shader object, keeping the four fields that locate its programs */
typedef struct {
    uint32_t *v;
    size_t n, cap;
} U32s;

typedef struct {
    R r;
    const TNode *t;
    int nn, pos, field; /* field: 1 offsets, 2 compressedLengths, 3 decompressedLengths, 4 compressedBlob */
    U32s lists[3];
    const uint8_t *blob;
    size_t blob_len;
    int depth;
} Walk;

static void push(U32s *l, uint32_t v) {
    if (l->n == l->cap) l->cap = l->cap ? l->cap * 2 : 64, l->v = realloc(l->v, l->cap * sizeof *l->v);
    l->v[l->n++] = v;
}

static void skip_subtree(Walk *w) {
    int lv = w->t[w->pos].level;
    w->pos++;
    while (w->pos < w->nn && w->t[w->pos].level > lv) w->pos++;
}

static void node(Walk *w) {
    if (w->r.shrt || w->pos >= w->nn || ++w->depth > 64) { w->r.shrt = 1; return; }
    int i = w->pos;
    const TNode *n = &w->t[i];
    w->pos++;
    if (n->level == 1) {
        w->field = !strcmp(n->name, "offsets") ? 1 : !strcmp(n->name, "compressedLengths") ? 2
                 : !strcmp(n->name, "decompressedLengths") ? 3 : !strcmp(n->name, "compressedBlob") ? 4 : 0;
    }
    if (n->array) {
        if (w->pos >= w->nn) { w->r.shrt = 1; w->depth--; return; }
        skip_subtree(w); /* the size node */
        int elem = w->pos;
        if (elem >= w->nn) { w->r.shrt = 1; w->depth--; return; }
        int32_t count = i32(&w->r);
        const TNode *e = &w->t[elem];
        int leaf = elem + 1 >= w->nn || w->t[elem + 1].level <= e->level;
        if (count < 0) w->r.shrt = 1;
        else if (leaf && e->size == 1 && !e->align) {
            if (need(&w->r, (size_t)count)) {
                if (w->field == 4) w->blob = w->r.d + w->r.p, w->blob_len = (size_t)count;
                w->r.p += (size_t)count;
            }
            w->pos = elem;
            skip_subtree(w);
        } else {
            for (int32_t k = 0; k < count && !w->r.shrt; k++) {
                w->pos = elem;
                node(w);
            }
            if (!count) {
                w->pos = elem;
                skip_subtree(w);
            }
        }
    } else {
        int kids = 0;
        while (w->pos < w->nn && w->t[w->pos].level > n->level && !w->r.shrt) node(w), kids++;
        if (!kids) {
            if (n->size < 0 || n->size > 8) w->r.shrt = 1;
            else {
                uint64_t v = rd(&w->r, n->size);
                if (w->field >= 1 && w->field <= 3 && n->size == 4) push(&w->lists[w->field - 1], (uint32_t)v);
            }
        }
    }
    if (n->align) align(&w->r, 4);
    w->depth--;
}

/* stages of every Shader object in a serialized file; -1 when it needs a tree it does not have */
static int ser_extract(const uint8_t *d, size_t n, us_stage_fn fn, void *ctx, unsigned *nstages) {
    Ser s;
    if (ser_open(&s, d, n, 1) != 1) {
        ser_free(&s);
        return 0;
    }
    R *r = &s.r;
    int nobj = i32(r);
    int rc = 0;
    for (int o = 0; o < nobj && !r->shrt; o++) {
        align(r, 4);
        u64(r);
        uint64_t start = s.version >= 22 ? u64(r) : u32(r);
        u32(r);
        int tid = i32(r);
        if (r->shrt || tid < 0 || tid >= s.ntypes || s.types[tid].cls != SHADER_CLASS) continue;
        const TNode *tree = s.types[tid].tree;
        int nn = s.types[tid].nn;
        if (tree && nn) keep_tree(tree, nn);
        else {
            tree = __atomic_load_n(&g_tree.tree, __ATOMIC_ACQUIRE), nn = g_tree.nn;
            if (!tree) { rc = -1; continue; }
        }
        Walk w;
        memset(&w, 0, sizeof w);
        w.r = (R){d, n, (size_t)(s.data_off + start), r->big, 0};
        w.t = tree, w.nn = nn;
        node(&w);
        if (!w.r.shrt && w.blob && w.lists[0].n == w.lists[1].n && w.lists[1].n == w.lists[2].n) {
            for (size_t k = 0; k < w.lists[0].n; k++) {
                uint32_t a = w.lists[0].v[k], b = w.lists[1].v[k], c = w.lists[2].v[k];
                if ((uint64_t)a + b > w.blob_len || !c) continue;
                uint8_t *seg = malloc(c);
                if (seg && som_lz4_decompress(w.blob + a, b, seg, c) == (int)c) *nstages += stages_in(seg, c, fn, ctx);
                free(seg);
            }
        }
        for (int k = 0; k < 3; k++) free(w.lists[k].v);
    }
    ser_free(&s);
    return rc;
}

/* ---- UnityFS bundles */
typedef struct {
    uint32_t usize, csize;
    uint16_t flags;
    uint64_t uoff, foff; /* uncompressed offset in the stream, compressed offset in the file */
} Block;

typedef struct {
    uint64_t off, size;
    const char *path;
} BNode;

typedef struct {
    uint8_t *info; /* decompressed block table (node paths point into it) */
    Block *b;
    int nb;
    BNode *nodes;
    int nnodes;
} Bundle;

static void bundle_free(Bundle *b) { free(b->info), free(b->b), free(b->nodes); }

/* fd/base/size: the bundle inside romfs.bin. 1 ok, 0 not UnityFS / unsupported. */
static int bundle_open(int fd, uint64_t base, uint64_t fsize, Bundle *out) {
    memset(out, 0, sizeof *out);
    uint8_t head[512];
    size_t hn = fsize < sizeof head ? (size_t)fsize : sizeof head;
    if (hn < 48 || !pread_all(fd, head, hn, base) || memcmp(head, "UnityFS\0", 8)) return 0;
    R r = {head, hn, 8, 1, 0};
    uint32_t version = u32(&r);
    strz(&r), strz(&r);
    uint64_t size = u64(&r);
    uint32_t csize = u32(&r), usize = u32(&r), flags = u32(&r);
    if (version >= 7) align(&r, 16);
    if (r.shrt || csize > (64u << 20) || usize > (256u << 20) || size > fsize) return 0;
    uint64_t data_at, info_at;
    if (flags & 0x80) info_at = size - csize, data_at = r.p;
    else info_at = r.p, data_at = r.p + csize;
    uint8_t *raw = malloc(csize ? csize : 1);
    if (!raw || !pread_all(fd, raw, csize, base + info_at)) { free(raw); return 0; }
    uint32_t comp = flags & 0x3f;
    uint8_t *info = raw;
    size_t info_n = csize;
    if (comp == 2 || comp == 3) {
        info = malloc(usize ? usize : 1);
        if (!info || som_lz4_decompress(raw, csize, info, usize) != (int)usize) { free(raw), free(info); return 0; }
        free(raw);
        info_n = usize;
    } else if (comp) {
        free(raw);
        return 0;
    }
    R b = {info, info_n, 16, 1, 0};
    int nb = i32(&b);
    if (b.shrt || nb < 0 || nb > 1000000) { free(info); return 0; }
    out->info = info;
    out->b = calloc((size_t)nb ? (size_t)nb : 1, sizeof *out->b);
    out->nb = nb;
    if (flags & 0x200) data_at = (data_at + 15) & ~15ull;
    uint64_t uo = 0, fo = data_at;
    for (int i = 0; i < nb; i++) {
        Block *k = &out->b[i];
        k->usize = u32(&b), k->csize = u32(&b), k->flags = (uint16_t)rd(&b, 2);
        k->uoff = uo, k->foff = fo;
        uo += k->usize, fo += k->csize;
    }
    int nn = i32(&b);
    if (b.shrt || nn < 0 || nn > 100000 || fo > fsize) { bundle_free(out); return 0; }
    out->nodes = calloc((size_t)nn ? (size_t)nn : 1, sizeof *out->nodes);
    for (int i = 0; i < nn && !b.shrt; i++) {
        out->nodes[i].off = u64(&b), out->nodes[i].size = u64(&b);
        u32(&b);
        out->nodes[i].path = strz(&b);
        out->nnodes = i + 1;
    }
    if (b.shrt) { bundle_free(out); return 0; }
    return 1;
}

static int is_resource(const char *p) {
    size_t l = strlen(p);
    return (l > 5 && !strcmp(p + l - 5, ".resS")) || (l > 9 && !strcmp(p + l - 9, ".resource"));
}

/* bytes [off, off+len) of the uncompressed stream into dst; 1 ok */
static int bundle_read(int fd, uint64_t base, const Bundle *b, uint64_t off, uint64_t len, uint8_t *dst) {
    uint8_t *tmp = NULL, *cbuf = NULL;
    size_t tcap = 0, ccap = 0;
    int ok = 1;
    for (int i = 0; i < b->nb && ok && len; i++) {
        const Block *k = &b->b[i];
        if (k->uoff + k->usize <= off || k->uoff >= off + len) continue;
        if (k->usize > tcap) free(tmp), tcap = k->usize, tmp = malloc(tcap);
        if (k->csize > ccap) free(cbuf), ccap = k->csize, cbuf = malloc(ccap);
        if (!tmp || !cbuf || !pread_all(fd, cbuf, k->csize, base + k->foff)) { ok = 0; break; }
        uint32_t c = k->flags & 0x3f;
        if (c == 2 || c == 3) ok = som_lz4_decompress(cbuf, k->csize, tmp, k->usize) == (int)k->usize;
        else if (!c && k->csize == k->usize) memcpy(tmp, cbuf, k->usize);
        else ok = 0;
        uint64_t a = off > k->uoff ? off : k->uoff, e = off + len < k->uoff + k->usize ? off + len : k->uoff + k->usize;
        if (ok) memcpy(dst + (a - off), tmp + (a - k->uoff), (size_t)(e - a));
    }
    free(tmp), free(cbuf);
    return ok;
}

/* 1 holds a Shader object, 0 not, -1 unreadable */
static int file_has_shader(int fd, uint64_t base, uint64_t size) {
    Bundle b;
    if (bundle_open(fd, base, size, &b)) {
        int has = 0;
        for (int i = 0; i < b.nnodes && !has; i++) {
            const BNode *nd = &b.nodes[i];
            if (is_resource(nd->path)) continue;
            /* the type table is at the start of the node: grow the read until it parses */
            for (uint64_t want = 64 << 10;; want *= 4) {
                uint64_t len = want < nd->size ? want : nd->size;
                uint8_t *buf = malloc(len ? len : 1);
                int r = buf && bundle_read(fd, base, &b, nd->off, len, buf) ? ser_has_shader(buf, len) : 0;
                free(buf);
                if (r >= 0 || len == nd->size) { has = r > 0; break; }
            }
        }
        bundle_free(&b);
        return has;
    }
    uint8_t magic[8];
    if (size < 48 || !pread_all(fd, magic, 8, base)) return 0;
    if (!memcmp(magic, "UnityFS\0", 8)) return -1; /* a bundle bundle_open could not read */
    for (uint64_t want = 64 << 10;; want *= 4) {
        uint64_t len = want < size ? want : size;
        uint8_t *buf = malloc(len);
        int r = buf && pread_all(fd, buf, len, base) ? ser_has_shader(buf, len) : 0;
        free(buf);
        if (r >= 0 || len == size) return r > 0;
    }
}

/* stages of a holder file; -1 when it needs the shared type tree and none is known yet */
static int file_extract(int fd, uint64_t base, uint64_t size, us_stage_fn fn, void *ctx, unsigned *n) {
    Bundle b;
    int rc = 0;
    if (bundle_open(fd, base, size, &b)) {
        for (int i = 0; i < b.nnodes; i++) {
            const BNode *nd = &b.nodes[i];
            if (is_resource(nd->path) || nd->size > (1ull << 31)) continue;
            uint8_t *buf = malloc(nd->size ? nd->size : 1);
            if (buf && bundle_read(fd, base, &b, nd->off, nd->size, buf) && ser_extract(buf, nd->size, fn, ctx, n) < 0) rc = -1;
            free(buf);
        }
        bundle_free(&b);
        return rc;
    }
    if (size > (1ull << 31)) return 0;
    uint8_t *buf = malloc(size);
    if (buf && pread_all(fd, buf, size, base)) rc = ser_extract(buf, size, fn, ctx, n);
    free(buf);
    return rc;
}

/* ---- romfs */
typedef struct {
    uint64_t off, size;
    int bundle;
    const char *name;
} RFile;

typedef struct {
    int fd;
    RFile *f;
    int nf, next;
    pthread_mutex_t mu;
    RFile **later; /* holders that are not bundles (need the type tree of a bundle) */
    int nlater;
    us_stage_fn fn;
    void *ctx;
    UsStats *st;
} Scan;

static int candidate(const char *name, int *bundle) {
    const char *dot = strrchr(name, '.');
    *bundle = dot && !strcmp(dot, ".bundle");
    return !dot || *bundle || !strcmp(dot, ".unity3d") || !strcmp(dot, ".assets");
}

static void *scan_worker(void *arg) {
    Scan *s = arg;
    for (;;) {
        pthread_mutex_lock(&s->mu);
        int i = s->next < s->nf ? s->next++ : -1;
        pthread_mutex_unlock(&s->mu);
        if (i < 0) break;
        RFile *f = &s->f[i];
        int h = file_has_shader(s->fd, f->off, f->size);
        __atomic_add_fetch(&s->st->checked, 1, __ATOMIC_RELAXED);
        if (h < 0) {
            __atomic_add_fetch(&s->st->unreadable, 1, __ATOMIC_RELAXED);
            continue;
        }
        if (!h) continue;
        __atomic_add_fetch(&s->st->holders, 1, __ATOMIC_RELAXED);
        if (!f->bundle) {
            pthread_mutex_lock(&s->mu);
            s->later[s->nlater++] = f;
            pthread_mutex_unlock(&s->mu);
            continue;
        }
        unsigned n = 0;
        file_extract(s->fd, f->off, f->size, s->fn, s->ctx, &n);
        __atomic_add_fetch(&s->st->stages, n, __ATOMIC_RELAXED);
    }
    return NULL;
}

int us_scan_romfs(const char *romfs_path, int nthreads, us_stage_fn fn, void *ctx, UsStats *st) {
    memset(st, 0, sizeof *st);
    int fd = open(romfs_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        som_log("unity scan: cannot open %s", romfs_path);
        return -1;
    }
    uint64_t hdr[10];
    if (!pread_all(fd, hdr, sizeof hdr, 0) || hdr[0] != 0x50 || hdr[8] > (256u << 20)) {
        som_log("unity scan: %s has no romfs header", romfs_path);
        close(fd);
        return -1;
    }
    uint64_t meta_off = hdr[7], meta_size = hdr[8], data_off = hdr[9];
    uint8_t *meta = malloc(meta_size);
    if (!meta || !pread_all(fd, meta, meta_size, meta_off)) {
        free(meta);
        close(fd);
        return -1;
    }
    Scan s = {.fd = fd, .mu = PTHREAD_MUTEX_INITIALIZER, .fn = fn, .ctx = ctx, .st = st};
    size_t cap = 0;
    for (uint64_t p = 0; p + 0x20 <= meta_size;) {
        uint64_t data, size;
        uint32_t nlen;
        memcpy(&data, meta + p + 8, 8), memcpy(&size, meta + p + 16, 8), memcpy(&nlen, meta + p + 28, 4);
        if (p + 0x20 + nlen > meta_size) break;
        char *name = (char *)meta + p + 0x20; /* NUL-terminated by the padding, or by the next entry's parent */
        char *nm = strndup(name, nlen);
        st->files++;
        int bundle;
        if (candidate(nm, &bundle) && size >= 48) {
            if ((size_t)s.nf == cap) cap = cap ? cap * 2 : 4096, s.f = realloc(s.f, cap * sizeof *s.f);
            s.f[s.nf++] = (RFile){data_off + data, size, bundle, nm};
        } else {
            free(nm);
        }
        p += 0x20 + ((nlen + 3) & ~3u);
    }
    free(meta);
    st->candidates = (unsigned)s.nf;
    s.later = calloc((size_t)s.nf + 1, sizeof *s.later);
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 8) nthreads = 8;
    pthread_t th[8];
    int started = 0;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8u << 20); /* the callback may run the translator */
    for (int i = 0; i < nthreads; i++)
        if (!pthread_create(&th[started], &at, scan_worker, &s)) {
            pthread_setname_np(th[started], "som-prep");
            started++;
        }
    pthread_attr_destroy(&at);
    if (!started) scan_worker(&s);
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < s.nlater; i++) {
        unsigned n = 0;
        if (file_extract(fd, s.later[i]->off, s.later[i]->size, fn, ctx, &n) < 0)
            som_log("unity scan: %s needs a Shader type tree and no bundle had one", s.later[i]->name);
        st->stages += n;
    }
    for (int i = 0; i < s.nf; i++) free((char *)s.f[i].name);
    free(s.f), free(s.later);
    close(fd);
    return 0;
}

int us_scan_nso(const char *nso_path, us_stage_fn fn, void *ctx) {
    int fd = open(nso_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat sb;
    fstat(fd, &sb);
    uint8_t *raw = malloc((size_t)sb.st_size);
    int ok = raw && pread_all(fd, raw, (size_t)sb.st_size, 0);
    close(fd);
    if (!ok || sb.st_size < 0x100 || memcmp(raw, "NSO0", 4)) {
        free(raw);
        return -1;
    }
    uint32_t flags, off, dec, csz;
    memcpy(&flags, raw + 0xC, 4), memcpy(&off, raw + 0x20, 4), memcpy(&dec, raw + 0x28, 4), memcpy(&csz, raw + 0x64, 4);
    int n = -1;
    if ((uint64_t)off + csz <= (uint64_t)sb.st_size) {
        uint8_t *seg = raw + off;
        uint8_t *buf = NULL;
        if (flags & 2) {
            buf = malloc(dec);
            if (buf && som_lz4_decompress(raw + off, csz, buf, dec) == (int)dec) seg = buf;
            else seg = NULL;
        }
        if (seg) n = (int)stages_in(seg, (flags & 2) ? dec : csz, fn, ctx);
        free(buf);
    }
    free(raw);
    return n;
}
