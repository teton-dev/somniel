/* M1 spike: map the Engage NSOs at fixed addresses, patch them, and run rtld
 * natively on a pthread until the guest makes its first service request. */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "debug_log.h"
#include "guest.h"
#include "hook.h"
#include "ipc.h"
#include "movies.h"
#include "thread.h"
#include "nvn/shader_cache.h"

extern const char *som_data_dir(void);
#ifdef SOM_DSMOD
int dsmod_apply_plan(const NsoModule *mods, int nmods, uint64_t mailbox_hint);
void dsmod_start(void);
#else
#define dsmod_apply_plan(a, b, c) 0
#define dsmod_start() ((void)0)
#endif

#define ISLAND_A   (MODULE_BASE - 0x02000000ull) /* stub islands: before and after the modules (0x6000000 on the Horizon window) */
#define ISLAND_SZ  0x01000000ull
#define MODULE_BASE g_as_base /* the guest window (guest.h): 0x8000000, or 4 GiB where ART occupies the low range */
#define GUEST_STACK_SIZE 0x1000000ull /* 16 MiB main-thread stack */
#define TLR_SIZE   0x1000ull

typedef struct SpikeRun {
    GuestCtx *ctx;
    uint64_t entry, sp, tlr;
} SpikeRun;

static SpikeRun g_run;

static void *guest_thread(void *arg) {
    SpikeRun *r = arg;
    /* a private signal stack, so faults on a bad guest sp can still be reported */
    static char altstack[64 * 1024];
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0};
    sigaltstack(&ss, NULL);

    som_log("guest thread: entering rtld at 0x%llx, sp 0x%llx", (unsigned long long)r->entry,
            (unsigned long long)r->sp);
    uint32_t main_handle = 0;
    kthread_register_main(r->ctx, r->tlr, &main_handle);
    som_enter_guest(r->ctx, r->entry, r->sp, 0, main_handle);
    som_log("guest thread: stopped: %s (%llu svcs)", som_stop_reason(), (unsigned long long)r->ctx->svc_count);
    return NULL;
}

static void protect(uint64_t addr, uint64_t size, int prot) {
    size = (size + 0xFFF) & ~0xFFFull;
    if (size && mprotect((void *)addr, size, prot) != 0)
        som_log("mprotect 0x%llx+0x%llx failed: %s", (unsigned long long)addr, (unsigned long long)size,
                strerror(errno));
}

/* Cobalt (subsdk9) decides "emulator" when the game's code starts at 0x8004000 or 0x8504000 (an emulator's layout) and
 * "real console" otherwise. On the console path it starts a TCP log server on port 6969 and sends all of its output
 * there; that needs real sockets, which Somniel does not provide (nn::socket::Initialize is a stub), so the server
 * thread aborts and Cobalt's messages vanish. Somniel's default window puts main at 0x8004000, but on devices whose
 * low range is taken (moto g 2025, sometimes the Galaxy A17: guest.c) main moves to 0x100004000. Then the check's
 * `cset w0, eq` is made `mov w0, #1`, so Cobalt takes the emulator path everywhere. Matched by its exact instructions
 * (Cobalt 1.31.0, subsdk9+0x2c0); nothing is changed when they are not found. */
static void cobalt_emulator_patch(const NsoModule *mods, int nmods) {
    static const uint32_t sig[7] = {0x52880009, 0x52a00a0a, 0x72a10009, 0xeb09011f, 0xaa0a0129, 0xfa491104, 0x1a9f17e0};
    if (g_as_base == 0x08000000ull) return; /* main is at 0x8004000: the check already says emulator */
    for (int i = 0; i < nmods; i++) {
        if (strcmp(mods[i].name, "subsdk9") != 0) continue;
        uint32_t *t = (uint32_t *)(uintptr_t)(mods[i].base + mods[i].text_off);
        size_t n = mods[i].text_size / 4, hits = 0;
        for (size_t k = 0; k + 7 <= n; k++)
            if (t[k] == sig[0] && !memcmp(t + k, sig, sizeof sig)) t[k + 6] = 0x52800020u, hits++; /* mov w0, #1 */
        som_log("spike: Cobalt emulator check %s (%zu found)", hits ? "patched to emulator: high window" : "not found", hits);
    }
}

