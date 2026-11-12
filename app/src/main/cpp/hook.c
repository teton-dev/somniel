#include "hook.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "debug_log.h"

#define MAX_HOOKS 4096
#define DT_NULL 0
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_GNU_HASH 0x6FFFFEF5ull

static struct {
    const char *name;
    HostFn fn;
    int installed;
} g_hooks[MAX_HOOKS];
static int g_nhooks;

void hook_register(const char *symbol, HostFn fn) {
    if (g_nhooks < MAX_HOOKS) {
        g_hooks[g_nhooks].name = symbol;
        g_hooks[g_nhooks].fn = fn;
        g_nhooks++;
    }
}

const char *hook_name(unsigned idx) { return idx < (unsigned)g_nhooks ? g_hooks[idx].name : "?"; }

static __thread unsigned t_hook_idx;
const char *hook_current_name(void) { return hook_name(t_hook_idx); }

static Island *g_isl;

/* A host function the guest reaches through a pointer (NVN procs): returns the address to hand out, 0 on failure. */
uint64_t hook_register_dynamic(const char *name, HostFn fn) {
    if (!g_isl || g_nhooks >= MAX_HOOKS) return 0;
    unsigned idx = (unsigned)g_nhooks++;
    g_hooks[idx].name = name;
    g_hooks[idx].fn = fn;
    g_hooks[idx].installed = 1;
    return island_hook_stub(g_isl, idx);
}

void hook_dispatch(unsigned idx, GuestCtx *c) {
    if (idx >= (unsigned)g_nhooks) som_stop("hook index %u out of range", idx);
    t_hook_idx = idx;
    som_trace("t%d hook %s", c->id, g_hooks[idx].name);
    g_hooks[idx].fn(c);
}

static uint32_t rd32(uint64_t a) { return *(const uint32_t *)a; }
static uint64_t rd64(uint64_t a) { return *(const uint64_t *)a; }

