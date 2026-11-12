/* nn::ro replacement: loads plugin NROs (Cobalt's mod plugins, skyline-rs cdylibs) into the guest address space.
 *
 * Cobalt calls RegisterModuleInfo (NRR, ignored), GetBufferSize (image + bss size), LoadModule (copy + relocate +
 * bind imports) and LookupModuleSymbol("main"). The image is mapped by the host at a fixed guest address followed by
 * a stub island of its own (the svc / tpidr patcher needs the stubs within +-128 MiB of the code). Imports are
 * resolved against the dynamic symbols of every loaded NSO (module_symbol); an import nobody exports gets a stub
 * that stops the game with its name when called, so a missing function is reported instead of jumping to 0. */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "debug_log.h"
#include "hook.h"

#define NRO_FIRST_BASE 0x40000000ull
#define NRO_STEP       0x02000000ull
#define NRO_ISLAND_SZ  0x00200000ull
#define MAX_NROS       32

#define DT_NULL 0
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_INIT 12
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27

#define R_AARCH64_ABS64 0x101
#define R_AARCH64_GLOB_DAT 0x401
#define R_AARCH64_JUMP_SLOT 0x402
#define R_AARCH64_RELATIVE 0x403

typedef struct {
    uint64_t base;
    uint64_t image_size; /* text+ro+data+bss, page rounded */
    int used;
} LoadedNro;

static LoadedNro g_nro[MAX_NROS];
static pthread_mutex_t g_nro_mu = PTHREAD_MUTEX_INITIALIZER;

