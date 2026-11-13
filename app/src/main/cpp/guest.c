#include "guest.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

_Static_assert(offsetof(GuestCtx, x) == CTX_X, "CTX_X");
_Static_assert(offsetof(GuestCtx, sp) == CTX_SP, "CTX_SP");
_Static_assert(offsetof(GuestCtx, nzcv) == CTX_NZCV, "CTX_NZCV");
_Static_assert(offsetof(GuestCtx, ret_addr) == CTX_RET, "CTX_RET");
_Static_assert(offsetof(GuestCtx, host_sp) == CTX_HOST_SP, "CTX_HOST_SP");
_Static_assert(offsetof(GuestCtx, host_tls) == CTX_HOST_TLS, "CTX_HOST_TLS");
_Static_assert(offsetof(GuestCtx, guest_tpidr) == CTX_GUEST_TPIDR, "CTX_GUEST_TPIDR");
_Static_assert(offsetof(GuestCtx, tlr) == CTX_TLR, "CTX_TLR");
_Static_assert(offsetof(GuestCtx, svc_num) == CTX_SVC, "CTX_SVC");
_Static_assert(offsetof(GuestCtx, dispatch) == CTX_DISPATCH, "CTX_DISPATCH");
_Static_assert(offsetof(GuestCtx, callee) == CTX_CALLEE, "CTX_CALLEE");
_Static_assert(offsetof(GuestCtx, q) == CTX_Q, "CTX_Q");
_Static_assert(offsetof(GuestCtx, call_sp) == CTX_CALL_SP, "CTX_CALL_SP");

extern void som_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern const char *som_data_dir(void);

/* Horizon layout by default; see guest.h. NRO modules (Cobalt) are placed 0x38000000 above the base on that layout. */
uint64_t g_as_base = 0x08000000ull;
uint64_t g_aslr_size = ALIAS_BASE - 0x08000000ull;
uint64_t g_nro_first_base = 0x40000000ull;

void guest_layout_init(void) {
    static int done;
    if (done) return;
    done = 1;
    /* stub island A (0x6000000), the modules (~127 MiB from 0x8000000) and island B, with room for a Cobalt subsdk9 */
    const uint64_t lo = 0x06000000ull, hi = 0x11800000ull;
    void *p = mmap((void *)lo, hi - lo, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    if (p != MAP_FAILED) munmap(p, hi - lo); /* free (p == lo), or an old kernel that treated the address as a hint */
    /* the low range is free on the Adreno devices and was taken on the Galaxy A17 (one launch seen), so each device only
     * exercises one path: <data>/guest_window_high forces the 4 GiB window for testing it anywhere */
    char flag[600];
    snprintf(flag, sizeof flag, "%s/guest_window_high", som_data_dir());
    int forced = access(flag, F_OK) == 0;
    if (p == (void *)lo && !forced) {
        som_log("guest layout: Horizon window at 0x%llx", (unsigned long long)g_as_base);
        return;
    }
    g_as_base = 0x100000000ull;
    g_aslr_size = ALIAS_BASE - g_as_base;
    g_nro_first_base = g_as_base + 0x40000000ull;
    som_log("guest layout: 0x%llx-0x%llx is %s: window moved to 0x%llx", (unsigned long long)lo, (unsigned long long)hi,
            forced ? "forced off by guest_window_high" : "taken on this device (ART heap)", (unsigned long long)g_as_base);
}

uint64_t g_tick_hi = 1, g_tick_frac = 0;

void som_tick_init(void) {
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    const uint64_t guest = 19200000;
    if (!freq || freq == guest) {
        som_log("ticks: host counter %llu Hz: guest ticks are the host's (the Switch's 19.2 MHz)", (unsigned long long)freq);
        return;
    }
    g_tick_hi = guest / freq;
    g_tick_frac = (uint64_t)(((unsigned __int128)(guest % freq) << 64) / freq);
    som_log("ticks: host counter %llu Hz: guest ticks scaled to 19.2 MHz (x %.6f)", (unsigned long long)freq,
            (double)guest / (double)freq);
}

uint64_t som_guest_tick(void) {
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v * g_tick_hi + (uint64_t)(((unsigned __int128)v * g_tick_frac) >> 64);
}

#define MAX_CTX 512
uint64_t som_ctx_pool_lo, som_ctx_pool_hi;
static GuestCtx *g_pool;
static int g_pool_used;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;

GuestCtx *guest_ctx_alloc(void) {
    pthread_mutex_lock(&g_pool_lock);
    if (!g_pool) {
        size_t sz = sizeof(GuestCtx) * MAX_CTX;
        g_pool = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_pool == MAP_FAILED) {
            g_pool = NULL;
            pthread_mutex_unlock(&g_pool_lock);
            return NULL;
        }
        som_ctx_pool_lo = (uint64_t)g_pool;
        som_ctx_pool_hi = (uint64_t)g_pool + sz;
    }
    GuestCtx *c = NULL;
    if (g_pool_used < MAX_CTX) {
        c = &g_pool[g_pool_used];
        c->id = g_pool_used++;
    }
    pthread_mutex_unlock(&g_pool_lock);
    return c;
}