int spike_start(const char *exefs) {
    static const char *const kOrder[] = {"rtld",    "main",    "subsdk0", "subsdk1", "subsdk2", "subsdk3",
                                         "subsdk4", "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9",
                                         "sdk"};
    NsoModule mods[16];
    int nmods = 0;
    guest_layout_init(); /* first: picks the window that AS_BASE / MODULE_BASE / ISLAND_A name */
    som_tick_init();     /* before patching: the cntpct sites are scaled where the host counter is not 19.2 MHz */
    uint64_t base = MODULE_BASE;
    char path[512];

    som_install_crash_handlers();
    if (!som_data_dir()[0]) { /* normally main.c has set it; else it is the parent of the exefs directory */
        char dir[400];
        snprintf(dir, sizeof dir, "%s", exefs);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0;
        services_set_data_dir(dir);
    }
    sc_prepare_start(); /* shaders: translate the whole game in the background while it boots (shader_cache.c) */
    movies_init();      /* movies: index them; on a software-VP9 device, convert them to H.264 in the background (movies.c) */
    som_log("spike: loading from %s", exefs);
    for (size_t i = 0; i < sizeof kOrder / sizeof kOrder[0]; i++) {
        uint32_t sz;
        /* Atmosphere's exefs replacement: a file in <data>/sd/atmosphere/contents/<title id>/exefs/ wins over the
         * game's own (this is how Cobalt's subsdk9 is installed). Absent folder = plain game. */
        snprintf(path, sizeof path, "%s/sd/atmosphere/contents/0100A6301214E000/exefs/%s", som_data_dir(), kOrder[i]);
        if (nso_peek_size(path, &sz) == 0) {
            som_log("spike: %s comes from the SD layout (atmosphere exefs override)", kOrder[i]);
        } else {
            snprintf(path, sizeof path, "%s/%s", exefs, kOrder[i]);
            if (nso_peek_size(path, &sz) != 0) continue;
        }
        if (nso_load(path, kOrder[i], base, &mods[nmods]) != 0) return -1;
        som_register_module(kOrder[i], base, sz);
        base += sz;
        nmods++;
    }
    if (nmods == 0 || strcmp(mods[0].name, "rtld") != 0) {
        som_log("spike: no rtld found");
        return -1;
    }

    static Island isl[2]; /* the hook layer keeps a pointer to these */
    uint64_t island_b = (base + 0xFFFFF) & ~0xFFFFFull;
    if (island_init(&isl[0], ISLAND_A, ISLAND_SZ) != 0 || island_init(&isl[1], island_b, ISLAND_SZ) != 0)
        return -1;

    PatchStats st;
    memset(&st, 0, sizeof st);
    for (int i = 0; i < nmods; i++)
        patch_text(mods[i].base + mods[i].text_off, mods[i].text_size, isl, 2, &st);
    som_log("spike: patched svc=%u mrs_tpidr=%u mrs_tpidrro=%u msr_tpidr=%u cntpct=%u skipped=%u (islands used 0x%zx/0x%zx)",
            st.svc, st.mrs_tpidr, st.mrs_tpidrro, st.msr_tpidr, st.cntpct, st.skipped, isl[0].used, isl[1].used);
    {
        uint64_t frq;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
        som_log("spike: host counter frequency %llu Hz (the Switch's is 19200000)", (unsigned long long)frq);
    }
    hooks_system_register();
    hooks_time_register();
    hooks_gfx_register();
    hooks_hid_register();
    hooks_movie_register();
    hooks_swkbd_register();
    hooks_install(mods, nmods, isl, 2);
    {
        char pp[600];
        snprintf(pp, sizeof pp, "%s/probes.txt", som_data_dir());
        probes_install(mods, nmods, isl, 2, pp);
    }
    game_patches_apply(mods, nmods, isl, 2);
    dsmod_apply_plan(mods, nmods, island_b + ISLAND_SZ + 0x100000); /* dual-screen companion (dsmod.cpp): plan before the text is sealed */
    cobalt_emulator_patch(mods, nmods);
    island_seal(&isl[0]);
    island_seal(&isl[1]);

    for (int i = 0; i < nmods; i++) {
        const NsoModule *m = &mods[i];
        uint64_t text = m->base + m->text_off, ro = m->base + m->ro_off, data = m->base + m->data_off;
        uint64_t text_sz = (m->text_size + 0xFFF) & ~0xFFFull;
        uint64_t ro_sz = (m->ro_size + 0xFFF) & ~0xFFFull;
        uint64_t data_sz = m->image_size - m->data_off;
        __builtin___clear_cache((char *)text, (char *)(text + m->text_size));
        protect(text, text_sz, PROT_READ | PROT_EXEC);
        protect(ro, ro_sz, PROT_READ);
        guest_region_set(text, text_sz, MT_CODE_STATIC, PERM_R | PERM_X);
        guest_region_set(ro, ro_sz, MT_CODE_STATIC, PERM_R);
        guest_region_set(data, data_sz, MT_CODE_MUTABLE, PERM_R | PERM_W);
    }

    /* main thread stack and thread-local region */
    void *stk = mmap((void *)STACK_BASE, GUEST_STACK_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    uint64_t tlr_addr = STACK_BASE + GUEST_STACK_SIZE + 0x10000;
    void *tlr = mmap((void *)tlr_addr, TLR_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if ((uint64_t)stk != STACK_BASE || (uint64_t)tlr != tlr_addr) {
        som_log("spike: stack/TLR mmap failed: %s", strerror(errno));
        return -1;
    }
    guest_region_set(STACK_BASE, GUEST_STACK_SIZE, MT_MAPPED, PERM_R | PERM_W);
    guest_region_set(tlr_addr, TLR_SIZE, MT_TLS, PERM_R | PERM_W);

    GuestCtx *ctx = guest_ctx_alloc();
    if (!ctx) return -1;
    memset(ctx, 0, sizeof *ctx);
    ctx->id = 0;
    ctx->dispatch = som_svc_dispatch;
    ctx->tlr = tlr_addr;

    g_run.ctx = ctx;
    g_run.entry = mods[0].base; /* rtld's entry is the start of its text */
    g_run.sp = STACK_BASE + GUEST_STACK_SIZE;
    g_run.tlr = tlr_addr;
    services_register_all();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);
    pthread_t t;
    if (pthread_create(&t, &attr, guest_thread, &g_run) != 0) {
        som_log("spike: pthread_create failed");
        return -1;
    }
    pthread_detach(t);
    dsmod_start();
    return 0;
}