static uint32_t rd32(const void *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t page_up(uint64_t v) { return (v + 0xFFF) & ~0xFFFull; }

/* total mapped size an NRO needs: the header's file size rounded up plus its bss */
static uint64_t nro_image_size(const uint8_t *h) {
    if (memcmp(h + 0x10, "NRO0", 4) != 0) return 0;
    uint64_t end = (uint64_t)rd32(h + 0x18);
    uint64_t bss = rd32(h + 0x34);
    return page_up(end) + page_up(bss);
}

static void unresolved_import(GuestCtx *c) {
    (void)c;
    som_stop("NRO import %s is not provided by any loaded module", hook_current_name());
}

/* skyline's own exports, which plugins built for skyline import but Cobalt's subsdk9 does not provide */
static void sky_noop(GuestCtx *c) { (void)c; }                               /* add_nro_(un)load_hook(cb) */
static void sky_program_id(GuestCtx *c) { ret_x(c, 0x0100A6301214E000ull); } /* u64 get_program_id(void) */
static void sky_false(GuestCtx *c) { ret_x(c, 0); } /* bool get_plugin_addresses(addr, *start, *end): not a plugin */

static const struct {
    const char *name;
    HostFn fn;
} kSkylineApi[] = {
    {"add_nro_load_hook", sky_noop},
    {"add_nro_unload_hook", sky_noop},
    {"get_program_id", sky_program_id},
    {"get_plugin_addresses", sky_false},
};

static uint64_t resolve_import(const char *name, uint64_t base) {
    if (strcmp(name, "__nro_header_start") == 0) return base;
    uint64_t a = module_symbol(name);
    if (a) return a;
    for (size_t i = 0; i < sizeof kSkylineApi / sizeof kSkylineApi[0]; i++)
        if (strcmp(name, kSkylineApi[i].name) == 0) {
            static uint64_t addr[sizeof kSkylineApi / sizeof kSkylineApi[0]];
            if (!addr[i]) addr[i] = hook_register_dynamic(kSkylineApi[i].name, kSkylineApi[i].fn);
            return addr[i];
        }
    char *dup = strdup(name);
    a = hook_register_dynamic(dup, unresolved_import);
    som_log("nro: import %s is not provided (stub installed)", name);
    return a;
}

/* nn::Result codes used here: ro module 22 */
#define RO_R_NOT_FOUND 0x216u
#define RO_R_INVALID 0x1a16u

static void ro_get_buffer_size(GuestCtx *c) {
    uint64_t *out = arg_p(c, 0);
    const uint8_t *nro = arg_p(c, 1);
    uint64_t sz = nro ? nro_image_size(nro) : 0;
    if (!sz || !out) {
        ret_x(c, RO_R_INVALID);
        return;
    }
    *out = sz;
    ret_x(c, 0);
}

static void ro_load_module(GuestCtx *c) {
    uint64_t *module = arg_p(c, 0);
    const uint8_t *nro = arg_p(c, 1);
    if (!module || !nro) {
        ret_x(c, RO_R_INVALID);
        return;
    }
    uint64_t size = nro_image_size(nro);
    if (!size) {
        som_log("nro: LoadModule on something that is not an NRO0");
        ret_x(c, RO_R_INVALID);
        return;
    }
    uint32_t seg_off[3], seg_sz[3];
    for (int i = 0; i < 3; i++) {
        seg_off[i] = rd32(nro + 0x20 + 8 * i);
        seg_sz[i] = rd32(nro + 0x24 + 8 * i);
    }

    pthread_mutex_lock(&g_nro_mu);
    int slot = -1;
    for (int i = 0; i < MAX_NROS; i++)
        if (!g_nro[i].used) {
            slot = i;
            break;
        }
    uint64_t base = 0;
    if (slot >= 0) {
        for (uint64_t cand = NRO_FIRST_BASE; cand < NRO_FIRST_BASE + 64 * NRO_STEP; cand += NRO_STEP) {
            void *p = mmap((void *)cand, size + NRO_ISLAND_SZ, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED && (uint64_t)p == cand) {
                base = cand;
                break;
            }
            if (p != MAP_FAILED) munmap(p, size + NRO_ISLAND_SZ);
        }
    }
    if (!base) {
        pthread_mutex_unlock(&g_nro_mu);
        som_log("nro: no address space for an NRO (%s)", slot < 0 ? "table full" : strerror(errno));
        ret_x(c, RO_R_INVALID);
        return;
    }
    g_nro[slot].base = base;
    g_nro[slot].image_size = size;
    g_nro[slot].used = 1;
    pthread_mutex_unlock(&g_nro_mu);

    /* segments: text, ro, data at their header offsets (the file is laid out like the memory image) */
    for (int i = 0; i < 3; i++) memcpy((void *)(base + seg_off[i]), nro + seg_off[i], seg_sz[i]);

    /* dynamic section through MOD0 */
    uint32_t mod_off = rd32(nro + 4);
    const uint8_t *mod0 = (const uint8_t *)(base + mod_off);
    if (memcmp(mod0, "MOD0", 4) != 0) {
        som_log("nro: no MOD0 header");
        ret_x(c, RO_R_INVALID);
        return;
    }
    const uint64_t *dyn = (const uint64_t *)((uint64_t)mod0 + (int64_t)(int32_t)rd32(mod0 + 4));
    uint64_t strtab = 0, symtab = 0, rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0, init = 0, init_arr = 0, init_arrsz = 0;
    for (; dyn[0] != DT_NULL; dyn += 2) {
        switch (dyn[0]) {
        case DT_STRTAB: strtab = dyn[1]; break;
        case DT_SYMTAB: symtab = dyn[1]; break;
        case DT_RELA: rela = dyn[1]; break;
        case DT_RELASZ: relasz = dyn[1]; break;
        case DT_JMPREL: jmprel = dyn[1]; break;
        case DT_PLTRELSZ: pltrelsz = dyn[1]; break;
        case DT_INIT: init = dyn[1]; break;
        case DT_INIT_ARRAY: init_arr = dyn[1]; break;
        case DT_INIT_ARRAYSZ: init_arrsz = dyn[1]; break;
        default: break;
        }
    }

    unsigned n_rel = 0, n_bind = 0, n_bad = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t off = pass == 0 ? rela : jmprel, len = pass == 0 ? relasz : pltrelsz;
        for (uint64_t k = 0; off && k + 24 <= len; k += 24) {
            const uint8_t *r = (const uint8_t *)(base + off + k);
            uint64_t where, info;
            int64_t addend;
            memcpy(&where, r, 8);
            memcpy(&info, r + 8, 8);
            memcpy(&addend, r + 16, 8);
            uint32_t type = (uint32_t)info, sym = (uint32_t)(info >> 32);
            uint64_t *slot_p = (uint64_t *)(base + where);
            if (type == R_AARCH64_RELATIVE) {
                *slot_p = base + (uint64_t)addend;
                n_rel++;
            } else if (type == R_AARCH64_ABS64 || type == R_AARCH64_GLOB_DAT || type == R_AARCH64_JUMP_SLOT) {
                const uint8_t *s = (const uint8_t *)(base + symtab + 24ull * sym);
                uint32_t name_off = rd32(s);
                uint16_t shndx;
                uint64_t value;
                memcpy(&shndx, s + 6, 2);
                memcpy(&value, s + 8, 8);
                uint64_t target;
                const char *sname = (const char *)(base + strtab + name_off);
                if (shndx != 0) {
                    target = base + value; /* defined here */
                } else if ((s[4] >> 4) == 2 /* STB_WEAK */ && strcmp(sname, "__nro_header_start") != 0 &&
                           !(target = module_symbol(sname))) {
                    target = 0; /* weak and absent: the plugin checks for null (skyline's optional APIs) */
                    som_log("nro: weak import %s absent (null)", sname);
                } else {
                    target = resolve_import(sname, base);
                }
                *slot_p = target + (type == R_AARCH64_JUMP_SLOT ? 0 : (uint64_t)addend);
                n_bind++;
            } else {
                n_bad++;
                som_log("nro: unsupported relocation type 0x%x", type);
            }
        }
    }

    /* the svc / tpidr patcher, with a stub island right behind the image (reserved with the image above; island_init
     * maps it itself, so hand the range back first) */
    munmap((void *)(base + size), NRO_ISLAND_SZ);
    Island *isl = calloc(1, sizeof *isl);
    if (island_init(isl, base + size, NRO_ISLAND_SZ) != 0) {
        som_log("nro: island at 0x%llx failed", (unsigned long long)(base + size));
        ret_x(c, RO_R_INVALID);
        return;
    }
    PatchStats st;
    memset(&st, 0, sizeof st);
    patch_text(base + seg_off[0], seg_sz[0], isl, 1, &st);
    island_seal(isl);
    __builtin___clear_cache((char *)(base + seg_off[0]), (char *)(base + seg_off[0] + seg_sz[0]));
    uint64_t text_sz = page_up(seg_sz[0]), ro_sz = page_up(seg_sz[1]);
    mprotect((void *)(base + seg_off[0]), text_sz, PROT_READ | PROT_EXEC);
    mprotect((void *)(base + seg_off[1]), ro_sz, PROT_READ);
    uint64_t data_off = seg_off[2];
    guest_region_set(base + seg_off[0], text_sz, MT_CODE_STATIC, PERM_R | PERM_X);
    guest_region_set(base + seg_off[1], ro_sz, MT_CODE_STATIC, PERM_R);
    guest_region_set(base + data_off, size - data_off, MT_CODE_MUTABLE, PERM_R | PERM_W);
    char name[16];
    snprintf(name, sizeof name, "nro%d", slot);
    som_register_module(name, base, size);

    som_log("nro: loaded at 0x%llx size 0x%llx (relative %u, bound %u, bad %u; svc=%u tpidr=%u skipped=%u)",
            (unsigned long long)base, (unsigned long long)size, n_rel, n_bind, n_bad, st.svc,
            st.mrs_tpidr + st.msr_tpidr, st.skipped);
    if (init_arrsz) som_log("nro: INIT_ARRAY (%llu bytes) is not run", (unsigned long long)init_arrsz);
    (void)init;
    (void)init_arr;

    *module = base;
    ret_x(c, 0);
}

