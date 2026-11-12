/* Rewrites the instructions the host kernel cannot run as the Switch kernel
 * would. Each one becomes a direct branch to a small stub in an "island" near
 * the code (a B reaches +-128 MiB), and the stub returns with another B:
 *
 *   svc #n               -> save context, call our handler, restore
 *   mrs xN, tpidr_el0    -> load the guest's TLS pointer from the thread context
 *   mrs xN, tpidrro_el0  -> load the thread-local-region pointer from the context
 *   msr tpidr_el0, xN    -> store into the context
 *
 * While guest code runs, the real tpidr_el0 holds a pointer to the GuestCtx. */

#include <pthread.h>
#include <string.h>
#include <sys/mman.h>

#include "debug_log.h"
#include "guest.h"

#define NOP 0xD503201Fu
#define MAX_DIST ((1ll << 27) - 0x10000) /* stay clear of the +-128 MiB limit */

int island_init(Island *isl, uint64_t addr, size_t cap) {
    void *p = mmap((void *)addr, cap, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if ((uint64_t)p != addr) {
        som_log("island: mmap at 0x%llx failed", (unsigned long long)addr);
        return -1;
    }
    isl->addr = addr;
    isl->mem = p;
    isl->cap = cap;
    /* copy the svc entry routine; it is position independent */
    size_t blob = (size_t)((const uint8_t *)som_svc_common_end - (const uint8_t *)som_svc_common_start);
    memcpy(isl->mem, som_svc_common_start, blob);
    isl->used = (blob + 15) & ~(size_t)15;
    return 0;
}

void island_seal(Island *isl) {
    __builtin___clear_cache((char *)isl->mem, (char *)isl->mem + isl->used);
    mprotect(isl->mem, isl->cap, PROT_READ | PROT_EXEC);
}

static uint32_t *isl_alloc(Island *isl, size_t ninsn) {
    size_t bytes = (ninsn * 4 + 15) & ~(size_t)15;
    if (isl->used + bytes > isl->cap) return NULL;
    uint32_t *p = (uint32_t *)(isl->mem + isl->used);
    isl->used += bytes;
    return p;
}

static uint32_t enc_b(uint64_t from, uint64_t to, int link) {
    int64_t d = ((int64_t)to - (int64_t)from) >> 2;
    return (link ? 0x94000000u : 0x14000000u) | ((uint32_t)d & 0x03FFFFFFu);
}

static Island *pick_island(Island *isl, int n, uint64_t site) {
    Island *best = NULL;
    int64_t best_d = 0;
    for (int i = 0; i < n; i++) {
        int64_t d = (int64_t)isl[i].addr - (int64_t)site;
        if (d < 0) d = -d;
        if (!best || d < best_d) {
            best = &isl[i];
            best_d = d;
        }
    }
    if (best_d > MAX_DIST) return NULL;
    return best;
}

/* A host-call stub that is not tied to a patched site: the guest calls its address. Created after the islands are
 * sealed, so the page is made writable for the moment. Returns the stub address, or 0. */
uint64_t island_hook_stub(Island *isl, unsigned idx) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    uint32_t *stub = isl_alloc(isl, 8);
    if (!stub) {
        pthread_mutex_unlock(&lock);
        return 0;
    }
    uint64_t sa = isl->addr + (uint64_t)((uint8_t *)stub - isl->mem);
    uint64_t page = (uint64_t)stub & ~0xFFFull;
    mprotect((void *)page, 0x2000, PROT_READ | PROT_WRITE);
    size_t k = 0;
    stub[k++] = 0xA9BF7BFDu;                                /* stp x29, x30, [sp, #-16]! */
    stub[k++] = 0xD2800000u | ((0x8000u | idx) << 5) | 29;  /* movz x29, #(0x8000 | idx) */
    stub[k] = enc_b(sa + k * 4, isl->addr, 1);              /* bl som_svc_common */
    k++;
    stub[k++] = 0xA8C17BFDu;                                /* ldp x29, x30, [sp], #16 */
    stub[k++] = 0xD65F03C0u;                                /* ret */
    while (k < 8) stub[k++] = NOP;
    __builtin___clear_cache((char *)stub, (char *)(stub + 8));
    mprotect((void *)page, 0x2000, PROT_READ | PROT_EXEC);
    pthread_mutex_unlock(&lock);
    return sa;
}

