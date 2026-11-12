/* Fragment shader rewrites for the Adreno, on the translated SPIR-V before vkCreateShaderModule.
 * <data>/fs_opt holds a number (read once per run):
 *  1  drop NoContraction: the translator marks nearly every float multiply and add (Maxwell rounds each separately);
 *     that stops the driver fusing them into FMAs. Fragment shaders only: vertex positions keep it, since a depth
 *     pre-pass and the main pass must compute bit-identical depths.
 *  2  RelaxedPrecision (FP16, double rate on the Adreno) on float math that does not reach a texture coordinate or
 *     LOD/offset/depth-compare operand, a float-to-int conversion or bitcast, or the depth output. The analysis walks
 *     backwards from those uses, through function variables (the translator's registers): a variable that ever feeds
 *     one stays full precision for every value stored in it. Marking too much as full precision is always safe.
 *  3  both. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../debug_log.h"

extern const char *som_data_dir(void);

enum {
    OpExtInst = 12, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32, OpFunction = 54, OpVariable = 59, OpLoad = 61,
    OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71, OpVectorShuffle = 79,
    OpCompositeConstruct = 80, OpCompositeExtract = 81, OpConvertFToU = 109, OpConvertFToS = 110, OpBitcast = 124,
    OpFNegate = 127, OpFAdd = 129, OpFSub = 131, OpFMul = 133, OpFDiv = 136, OpFRem = 138, OpFMod = 141,
    OpVectorTimesScalar = 142, OpDot = 148, OpSelect = 169, OpPhi = 245,
};
enum { DecRelaxedPrecision = 0, DecBuiltIn = 11, DecNoContraction = 42, BuiltInFragDepth = 22 };

int g_fs_opt = -1;

int spv_fs_opt_mode(void) {
    if (g_fs_opt >= 0) return g_fs_opt;
    char p[600];
    snprintf(p, sizeof p, "%s/fs_opt", som_data_dir());
    FILE *f = fopen(p, "r");
    int v = 0;
    if (f) {
        if (fscanf(f, "%d", &v) != 1) v = 0;
        fclose(f);
    }
    g_fs_opt = v & 3;
    som_log("spv: fragment shader rewrites %d (1 fused multiply-add, 2 FP16 math)", g_fs_opt);
    return g_fs_opt;
}

static int is_sample(unsigned op) { return op >= 87 && op <= 97; } /* OpImageSample* .. OpImageDrefGather */

