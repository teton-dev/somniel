#pragma once
/* Native guest execution: per-thread context, guest memory map, entry points.
 * Guest virtual addresses equal host virtual addresses. */

#include <stddef.h>
#include <stdint.h>

#include "guest_offsets.h"

typedef struct GuestCtx {
    uint64_t x[31];                     /* 0x000 saved guest x0..x30 at an svc */
    uint64_t sp;                        /* 0x0F8 guest sp at the svc */
    uint64_t nzcv;                      /* 0x100 */
    uint64_t ret_addr;                  /* 0x108 return into the svc stub */
    uint64_t host_sp;                   /* 0x110 host stack the handlers run on */
    uint64_t host_tls;                  /* 0x118 bionic's tpidr_el0 */
    uint64_t guest_tpidr;               /* 0x120 the guest's own tpidr_el0 */
    uint64_t tlr;                       /* 0x128 guest tpidrro_el0 (thread local region) */
    uint64_t svc_num;                   /* 0x130 */
    void (*dispatch)(struct GuestCtx *);/* 0x138 */
    uint64_t callee[20];                /* 0x140 */
    uint64_t pad[2];                    /* 0x1E0 */
    __uint128_t q[32];                  /* 0x1F0 */
    uint64_t call_sp;                   /* 0x3F0 frame of the innermost som_call_guest */
    /* host-only fields below; assembly never touches them */
    uint64_t svc_count;
    int id;
} __attribute__((aligned(16))) GuestCtx;

/* assembly (guest_asm.S) */
void som_enter_guest(GuestCtx *ctx, uint64_t pc, uint64_t sp, uint64_t arg0, uint64_t arg1);
void som_leave_guest(GuestCtx *ctx) __attribute__((noreturn));
uint64_t som_call_guest(GuestCtx *ctx, uint64_t fn, uint64_t sp, const uint64_t *args, uint64_t ret_stub);
void som_call_return(GuestCtx *ctx) __attribute__((noreturn));
void som_crash_shim(int sig, void *info, void *uctx);
void som_suspend_shim(int sig, void *info, void *uctx);
#define SOM_SIG_SUSPEND 39 /* real-time signal used to pause guest threads */
extern const uint32_t som_svc_common_start[];
extern const uint32_t som_svc_common_end[];

/* contexts live in one pool so the crash shim can recognise them */
extern uint64_t som_ctx_pool_lo __attribute__((visibility("hidden")));
extern uint64_t som_ctx_pool_hi __attribute__((visibility("hidden")));
GuestCtx *guest_ctx_alloc(void);

/* guest address-space map, as reported by svc QueryMemory */
enum { MT_FREE = 0, MT_CODE_STATIC = 3, MT_CODE_MUTABLE = 4, MT_HEAP = 5,
       MT_MAPPED = 0xB, MT_TLS = 0xC };
enum { PERM_R = 1, PERM_W = 2, PERM_X = 4 };

typedef struct MemInfo {
    uint64_t base, size;
    uint32_t type, attr, perm, ipc_ref, dev_ref, pad;
} MemInfo;

void guest_region_set(uint64_t base, uint64_t size, uint32_t type, uint32_t perm);
/* Bumped whenever guest memory can stop being readable (unmap, protection change, heap shrink). The dual-screen host
 * caches validated regions per thread and drops them when this changes. Additions need no bump. */
extern volatile uint64_t g_map_epoch;
static inline void guest_map_bump(void) { __atomic_fetch_add(&g_map_epoch, 1, __ATOMIC_RELEASE); }
void guest_query(uint64_t addr, MemInfo *out);

/* address-space layout reported to the guest through svc GetInfo */
/* The guest window starts at Horizon's 0x8000000 whenever that range is free (always on the Adreno devices, where this
 * is exactly the layout that has always been used). ART maps its Java heap, large-object space and JIT caches below
 * 4 GiB at addresses that vary by device (Galaxy A17, Mali-G68, Android 16: 0x2000000-0x12000000), and a module cannot
 * be mapped over them; guest_layout_init() then moves the window to 4 GiB, where nothing is mapped. It runs once at the
 * top of spike_start(), before any of the three variables below is read. */