int patch_hook(uint64_t site, unsigned idx, Island *isl, int nisl) {
    Island *is = pick_island(isl, nisl, site);
    uint32_t *stub = is ? isl_alloc(is, 8) : NULL;
    if (!stub) return 0;
    uint64_t sa = is->addr + (uint64_t)((uint8_t *)stub - is->mem);
    size_t k = 0;
    stub[k++] = 0xA9BF7BFDu;                                   /* stp x29, x30, [sp, #-16]! */
    stub[k++] = 0xD2800000u | ((0x8000u | idx) << 5) | 29;     /* movz x29, #(0x8000 | idx) */
    stub[k] = enc_b(sa + k * 4, is->addr, 1);                  /* bl som_svc_common */
    k++;
    stub[k++] = 0xA8C17BFDu;                                   /* ldp x29, x30, [sp], #16 */
    stub[k++] = 0xD65F03C0u;                                   /* ret: back to the guest caller */
    while (k < 8) stub[k++] = NOP;
    *(uint32_t *)site = enc_b(site, sa, 0);
    return 1;
}

/* A pass-through probe (debugging): the instruction at site becomes a branch to a stub that calls hook idx with the
 * full guest context (the host function only reads it), then runs the displaced instruction and continues at
 * site + 4. The displaced instruction is copied as is unless it is PC-relative: B/BL and the conditional branches
 * (B.cond, CBZ/CBNZ, TBZ/TBNZ) are re-targeted; ADR/ADRP/LDR-literal sites are refused (returns 0). */
int patch_probe(uint64_t site, unsigned idx, Island *isl, int nisl) {
    uint32_t w = *(const uint32_t *)site;
    uint32_t top8 = w >> 24;
    int is_b = (w >> 26) == 0x05, is_bl = (w >> 26) == 0x25;
    int is_bcond = (w & 0xFF000010u) == 0x54000000u;
    int is_cb = (top8 & 0x7E) == 0x34, is_tb = (top8 & 0x7E) == 0x36;
    int is_adr = (w & 0x1F000000u) == 0x10000000u;
    int is_ldrlit = (w & 0x3B000000u) == 0x18000000u;
    if (is_adr || is_ldrlit) return 0;
    Island *is = pick_island(isl, nisl, site);
    uint32_t *stub = is ? isl_alloc(is, 8) : NULL;
    if (!stub) return 0;
    uint64_t sa = is->addr + (uint64_t)((uint8_t *)stub - is->mem);
    size_t k = 0;
    stub[k++] = 0xA9BF7BFDu;                               /* stp x29, x30, [sp, #-16]! */
    stub[k++] = 0xD2800000u | ((0x8000u | idx) << 5) | 29; /* movz x29, #(0x8000 | idx) */
    stub[k] = enc_b(sa + k * 4, is->addr, 1);              /* bl som_svc_common */
    k++;
    stub[k++] = 0xA8C17BFDu;                               /* ldp x29, x30, [sp], #16 */
    if (is_b || is_bl) {
        int64_t d = (int64_t)(w & 0x03FFFFFFu) << 38 >> 36; /* sign-extended imm26 * 4 */
        uint64_t target = site + (uint64_t)d;
        stub[k] = enc_b(sa + k * 4, target, is_bl);
        k++;
        if (is_bl) {
            stub[k] = enc_b(sa + k * 4, site + 4, 0);
            k++;
        }
    } else if (is_bcond || is_cb || is_tb) {
        /* taken: skip to the "b target" two words on; not taken: "b site+4" */
        int64_t d;
        if (is_tb) d = (int64_t)((w >> 5) & 0x3FFFu) << 50 >> 48;
        else d = (int64_t)((w >> 5) & 0x7FFFFu) << 45 >> 43;
        uint64_t target = site + (uint64_t)d;
        if (is_tb) stub[k++] = (w & ~(0x3FFFu << 5)) | (2u << 5);
        else stub[k++] = (w & ~(0x7FFFFu << 5)) | (2u << 5);
        stub[k] = enc_b(sa + k * 4, site + 4, 0);
        k++;
        stub[k] = enc_b(sa + k * 4, target, 0);
        k++;
    } else {
        stub[k++] = w;
        stub[k] = enc_b(sa + k * 4, site + 4, 0);
        k++;
    }
    while (k < 8) stub[k++] = NOP;
    *(uint32_t *)site = enc_b(site, sa, 0);
    return 1;
}