/* ---- region map ---- */

#define MAX_REGIONS 256
static MemInfo g_regions[MAX_REGIONS];
static int g_nregions;
static pthread_mutex_t g_region_lock = PTHREAD_MUTEX_INITIALIZER;

void guest_region_set(uint64_t base, uint64_t size, uint32_t type, uint32_t perm) {
    pthread_mutex_lock(&g_region_lock);
    int i;
    for (i = 0; i < g_nregions; i++)
        if (g_regions[i].base == base) break;
    if (i == g_nregions) {
        if (g_nregions == MAX_REGIONS) {
            pthread_mutex_unlock(&g_region_lock);
            return;
        }
        /* keep sorted by base */
        i = g_nregions;
        while (i > 0 && g_regions[i - 1].base > base) {
            g_regions[i] = g_regions[i - 1];
            i--;
        }
        g_nregions++;
    }
    memset(&g_regions[i], 0, sizeof g_regions[i]);
    g_regions[i].base = base;
    g_regions[i].size = size;
    g_regions[i].type = type;
    g_regions[i].perm = perm;
    pthread_mutex_unlock(&g_region_lock);
}

void guest_region_clear(uint64_t base) {
    pthread_mutex_lock(&g_region_lock);
    for (int i = 0; i < g_nregions; i++)
        if (g_regions[i].base == base) {
            memmove(&g_regions[i], &g_regions[i + 1], (size_t)(g_nregions - i - 1) * sizeof g_regions[0]);
            g_nregions--;
            break;
        }
    pthread_mutex_unlock(&g_region_lock);
}

int alias_run(uint64_t addr, uint64_t *base, uint64_t *size, int *mapped);

void guest_query(uint64_t addr, MemInfo *out) {
    uint64_t ab, as;
    int am;
    if (addr >= AS_END) { /* like Horizon: one Inaccessible block that wraps to 0 (scan loops stop on it) */
        memset(out, 0, sizeof *out);
        out->base = AS_END;
        out->size = 0 - AS_END;
        out->type = 0x10;
        return;
    }
    if (alias_run(addr, &ab, &as, &am)) {
        memset(out, 0, sizeof *out);
        out->base = ab;
        out->size = as;
        out->type = am ? MT_HEAP : MT_FREE;
        out->perm = am ? (PERM_R | PERM_W) : 0;
        return;
    }
    pthread_mutex_lock(&g_region_lock);
    uint64_t gap_start = 0;
    memset(out, 0, sizeof *out);
    for (int i = 0; i < g_nregions; i++) {
        const MemInfo *r = &g_regions[i];
        if (addr < r->base) {
            out->base = gap_start;
            out->size = r->base - gap_start;
            out->type = MT_FREE;
            pthread_mutex_unlock(&g_region_lock);
            return;
        }
        if (addr < r->base + r->size) {
            *out = *r;
            pthread_mutex_unlock(&g_region_lock);
            return;
        }
        gap_start = r->base + r->size;
    }
    out->base = gap_start;
    out->size = (AS_END > gap_start ? AS_END : addr + 1) - gap_start;
    out->type = MT_FREE;
    pthread_mutex_unlock(&g_region_lock);
}