void nso_for_each_symbol(const NsoModule *m, SymCallback cb, void *user) {
    uint64_t base = m->base;
    uint64_t mod0 = base + rd32(base + 4);
    if (memcmp((void *)mod0, "MOD0", 4) != 0) return;
    uint64_t dyn = mod0 + (int32_t)rd32(mod0 + 4);
    uint64_t hash = 0, gnu = 0, symtab = 0, strtab = 0;
    for (uint64_t o = dyn;; o += 16) {
        uint64_t tag = rd64(o), val = rd64(o + 8);
        if (tag == DT_NULL) break;
        if (tag == DT_HASH) hash = val;
        else if (tag == DT_GNU_HASH) gnu = val;
        else if (tag == DT_SYMTAB) symtab = val;
        else if (tag == DT_STRTAB) strtab = val;
    }
    if (!symtab || !strtab) return;
    uint32_t n = 0;
    if (hash) {
        n = rd32(base + hash + 4);
    } else if (gnu) {
        uint64_t h = base + gnu;
        uint32_t nb = rd32(h), symoff = rd32(h + 4), bloom = rd32(h + 8);
        uint64_t buckets = h + 16 + (uint64_t)bloom * 8;
        uint32_t last = 0;
        for (uint32_t i = 0; i < nb; i++) {
            uint32_t b = rd32(buckets + 4ull * i);
            if (b > last) last = b;
        }
        if (last < symoff) {
            n = symoff;
        } else {
            uint64_t chains = buckets + 4ull * nb;
            while (!(rd32(chains + 4ull * (last - symoff)) & 1)) last++;
            n = last + 1;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        uint64_t s = base + symtab + 24ull * i;
        uint32_t name = rd32(s);
        uint16_t shndx = *(const uint16_t *)(s + 6);
        uint64_t value = rd64(s + 8), size = rd64(s + 16);
        if (!name || shndx == 0 || !value) continue;
        if (cb((const char *)(base + strtab + name), base + value, size, user)) return;
    }
}

struct FindCtx {
    Island *isl;
    int nisl;
    int found;
    int modules_with_hits;
};

static int on_symbol(const char *name, uint64_t addr, uint64_t size, void *user) {
    struct FindCtx *f = user;
    for (int i = 0; i < g_nhooks; i++) {
        if (g_hooks[i].installed || g_hooks[i].name[0] != name[0] || strcmp(g_hooks[i].name, name) != 0) continue;
        if (!patch_hook(addr, (unsigned)i, f->isl, f->nisl)) {
            som_log("hook: no stub space for %s", name);
            return 0;
        }
        g_hooks[i].installed = 1;
        f->found++;
        som_trace("hook: %s at 0x%llx (size 0x%llx)", name, (unsigned long long)addr, (unsigned long long)size);
        break;
    }
    return 0;
}

static NsoModule g_mods[16];
static int g_nmods;

struct LookupCtx {
    const char *name;
    uint64_t addr;
};

static int on_lookup(const char *name, uint64_t addr, uint64_t size, void *user) {
    struct LookupCtx *l = user;
    (void)size;
    if (strcmp(name, l->name) == 0) {
        l->addr = addr;
        return 1;
    }
    return 0;
}

/* address of an exported symbol in any loaded module, or 0 */
uint64_t module_symbol(const char *name) {
    struct LookupCtx l = {name, 0};
    for (int i = 0; i < g_nmods && !l.addr; i++) nso_for_each_symbol(&g_mods[i], on_lookup, &l);
    return l.addr;
}

/* ---- probes (debugging): <data>/probes.txt, one "module rva name [strN]" per line (e.g. "main 235ee98 TickMind.ret").
 * Each hit logs the thread, x0..x3 and x8 (and xN as an IL2CPP string with "strN"); a hit whose x0..x3 equal the
 * previous hit of the same probe within 300 ms is counted, not logged. Addresses come from Il2CppDumper's dump.cs
 * (RVA) or tools/il2cpp_disasm.py. */
typedef struct {
    uint64_t last[4];
    unsigned same;
    unsigned hits;
    uint64_t last_ns;
    int str_reg; /* >= 0: also print that register as an IL2CPP System.String (length +0x10, UTF-16 from +0x14) */
} ProbeState;
static ProbeState g_probe[MAX_HOOKS];

static uint64_t probe_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void probe_fn(GuestCtx *c) {
    ProbeState *p = &g_probe[t_hook_idx];
    uint64_t now = probe_now_ns();
    p->hits++;
    /* identical registers: counted, and logged again only after 300 ms (per-frame callers stay readable) */
    if (p->hits > 1 && !memcmp(p->last, c->x, sizeof p->last) && now - p->last_ns < 300000000ull) {
        p->same++;
        return;
    }
    char s[200] = "";
    if (p->str_reg >= 0 && c->x[p->str_reg] > 0x10000) {
        const uint8_t *o = (const uint8_t *)(uintptr_t)c->x[p->str_reg];
        int32_t len;
        memcpy(&len, o + 0x10, 4);
        size_t n = 0;
        s[n++] = ' ', s[n++] = '"';
        for (int32_t i = 0; i < len && i < 60 && n < sizeof s - 5; i++) { /* UTF-16 (BMP) to UTF-8 */
            uint16_t ch;
            memcpy(&ch, o + 0x14 + 2 * i, 2);
            if (ch < 0x80) s[n++] = ch >= 32 ? (char)ch : '?';
            else if (ch < 0x800) s[n++] = (char)(0xC0 | ch >> 6), s[n++] = (char)(0x80 | (ch & 0x3F));
            else s[n++] = (char)(0xE0 | ch >> 12), s[n++] = (char)(0x80 | ((ch >> 6) & 0x3F)), s[n++] = (char)(0x80 | (ch & 0x3F));
        }
        s[n++] = '"', s[n] = 0;
    }
    som_log("probe %s t%d x0=%#llx x1=%#llx x2=%#llx x3=%#llx x8=%#llx%s (hit %u, %u same before)", hook_name(t_hook_idx),
            c->id, (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2],
            (unsigned long long)c->x[3], (unsigned long long)c->x[8], s, p->hits, p->same);
    memcpy(p->last, c->x, sizeof p->last);
    p->same = 0;
    p->last_ns = now;
}

int probes_install(const NsoModule *mods, int nmods, Island *isl, int nisl, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256], mod[32], name[128], opt[16];
    unsigned long long rva;
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        opt[0] = 0;
        if (line[0] == '#' || sscanf(line, "%31s %llx %127s %15s", mod, &rva, name, opt) < 3) continue;
        const NsoModule *m = NULL;
        for (int i = 0; i < nmods; i++)
            if (!strcmp(mods[i].name, mod)) m = &mods[i];
        if (!m || g_nhooks >= MAX_HOOKS) {
            som_log("probe: no module %s for %s", mod, name);
            continue;
        }
        unsigned idx = (unsigned)g_nhooks++;
        g_hooks[idx].name = strdup(name);
        g_hooks[idx].fn = probe_fn;
        g_hooks[idx].installed = 1;
        g_probe[idx].str_reg = !strncmp(opt, "str", 3) ? atoi(opt + 3) : -1; /* "str1": x1 is a System.String */
        uint64_t site = m->base + rva;
        if (patch_probe(site, idx, isl, nisl)) n++;
        else som_log("probe: cannot probe %s at %s+%#llx (PC-relative load or no island space)", name, mod, rva);
    }
    fclose(f);
    som_log("probes: %d installed from %s", n, path);
    return n;
}

/* A host function at a guest address. Pass-through (replace == 0): fn sees and may change the guest registers (x0..x30,
 * q0..q31) just before the displaced instruction at site runs; site must not be PC-relative. Replace: site is a function's
 * first instruction, fn's x0/q0 result is returned to the caller. Returns 1 on success. */
int hook_site(uint64_t site, const char *name, HostFn fn, int replace, Island *isl, int nisl) {
    if (g_nhooks >= MAX_HOOKS) return 0;
    unsigned idx = (unsigned)g_nhooks++;
    g_hooks[idx].name = name;
    g_hooks[idx].fn = fn;
    g_hooks[idx].installed = 1;
    return replace ? patch_hook(site, idx, isl, nisl) : patch_probe(site, idx, isl, nisl);
}

int hooks_install(const NsoModule *mods, int nmods, Island *isl, int nisl) {
    g_isl = isl;
    for (int i = 0; i < nmods && i < 16; i++) g_mods[g_nmods++] = mods[i];
    struct FindCtx f = {isl, nisl, 0, 0};
    for (int i = 0; i < nmods; i++) nso_for_each_symbol(&mods[i], on_symbol, &f);
    int missing = 0;
    for (int i = 0; i < g_nhooks; i++)
        if (!g_hooks[i].installed) {
            som_log("hook: symbol not found: %s", g_hooks[i].name);
            missing++;
        }
    som_log("hooks: %d installed, %d symbols not found", f.found, missing);
    return f.found;
}