int patch_insert(uint64_t site, const uint32_t *w, unsigned n, Island *isl, int nisl) {
    if (n == 0 || n > 7) return 0;
    Island *is = pick_island(isl, nisl, site);
    uint32_t *stub = is ? isl_alloc(is, 8) : NULL;
    if (!stub) return 0;
    uint64_t sa = is->addr + (uint64_t)((uint8_t *)stub - is->mem);
    unsigned k = 0;
    for (; k < n; k++) stub[k] = w[k];
    stub[k] = enc_b(sa + k * 4, site + 4, 0);
    for (k++; k < 8; k++) stub[k] = NOP;
    *(uint32_t *)site = enc_b(site, sa, 0);
    return 1;
}

void patch_text(uint64_t text, size_t size, Island *isl, int nisl, PatchStats *st) {
    uint32_t *code = (uint32_t *)text;
    size_t n = size / 4;
    for (size_t i = 0; i < n; i++) {
        uint32_t w = code[i];
        uint64_t site = text + i * 4;
        uint32_t rt = w & 31;
        if ((w & 0xFFFFFFE0u) == 0xD53BE020u) { /* mrs xN, cntpct_el0: EL0 may only read the virtual counter */
            code[i] = 0xD53BE040u | rt;
            st->cntpct++;
            continue;
        }
        enum { NONE, SVC, MRS_TP, MRS_RO, MSR_TP } kind = NONE;
        uint32_t imm = 0;

        if ((w & 0xFFE0001Fu) == 0xD4000001u) {
            kind = SVC;
            imm = (w >> 5) & 0xFFFF;
        } else if ((w & 0xFFFFFFE0u) == 0xD53BD040u && rt != 31) {
            kind = MRS_TP;
        } else if ((w & 0xFFFFFFE0u) == 0xD53BD060u && rt != 31) {
            kind = MRS_RO;
        } else if ((w & 0xFFFFFFE0u) == 0xD51BD040u) {
            kind = MSR_TP;
        }
        if (kind == NONE) continue;

        Island *is = pick_island(isl, nisl, site);
        uint32_t *stub = is ? isl_alloc(is, 8) : NULL;
        if (!stub) {
            st->skipped++;
            continue;
        }
        uint64_t sa = is->addr + (uint64_t)((uint8_t *)stub - is->mem);
        uint64_t ret = site + 4;
        size_t k = 0;

        switch (kind) {
        case SVC:
            stub[k++] = 0xA9BF7BFDu;                    /* stp x29, x30, [sp, #-16]! */
            stub[k++] = 0xD2800000u | (imm << 5) | 29;  /* movz x29, #imm */
            stub[k] = enc_b(sa + k * 4, is->addr, 1);   /* bl som_svc_common */
            k++;
            stub[k++] = 0xA8C17BFDu;                    /* ldp x29, x30, [sp], #16 */
            st->svc++;
            break;
        case MRS_TP:
        case MRS_RO: {
            uint32_t off = (kind == MRS_TP ? CTX_GUEST_TPIDR : CTX_TLR) / 8;
            stub[k++] = 0xD53BD040u | rt;                         /* mrs xN, tpidr_el0 (= ctx) */
            stub[k++] = 0xF9400000u | (off << 10) | (rt << 5) | rt; /* ldr xN, [xN, #off] */
            kind == MRS_TP ? st->mrs_tpidr++ : st->mrs_tpidrro++;
            break;
        }
        case MSR_TP: {
            uint32_t s = (rt == 16) ? 17 : 16; /* scratch register */
            uint32_t off = CTX_GUEST_TPIDR / 8;
            stub[k++] = 0xF81F0FE0u | s;                            /* str xS, [sp, #-16]! */
            stub[k++] = 0xD53BD040u | s;                            /* mrs xS, tpidr_el0 (= ctx) */
            stub[k++] = 0xF9000000u | (off << 10) | (s << 5) | rt;  /* str xN, [xS, #off] */
            stub[k++] = 0xF84107E0u | s;                            /* ldr xS, [sp], #16 */
            st->msr_tpidr++;
            break;
        }
        default:
            break;
        }
        stub[k] = enc_b(sa + k * 4, ret, 0); /* b back to the instruction after the site */
        k++;
        while (k < 8) stub[k++] = NOP;

        code[i] = enc_b(site, sa, 0); /* the site itself becomes a plain B */
    }
}