extern uint64_t g_as_base, g_aslr_size, g_nro_first_base;
void guest_layout_init(void);

/* Guest system ticks run at the Switch's 19.2 MHz: nn::os::GetSystemTickFrequency is the constant 19200000, while every
 * tick the SDK reads (mrs cntpct_el0, svc GetSystemTick) came straight from the host counter. Snapdragon counts at
 * 19.2 MHz too; the Exynos 1330 counts at 26 MHz, so the game's clock ran 1.35x fast there (movies dropped two thirds
 * of their frames and resynced every second). guest = host * g_tick_hi + (host * g_tick_frac) >> 64, which is exactly
 * the host value (hi 1, frac 0) where the host already counts at 19.2 MHz. som_tick_init() runs before patching. */
extern uint64_t g_tick_hi, g_tick_frac;
void som_tick_init(void);
uint64_t som_guest_tick(void);
#define AS_BASE        g_as_base
#define ASLR_BASE      g_as_base
#define ASLR_SIZE      g_aslr_size             /* up to 64 GiB: ends at ALIAS_BASE */
#define ALIAS_BASE     0x0000001000000000ull
#define ALIAS_SIZE     0x0000001000000000ull
#define HEAP_BASE      0x0000002000000000ull
#define HEAP_REGION    0x0000001000000000ull
#define STACK_BASE     0x0000003000000000ull
#define STACK_REGION   0x0000000100000000ull
#define AS_END         0x0000004000000000ull

/* svc handling (svc.c) */
void som_svc_dispatch(GuestCtx *ctx);
void som_register_module(const char *name, uint64_t base, uint64_t size);
const char *som_module_of(uint64_t pc, uint64_t *off);
/* log, dump the guest registers and frame chain, and end the calling guest thread */
void som_stop(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void guest_region_clear(uint64_t base);
void som_install_crash_handlers(void);
const char *som_stop_reason(void);

/* NSO loading (nso_loader.c) */
typedef struct NsoModule {
    char name[16];
    uint64_t base;
    uint32_t text_off, text_size, ro_off, ro_size, data_off, data_size, bss_size;
    uint32_t image_size;
    uint8_t build_id[32];
} NsoModule;

int nso_load(const char *path, const char *name, uint64_t base, NsoModule *out);
int nso_peek_size(const char *path, uint32_t *image_size);

/* patching (patcher.c) */
typedef struct PatchStats {
    unsigned svc, mrs_tpidr, mrs_tpidrro, msr_tpidr, cntpct, skipped;
} PatchStats;

typedef struct Island {
    uint64_t addr;
    uint8_t *mem;
    size_t used, cap;
} Island;

int island_init(Island *isl, uint64_t addr, size_t cap);
void island_seal(Island *isl);
void patch_text(uint64_t text, size_t size, Island *isl, int nisl, PatchStats *st);
/* replace the first instruction of the function at site with a branch to a host-call stub; 0 on failure */
int patch_hook(uint64_t site, unsigned idx, Island *isl, int nisl);
int patch_probe(uint64_t site, unsigned idx, Island *isl, int nisl); /* debugging: pass-through, see patcher.c */
/* replace the instruction at site by n instructions (no PC-relative ones) run from an island, then continue at site+4 */
int patch_insert(uint64_t site, const uint32_t *w, unsigned n, Island *isl, int nisl);
void game_patches_apply(const NsoModule *mods, int nmods, Island *isl, int nisl); /* game_patches.c */
uint64_t island_hook_stub(Island *isl, unsigned idx);

/* running the guest (spike.c) */
int spike_start(const char *exefs_dir);