/* Returns the rewritten module (malloc'd, *n bytes) or NULL to keep the original. */
uint32_t *spv_fs_opt(const uint32_t *w, size_t *n_bytes, const char *key) {
    int mode = spv_fs_opt_mode();
    size_t nw = *n_bytes / 4;
    if (!mode || nw < 5 || w[0] != 0x07230203) return NULL;
    uint32_t bound = w[3];
    if (bound > (1u << 22)) return NULL;
    /* per id: defining instruction offset, kinds */
    uint32_t *def = calloc(bound, sizeof *def);
    uint8_t *isfloat = calloc(bound, 1);  /* type id: float32 scalar or vector */
    uint8_t *ptrfunc = calloc(bound, 1);  /* variable in Function storage */
    uint32_t *base = calloc(bound, sizeof *base); /* pointer id -> its variable */
    uint8_t *hi = calloc(bound, 1);       /* value or variable that must stay full precision */
    uint8_t *relax = calloc(bound, 1);
    uint32_t *work = malloc(bound * 4 * sizeof *work);
    uint32_t *out = malloc((nw + bound * 3 + 16) * 4);
    if (!def || !isfloat || !ptrfunc || !base || !hi || !relax || !work || !out) goto fail;
    size_t nwork = 0;
#define MARK(id) do { uint32_t m_ = (id); if (m_ && m_ < bound && !hi[m_]) { hi[m_] = 1; work[nwork++] = m_; } } while (0)
    uint32_t fragdepth = 0;
    int in_func = 0;
    size_t i;
    /* pass 1: definitions, types, variables, roots */
    for (i = 5; i < nw;) {
        unsigned op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > nw) goto fail;
        const uint32_t *a = &w[i + 1];
        switch (op) {
        case OpTypeFloat: if (a[1] == 32) isfloat[a[0]] = 1; break;
        case OpTypeVector: if (a[1] < bound && isfloat[a[1]]) isfloat[a[0]] = 1; break;
        case OpVariable: if (a[1] < bound) { def[a[1]] = (uint32_t)i; base[a[1]] = a[1]; if (a[2] == 7) ptrfunc[a[1]] = 1; } break;
        case OpAccessChain: case OpInBoundsAccessChain:
            if (a[1] < bound && a[2] < bound) { def[a[1]] = (uint32_t)i; base[a[1]] = base[a[2]] ? base[a[2]] : a[2]; }
            break;
        case OpDecorate:
            if (wc >= 4 && a[1] == DecBuiltIn && a[2] == BuiltInFragDepth) fragdepth = a[0];
            break;
        default:
            break;
        }
        /* value instructions inside functions (result type, result id): the ones whose second word is not a result id
         * (store, branches, merges, image writes, barriers) are left out, so no real definition is shadowed */
        if (op == OpFunction) in_func = 1;
        if (in_func && op != OpVariable && op != OpAccessChain && op != OpInBoundsAccessChain && wc >= 3 && op != OpStore &&
            op != OpFunction && !(op >= 246 && op <= 251) && op != 99 /* ImageWrite */ && op != 224 /* ControlBarrier */ &&
            op != 225 /* MemoryBarrier */ && a[1] < bound && !def[a[1]])
            def[a[1]] = (uint32_t)i;
        i += wc;
    }
    /* pass 2: roots */
    for (i = 5; i < nw; i += w[i] >> 16) {
        unsigned op = w[i] & 0xffff, wc = w[i] >> 16;
        const uint32_t *a = &w[i + 1];
        if (is_sample(op)) for (unsigned k = 3; k < wc - 1; k++) MARK(a[k]); /* coordinate, dref, image operands */
        else if (op == OpConvertFToU || op == OpConvertFToS || op == OpBitcast) MARK(a[2]);
        else if (op == OpStore && fragdepth && a[0] < bound && (a[0] == fragdepth || base[a[0]] == fragdepth)) MARK(a[1]);
    }
    /* pass 3: backwards */
    while (nwork) {
        uint32_t id = work[--nwork];
        uint32_t d = def[id];
        if (!d) continue;
        unsigned op = w[d] & 0xffff, wc = w[d] >> 16;
        const uint32_t *a = &w[d + 1];
        if (op == OpVariable) {
            /* every value stored into it (directly or through an access chain) */
            for (size_t j = 5; j < nw; j += w[j] >> 16)
                if ((w[j] & 0xffff) == OpStore && w[j + 1] < bound && (w[j + 1] == id || base[w[j + 1]] == id)) MARK(w[j + 2]);
        } else if (op == OpLoad) {
            uint32_t v = a[2] < bound ? (base[a[2]] ? base[a[2]] : a[2]) : 0;
            MARK(v);
        } else if (op == OpAccessChain || op == OpInBoundsAccessChain) {
            MARK(base[id]);
        } else {
            for (unsigned k = 2; k < wc - 1; k++) MARK(a[k]); /* operands (literals among them only over-mark) */
        }
    }
    /* pass 4: what to relax */
    unsigned nrelax = 0, nnc = 0;
    if (mode & 2)
        for (i = 5; i < nw; i += w[i] >> 16) {
            unsigned op = w[i] & 0xffff, wc = w[i] >> 16;
            const uint32_t *a = &w[i + 1];
            if (wc < 3) continue;
            int ok = 0;
            switch (op) {
            case OpFNegate: case OpFAdd: case OpFSub: case OpFMul: case OpFDiv: case OpFRem: case OpFMod: case OpVectorTimesScalar:
            case OpDot: case OpExtInst: case OpVectorShuffle: case OpCompositeConstruct: case OpCompositeExtract: case OpSelect: case OpPhi:
                ok = 1;
                break;
            case OpLoad: ok = a[2] < bound && ptrfunc[base[a[2]] ? base[a[2]] : a[2]]; break;
            default: break;
            }
            if (ok && a[0] < bound && isfloat[a[0]] && a[1] < bound && !hi[a[1]]) relax[a[1]] = 1, nrelax++;
        }
    /* pass 5: write; decorations go after the last annotation before the first type */
    size_t o = 0, ins = 0;
    memcpy(out, w, 5 * 4);
    o = 5;
    for (i = 5; i < nw; i += w[i] >> 16) {
        unsigned op = w[i] & 0xffff, wc = w[i] >> 16;
        if ((mode & 1) && op == OpDecorate && wc == 3 && w[i + 2] == DecNoContraction) { nnc++; continue; }
        if (!ins && (op == 19 || (op >= 20 && op <= 39) || op == 43 || op == 41 || op == 42 || op == OpVariable)) {
            for (uint32_t id = 1; id < bound; id++)
                if (relax[id]) out[o++] = (3u << 16) | OpDecorate, out[o++] = id, out[o++] = DecRelaxedPrecision;
            ins = 1;
        }
        memcpy(&out[o], &w[i], wc * 4);
        o += wc;
    }
    static int nlog;
    if (nlog++ < 6) som_log("spv: fs %.16s: %u NoContraction dropped, %u values FP16", key, nnc, nrelax);
    if (nlog <= 3) {
        char p[700];
        snprintf(p, sizeof p, "%s/fsopt_%.16s.spv", som_data_dir(), key);
        FILE *f = fopen(p, "wb");
        if (f) fwrite(out, 4, o, f), fclose(f);
    }
    free(def), free(isfloat), free(ptrfunc), free(base), free(hi), free(relax), free(work);
    *n_bytes = o * 4;
    return out;
fail:
    free(def), free(isfloat), free(ptrfunc), free(base), free(hi), free(relax), free(work), free(out);
    return NULL;
#undef MARK
}