struct SymFind {
    const char *name;
    uint64_t addr;
};

static int on_nro_symbol(const char *name, uint64_t addr, uint64_t size, void *user) {
    struct SymFind *f = user;
    (void)size;
    if (strcmp(name, f->name) == 0) {
        f->addr = addr;
        return 1;
    }
    return 0;
}

static void ro_lookup_module_symbol(GuestCtx *c) {
    uint64_t *out = arg_p(c, 0);
    const uint64_t *module = arg_p(c, 1);
    const char *name = arg_p(c, 2);
    struct SymFind f = {name, 0};
    if (module && name) {
        NsoModule m;
        memset(&m, 0, sizeof m);
        m.base = *module;
        nso_for_each_symbol(&m, on_nro_symbol, &f);
    }
    if (out && f.addr) *out = f.addr;
    ret_x(c, f.addr ? 0 : RO_R_NOT_FOUND);
}

static void ro_unload_module(GuestCtx *c) {
    (void)c;
    som_log("nro: UnloadModule ignored (plugins stay loaded)");
    ret_x(c, 0);
}

void nro_loader_register(void) {
    hook_register("_ZN2nn2ro13GetBufferSizeEPmPKv", ro_get_buffer_size);
    hook_register("_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi", ro_load_module);
    hook_register("_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmib", ro_load_module);
    hook_register("_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc", ro_lookup_module_symbol);
    hook_register("_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE", ro_unload_module);
}
