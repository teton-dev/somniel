/* Horizon kernel calls, answered on the host. Every call is logged to
 * svc_trace.log. Calls that are not implemented stop the guest with their
 * number and registers, so the next piece of work is always visible. */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <linux/falloc.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <dlfcn.h>

#include "debug_log.h"
#include "telemetry.h"
#include "guest.h"
#include "hook.h"
#include "ipc.h"
#include "kobj.h"
#include "sync.h"
#include "thread.h"

static const char *const kNames[0x80] = {
    [0x01] = "SetHeapSize", [0x02] = "SetMemoryPermission", [0x03] = "SetMemoryAttribute",
    [0x04] = "MapMemory", [0x05] = "UnmapMemory", [0x06] = "QueryMemory", [0x07] = "ExitProcess",
    [0x08] = "CreateThread", [0x09] = "StartThread", [0x0A] = "ExitThread", [0x0B] = "SleepThread",
    [0x0C] = "GetThreadPriority", [0x0D] = "SetThreadPriority", [0x0E] = "GetThreadCoreMask",
    [0x0F] = "SetThreadCoreMask", [0x10] = "GetCurrentProcessorNumber", [0x11] = "SignalEvent",
    [0x12] = "ClearEvent", [0x13] = "MapSharedMemory", [0x14] = "UnmapSharedMemory",
    [0x15] = "CreateTransferMemory", [0x16] = "CloseHandle", [0x17] = "ResetSignal",
    [0x18] = "WaitSynchronization", [0x19] = "CancelSynchronization", [0x1A] = "ArbitrateLock",
    [0x1B] = "ArbitrateUnlock", [0x1C] = "WaitProcessWideKeyAtomic", [0x1D] = "SignalProcessWideKey",
    [0x1E] = "GetSystemTick", [0x1F] = "ConnectToNamedPort", [0x20] = "SendSyncRequestLight",
    [0x21] = "SendSyncRequest", [0x22] = "SendSyncRequestWithUserBuffer",
    [0x23] = "SendAsyncRequestWithUserBuffer", [0x24] = "GetProcessId", [0x25] = "GetThreadId",
    [0x26] = "Break", [0x27] = "OutputDebugString", [0x28] = "ReturnFromException",
    [0x29] = "GetInfo", [0x2A] = "FlushEntireDataCache", [0x2B] = "FlushDataCache",
    [0x2C] = "MapPhysicalMemory", [0x2D] = "UnmapPhysicalMemory", [0x32] = "SetThreadActivity", [0x33] = "GetThreadContext3", [0x34] = "WaitForAddress",
    [0x35] = "SignalToAddress", [0x36] = "SynchronizePreemptionState", [0x40] = "CreateSession",
    [0x41] = "AcceptSession", [0x42] = "ReplyAndReceiveLight", [0x43] = "ReplyAndReceive",
    [0x45] = "CreateEvent",
};

static char g_stop[256];
const char *som_stop_reason(void) { return g_stop[0] ? g_stop : "(none)"; }

static uint64_t cntvct(void) {
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

/* ---- module table for symbolising addresses in logs ---- */
#define MAX_MODS 16
static struct {
    char name[16];
    uint64_t base, size;
} g_mods[MAX_MODS];
static int g_nmods;

void som_register_module(const char *name, uint64_t base, uint64_t size) {
    if (g_nmods < MAX_MODS) {
        snprintf(g_mods[g_nmods].name, 16, "%s", name);
        g_mods[g_nmods].base = base;
        g_mods[g_nmods].size = size;
        g_nmods++;
    }
}

const char *som_module_of(uint64_t pc, uint64_t *off) {
    for (int i = 0; i < g_nmods; i++)
        if (pc >= g_mods[i].base && pc < g_mods[i].base + g_mods[i].size) {
            *off = pc - g_mods[i].base;
            return g_mods[i].name;
        }
    *off = pc;
    return "?";
}

static void log_backtrace(GuestCtx *c, const char *what) {
    uint64_t off;
    char line[512];
    int n = snprintf(line, sizeof line, "t%d %s: lr=%s+0x%llx", c->id, what, som_module_of(c->x[30], &off), (unsigned long long)off);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 10 && fp && !(fp & 7) && fp >= AS_BASE && fp < AS_END && n < (int)sizeof line - 40; i++) {
        uint64_t next = ((uint64_t *)fp)[0], lr = ((uint64_t *)fp)[1];
        n += snprintf(line + n, sizeof line - (size_t)n, " <- %s+0x%llx", som_module_of(lr, &off), (unsigned long long)off);
        if (next <= fp || next - fp > (64u << 20)) break;
        fp = next;
    }
    /* infinite waits re-enter every 100 ms (forced wakeups in sync.c): log a backtrace only when it changes */
    static __thread uint64_t last_hash;
    uint64_t h = 1469598103934665603ull;
    for (const char *p = line; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
    if (h == last_hash) return;
    last_hash = h;
    som_log("%s", line);
}

static void dump_regs(GuestCtx *c) {
    uint64_t off;
    const char *m = som_module_of(c->x[30], &off);
    som_log("  lr=0x%llx (%s+0x%llx) sp=0x%llx", (unsigned long long)c->x[30], m, (unsigned long long)off,
            (unsigned long long)c->sp);
    /* walk the guest frame chain */
    uint64_t fp = c->x[29];
    for (int n = 0; n < 12 && fp && !(fp & 7) && fp >= AS_BASE && fp < AS_END; n++) {
        uint64_t next = ((uint64_t *)fp)[0], lr = ((uint64_t *)fp)[1];
        const char *fm = som_module_of(lr, &off);
        som_log("  frame %2d: %s+0x%llx", n, fm, (unsigned long long)off);
        if (next <= fp || next - fp > (64u << 20)) break;
        fp = next;
    }
    for (int i = 0; i < 8; i++) som_log("  x%d=0x%llx", i, (unsigned long long)c->x[i]);
}

void som_stop(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_stop, sizeof g_stop, fmt, ap);
    va_end(ap);
    som_log("GUEST STOP: %s", g_stop);
    if (!t_ctx) abort();
    dump_regs(t_ctx);
    som_leave_guest(t_ctx);
}

/* ---- memory ---- */

static int g_heap_fd = -1;
static uint64_t g_heap_size;

static int memfd_new(const char *name) { return (int)syscall(SYS_memfd_create, name, 0); }

static uint32_t svc_set_heap_size(GuestCtx *c) {
    uint64_t size = c->x[1];
    if (size > HEAP_REGION || (size & 0x1FFFFF)) return RES_INVALID_SIZE;
    if (g_heap_fd < 0) {
        g_heap_fd = memfd_new("somniel-heap");
        if (g_heap_fd < 0) {
            som_log("memfd_create failed: %s", strerror(errno));
            return RES_OUT_OF_MEMORY;
        }
    }
    if (size > g_heap_size) {
        if (ftruncate(g_heap_fd, (off_t)size) != 0) {
            som_log("ftruncate(heap, 0x%llx) failed: %s", (unsigned long long)size, strerror(errno));
            return RES_OUT_OF_MEMORY;
        }
        void *p = mmap((void *)(HEAP_BASE + g_heap_size), size - g_heap_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED, g_heap_fd, (off_t)g_heap_size);
        if ((uint64_t)p != HEAP_BASE + g_heap_size) {
            som_log("SetHeapSize: mmap failed: %s", strerror(errno));
            return RES_OUT_OF_MEMORY;
        }
    } else if (size < g_heap_size) {
        guest_map_bump();
        munmap((void *)(HEAP_BASE + size), g_heap_size - size);
        ftruncate(g_heap_fd, (off_t)size);
    }
    g_heap_size = size;
    guest_region_set(HEAP_BASE, size, MT_HEAP, PERM_R | PERM_W);
    c->x[1] = HEAP_BASE;
    return RES_OK;
}

static int in_heap(uint64_t a, uint64_t n) { return a >= HEAP_BASE && a + n <= HEAP_BASE + g_heap_size && a + n > a; }

/* Where the pages at src live: the shared-memory file and the offset into it. */
static int backing_of(uint64_t src, uint64_t size, int *fd, off_t *off);

static uint32_t svc_map_memory(GuestCtx *c) {
    guest_map_bump();
    uint64_t dst = c->x[0], src = c->x[1], size = c->x[2];
    int fd;
    off_t off;
    if (!backing_of(src, size, &fd, &off)) {
        som_log("MapMemory: source 0x%llx+0x%llx has no shared backing", (unsigned long long)src, (unsigned long long)size);
        return RES_INVALID_MEMORY;
    }
    void *p = mmap((void *)dst, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, off);
    if ((uint64_t)p != dst) return RES_INVALID_MEMORY;
    mprotect((void *)src, size, PROT_NONE);
    guest_region_set(dst, size, MT_MAPPED, PERM_R | PERM_W);
    return RES_OK;
}

static uint32_t svc_unmap_memory(GuestCtx *c) {
    guest_map_bump();
    uint64_t dst = c->x[0], src = c->x[1], size = c->x[2];
    int fd;
    off_t off;
    if (!backing_of(src, size, &fd, &off)) return RES_INVALID_MEMORY;
    mmap((void *)dst, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    mprotect((void *)src, size, PROT_READ | PROT_WRITE);
    guest_region_clear(dst);
    return RES_OK;
}
/* Pages of the alias region mapped with MapPhysicalMemory, one bit per 4 KiB page. */
#define ALIAS_PAGES (ALIAS_SIZE >> 12)
static uint8_t *g_alias_bits;
static int g_alias_fd = -1;
static pthread_mutex_t g_alias_lock = PTHREAD_MUTEX_INITIALIZER;

static inline int alias_get(uint64_t p) { return (g_alias_bits[p >> 3] >> (p & 7)) & 1; }

/* The bitmap is scanned and written a 64-bit word at a time: a QueryMemory on a large free stretch of the 64 GiB region
 * used to walk up to 16 million pages one bit at a time under the lock (16 to 60 ms, stalling every MapPhysicalMemory
 * behind it; the combat-start hitches on the AYN Thor). */
static uint64_t alias_next_diff(uint64_t p, uint64_t end, int v) { /* first page in [p, end) whose state is not v, else end */
    const uint64_t *w = (const uint64_t *)g_alias_bits;
    while (p < end) {
        uint64_t wi = p >> 6, x = w[wi];
        if (v) x = ~x;
        x >>= (p & 63);
        if (x) {
            uint64_t r = p + (uint64_t)__builtin_ctzll(x);
            return r < end ? r : end;
        }
        p = (wi + 1) << 6;
    }
    return end;
}

static uint64_t alias_run_start(uint64_t p, int v) { /* first page of the run of state v that ends at page p (inclusive) */
    const uint64_t *w = (const uint64_t *)g_alias_bits;
    uint64_t i = p; /* pages below i are still to examine */
    while (i > 0) {
        uint64_t wi = (i - 1) >> 6, bit = (i - 1) & 63, x = w[wi];
        if (v) x = ~x;
        if (bit != 63) x &= (1ull << (bit + 1)) - 1;
        if (x) return (wi << 6) + 63 - (uint64_t)__builtin_clzll(x) + 1;
        i = wi << 6;
    }
    return 0;
}

static void alias_set_range(uint64_t a, uint64_t b, int v) { /* pages [a, b) */
    uint64_t *w = (uint64_t *)g_alias_bits;
    while (a < b) {
        uint64_t bit = a & 63, n = 64 - bit;
        if (n > b - a) n = b - a;
        uint64_t mask = (n == 64 ? ~0ull : ((1ull << n) - 1)) << bit;
        if (v) w[a >> 6] |= mask;
        else w[a >> 6] &= ~mask;
        a += n;
    }
}

/* Freeing the memory behind an unmapped range (the hole punch) costs 50 to 175 ms for a few hundred MB. It runs on a
 * helper thread; the pages are already unmapped, and a MapPhysicalMemory over a range still being punched waits for it
 * (the kernel hands out zeroed pages, so a late punch must never hit a remapped range). */
#define PUNCH_MAX 64
typedef struct { uint64_t off, len; int inflight; } Punch;
static Punch g_pq[PUNCH_MAX];
static int g_npq, g_pq_thread;
static pthread_mutex_t g_pq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pq_work = PTHREAD_COND_INITIALIZER, g_pq_done = PTHREAD_COND_INITIALIZER;

static void *punch_worker(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "alias-punch");
    pthread_mutex_lock(&g_pq_mu);
    for (;;) {
        int k = -1;
        for (int i = 0; i < g_npq; i++)
            if (!g_pq[i].inflight) { k = i; break; }
        if (k < 0) { pthread_cond_wait(&g_pq_work, &g_pq_mu); continue; }
        g_pq[k].inflight = 1;
        Punch p = g_pq[k];
        pthread_mutex_unlock(&g_pq_mu);
        fallocate(g_alias_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)p.off, (off_t)p.len);
        pthread_mutex_lock(&g_pq_mu);
        for (int i = 0; i < g_npq; i++)
            if (g_pq[i].inflight && g_pq[i].off == p.off && g_pq[i].len == p.len) { g_pq[i] = g_pq[--g_npq]; break; }
        pthread_cond_broadcast(&g_pq_done);
    }
    return NULL;
}

static void punch_queue(uint64_t off, uint64_t len) {
    pthread_mutex_lock(&g_pq_mu);
    if (g_npq == PUNCH_MAX) { /* queue full: do it here */
        pthread_mutex_unlock(&g_pq_mu);
        fallocate(g_alias_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)off, (off_t)len);
        return;
    }
    if (!g_pq_thread) {
        pthread_t t;
        if (pthread_create(&t, NULL, punch_worker, NULL) == 0) pthread_detach(t), g_pq_thread = 1;
        else {
            pthread_mutex_unlock(&g_pq_mu);
            fallocate(g_alias_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)off, (off_t)len);
            return;
        }
    }
    g_pq[g_npq++] = (Punch){off, len, 0};
    pthread_cond_signal(&g_pq_work);
    pthread_mutex_unlock(&g_pq_mu);
}

static void punch_wait(uint64_t off, uint64_t len) { /* until no queued or running punch overlaps [off, off+len) */
    pthread_mutex_lock(&g_pq_mu);
    for (;;) {
        int hit = 0;
        for (int i = 0; i < g_npq; i++)
            if (g_pq[i].off < off + len && off < g_pq[i].off + g_pq[i].len) { hit = 1; break; }
        if (!hit) break;
        pthread_cond_wait(&g_pq_done, &g_pq_mu);
    }
    pthread_mutex_unlock(&g_pq_mu);
}

/* The run of equal state containing addr, for QueryMemory. Returns 1 when addr is inside the alias region. */
int alias_run(uint64_t addr, uint64_t *base, uint64_t *size, int *mapped) {
    if (addr < ALIAS_BASE || addr >= ALIAS_BASE + ALIAS_SIZE) return 0;
    pthread_mutex_lock(&g_alias_lock);
    uint64_t p = (addr - ALIAS_BASE) >> 12, s = p, e = p + 1;
    int v = g_alias_bits ? alias_get(p) : 0;
    if (g_alias_bits) {
        s = alias_run_start(p, v);
        e = alias_next_diff(p + 1, ALIAS_PAGES, v);
    } else {
        s = 0;
        e = ALIAS_PAGES;
    }
    pthread_mutex_unlock(&g_alias_lock);
    *base = ALIAS_BASE + (s << 12);
    *size = (e - s) << 12;
    *mapped = v;
    return 1;
}

/* MapPhysicalMemory maps only the pages of the range that are not mapped yet and keeps the others, as the kernel
 * does; UnmapPhysicalMemory releases only the mapped ones. */
static uint32_t alias_apply(uint64_t addr, uint64_t size, int map) {
    if ((addr & 0xFFF) || !size || (size & 0xFFF) || addr < ALIAS_BASE || addr + size > ALIAS_BASE + ALIAS_SIZE)
        return (addr & 0xFFF) ? RES_INVALID_ADDRESS : RES_INVALID_SIZE;
    uint32_t res = RES_OK;
    guest_map_bump(); /* the dual-screen host's cached readable regions may include these pages */
    if (map) punch_wait(addr - ALIAS_BASE, size); /* the pages must be fresh: no punch of this range may still be pending */
    pthread_mutex_lock(&g_alias_lock);
    if (!g_alias_bits) g_alias_bits = calloc(ALIAS_PAGES / 8, 1);
    if (g_alias_fd < 0) {
        g_alias_fd = memfd_new("somniel-alias");
        if (g_alias_fd < 0 || ftruncate(g_alias_fd, (off_t)ALIAS_SIZE) != 0) {
            som_log("alias memfd setup failed: %s", strerror(errno));
            pthread_mutex_unlock(&g_alias_lock);
            return RES_OUT_OF_MEMORY;
        }
    }
    uint64_t p0 = (addr - ALIAS_BASE) >> 12, n = size >> 12;
    for (uint64_t i = 0; i < n && res == RES_OK;) {
        int state = alias_get(p0 + i);
        uint64_t j = alias_next_diff(p0 + i, p0 + n, state) - p0;
        if (state != map) { /* a run that needs changing */
            uint64_t a = addr + (i << 12), len = (j - i) << 12;
            void *p;
            if (map) {
                p = mmap((void *)a, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, g_alias_fd, (off_t)(a - ALIAS_BASE));
            } else {
                p = mmap((void *)a, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
                if ((uint64_t)p == a) punch_queue(a - ALIAS_BASE, len);
            }
            if ((uint64_t)p != a) res = RES_OUT_OF_MEMORY;
            else alias_set_range(p0 + i, p0 + j, map);
        }
        i = j;
    }
    pthread_mutex_unlock(&g_alias_lock);
    return res;
}

static uint32_t svc_map_physical(GuestCtx *c) { return alias_apply(c->x[0], c->x[1], 1); }
static uint32_t svc_unmap_physical(GuestCtx *c) { return alias_apply(c->x[0], c->x[1], 0); }
static int backing_of(uint64_t src, uint64_t size, int *fd, off_t *off) {
    if (in_heap(src, size)) {
        *fd = g_heap_fd;
        *off = (off_t)(src - HEAP_BASE);
        return 1;
    }
    if (src >= ALIAS_BASE && src + size <= ALIAS_BASE + ALIAS_SIZE && src + size > src && g_alias_fd >= 0) {
        *fd = g_alias_fd;
        *off = (off_t)(src - ALIAS_BASE);
        return 1;
    }
    return 0;
}
/* ---- process-memory svcs on our own process (exlaunch/Cobalt hook installation) ----
 * MapProcessMemory(dst, process, src, size) makes a second, read-write view of the code pages at src. The pages are
 * moved into a memfd that is mapped at src (with its old permissions) and at dst, so a write through dst is seen by
 * the code at src. */
typedef struct ProcAlias { uint64_t src, dst, size; int fd; } ProcAlias;
static ProcAlias g_pa[16];
static pthread_mutex_t g_pa_lock = PTHREAD_MUTEX_INITIALIZER;

static int prot_from_perm(uint32_t perm) {
    return ((perm & PERM_R) ? PROT_READ : 0) | ((perm & PERM_W) ? PROT_WRITE : 0) | ((perm & PERM_X) ? PROT_EXEC : 0);
}

static uint32_t svc_map_process_memory(GuestCtx *c) {
    guest_map_bump();
    uint64_t dst = c->x[0], src = c->x[2], size = c->x[3];
    if (((dst | src | size) & 0xFFF) || !size) return RES_INVALID_ADDRESS;
    MemInfo mi;
    guest_query(src, &mi);
    int prot = prot_from_perm(mi.perm) ? prot_from_perm(mi.perm) : PROT_READ;
    pthread_mutex_lock(&g_pa_lock);
    int slot = -1;
    for (int i = 0; i < 16 && slot < 0; i++)
        if (!g_pa[i].fd) slot = i;
    uint32_t res = RES_OUT_OF_MEMORY;
    int fd = slot < 0 ? -1 : memfd_new("somniel-codealias");
    if (fd >= 0 && ftruncate(fd, (off_t)size) == 0 && pwrite(fd, (void *)src, size, 0) == (ssize_t)size) {
        void *a = mmap((void *)src, size, prot, MAP_SHARED | MAP_FIXED, fd, 0);
        void *b = mmap((void *)dst, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
        if ((uint64_t)a == src && (uint64_t)b == dst) {
            g_pa[slot] = (ProcAlias){src, dst, size, fd};
            guest_region_set(dst, size, MT_MAPPED, PERM_R | PERM_W);
            res = RES_OK;
        } else {
            som_log("MapProcessMemory: mmap failed: %s", strerror(errno));
        }
    } else if (fd >= 0) {
        som_log("MapProcessMemory: memfd setup failed: %s", strerror(errno));
    }
    if (res != RES_OK && fd >= 0) close(fd);
    pthread_mutex_unlock(&g_pa_lock);
    return res;
}

static uint32_t svc_unmap_process_memory(GuestCtx *c) {
    guest_map_bump();
    uint64_t dst = c->x[0], size = c->x[3];
    mmap((void *)dst, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    guest_region_clear(dst);
    pthread_mutex_lock(&g_pa_lock);
    for (int i = 0; i < 16; i++)
        if (g_pa[i].fd && g_pa[i].dst == dst) { /* the src mapping stays: same pages, now shared */
            close(g_pa[i].fd);
            g_pa[i].fd = 0;
        }
    pthread_mutex_unlock(&g_pa_lock);
    return RES_OK;
}

/* SetProcessMemoryPermission(process, addr, size, perm) */
static uint32_t svc_set_process_permission(GuestCtx *c) {
    uint64_t addr = c->x[1], size = c->x[2];
    if ((addr | size) & 0xFFF) return RES_INVALID_ADDRESS;
    guest_map_bump();
    if (mprotect((void *)addr, size, prot_from_perm((uint32_t)c->x[3])) != 0) return RES_INVALID_MEMORY;
    return RES_OK;
}

static void svc_get_info(GuestCtx *c) {
    uint64_t type = c->x[1], sub = c->x[3], v = 0;
    switch (type) {
    case 0: v = 0x7; break;                       /* core mask: cores 0-2 */
    case 1: v = 0xFFFFFFFFFFFFFFFFull; break;     /* priority mask */
    case 2: v = ALIAS_BASE; break;
    case 3: v = ALIAS_SIZE; break;
    case 4: v = HEAP_BASE; break;
    case 5: v = HEAP_REGION; break;
    case 6: v = 0xCC200000ull; break;             /* total memory: 3.2 GiB for the application */
    case 7: v = g_heap_size + 0x10000000ull; break;
    case 8: v = 0; break;                         /* debugger attached */
    case 9: v = 0xFFFF8001; break;                /* resource limit handle */
    case 10: v = 0; break;                        /* idle tick count */
    case 11: v = 0x1234567800000000ull ^ (sub * 0x9E3779B97F4A7C15ull); break; /* entropy */
    case 12: v = ASLR_BASE; break;
    case 13: v = ASLR_SIZE; break;
    case 14: v = STACK_BASE; break;
    case 15: v = STACK_REGION; break;
    case 16: v = 0x1000000; break;                /* system resource size total (main.npdm: 16 MiB) */
    case 17: v = 0x200000; break;                 /* system resource size used */
    case 18: v = 0x0100A6301214E000ull; break;    /* program id */
    case 20: v = 0; break;                        /* user exception context address */
    case 21: v = 0xCC200000ull; break;
    case 22: v = g_heap_size + 0x10000000ull; break;
    case 23: v = 1; break;                        /* is application */
    default:
        som_log("svc GetInfo: unknown type %llu", (unsigned long long)type);
        c->x[0] = RES_INVALID_ENUM;
        return;
    }
    c->x[0] = 0;
    c->x[1] = v;
}

/* ---- guest-to-guest sessions (CreateSession / ReplyAndReceive) ----
 * exlaunch (Cobalt) makes a session to itself, runs a server thread on it and sends the pseudo process handle
 * 0xFFFF8001 in a copy-handle descriptor to obtain a real process handle. Only that shape is supported: one request
 * of up to 0x40 bytes (header, optional handle descriptor, handles), no buffers. */
typedef struct GSess {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int refs, pending, replied, server_closed;
    uint32_t req[16];
} GSess;
static int g_process_object; /* what a KO_PROCESS handle points at */

static void gs_unref(GSess *g) {
    pthread_mutex_lock(&g->mu);
    int last = --g->refs == 0;
    pthread_mutex_unlock(&g->mu);
    if (last) free(g);
}

static void gs_create(GuestCtx *c) {
    GSess *g = calloc(1, sizeof *g);
    pthread_mutex_init(&g->mu, NULL);
    pthread_cond_init(&g->cv, NULL);
    g->refs = 2;
    c->x[0] = 0;
    c->x[1] = kobj_new(KO_GS_SERVER, g);
    c->x[2] = kobj_new(KO_GS_CLIENT, g);
}

static uint32_t gs_send(GuestCtx *c, GSess *g) {
    pthread_mutex_lock(&g->mu);
    memcpy(g->req, (void *)c->tlr, sizeof g->req);
    g->pending = 1;
    g->replied = 0;
    pthread_cond_broadcast(&g->cv);
    while (!g->replied && !g->server_closed) pthread_cond_wait(&g->cv, &g->mu);
    uint32_t r = g->replied ? RES_OK : 0xF601u; /* session closed */
    g->pending = 0;
    pthread_mutex_unlock(&g->mu);
    return r;
}

/* ReplyAndReceive(&index, handles, count, reply_target, timeout): one server handle, infinite wait. */
static uint32_t gs_receive(GuestCtx *c) {
    const uint32_t *hs = (const uint32_t *)c->x[1];
    uint32_t count = (uint32_t)c->x[2], reply = (uint32_t)c->x[3];
    GSess *g = NULL;
    uint32_t idx = 0;
    for (uint32_t i = 0; i < count && !g; i++)
        if ((g = kobj_get(hs[i], KO_GS_SERVER))) idx = i;
    if (!g) return RES_INVALID_HANDLE;
    pthread_mutex_lock(&g->mu);
    if (reply && kobj_get(reply, KO_GS_SERVER) == g) {
        g->replied = 1;
        g->pending = 0;
        pthread_cond_broadcast(&g->cv);
    }
    while (!g->pending) pthread_cond_wait(&g->cv, &g->mu);
    uint32_t *out = (uint32_t *)c->tlr;
    memcpy(out, g->req, sizeof g->req);
    if (out[1] & 0x80000000u) { /* handle descriptor: swap the pseudo process handle for a real one */
        uint32_t d = out[2], o = 3;
        if (d & 1) o += 2; /* process id */
        for (uint32_t i = 0; i < ((d >> 1) & 15) + ((d >> 5) & 15) && o < 16; i++, o++)
            if (out[o] == HANDLE_CUR_PROCESS) out[o] = kobj_new(KO_PROCESS, &g_process_object);
    }
    pthread_mutex_unlock(&g->mu);
    c->x[1] = idx;
    return RES_OK;
}

static uint32_t svc_close_handle(uint32_t h) {
    int kind = 0;
    void *o = kobj_take(h, &kind);
    if (!o) return RES_INVALID_HANDLE;
    switch (kind) {
    case KO_GS_SERVER: {
        GSess *g = o;
        pthread_mutex_lock(&g->mu);
        g->server_closed = 1;
        pthread_cond_broadcast(&g->cv);
        pthread_mutex_unlock(&g->mu);
        gs_unref(g);
        break;
    }
    case KO_GS_CLIENT: gs_unref(o); break;
    case KO_SESSION: ipc_close_session(o); break;
    case KO_EVENT_W:
    case KO_EVENT_R: kevent_unref(o); break;
    case KO_TMEM: free(o); break;
    default: break; /* threads and shared memory live on */
    }
    return RES_OK;
}

static int64_t timeout_arg(uint64_t v) { return (int64_t)v; }

/* Time spent in each kernel call, all threads together. som_stall_mark() is called at every present; with a label it
 * first logs what happened since the previous mark (the renderer calls it so for frames that took too long). */
static uint64_t g_svc_ns[0x80], g_svc_cnt[0x80];
uint64_t g_fs_read_ns, g_fs_read_bytes, g_fs_read_calls, g_gc_pauses, g_gc_pause_ns;
extern void nvn_time_report(char *out, size_t cap); /* nvn_glue.c */
extern int g_guest_tid[128];            /* thread.c */
void som_stall_mark(const char *label) {
    static uint64_t p_ns[0x80], p_cnt[0x80], p_fs[5];
    uint64_t fs[5] = {__atomic_load_n(&g_fs_read_ns, __ATOMIC_RELAXED), __atomic_load_n(&g_fs_read_bytes, __ATOMIC_RELAXED),
                      __atomic_load_n(&g_fs_read_calls, __ATOMIC_RELAXED), __atomic_load_n(&g_gc_pauses, __ATOMIC_RELAXED),
                      __atomic_load_n(&g_gc_pause_ns, __ATOMIC_RELAXED)};
    if (label) {
        char line[900];
        int o = snprintf(line, sizeof line, "%s: fs %llu reads %.2f MB %.1f ms; gc pauses %llu (%.1f ms) | svc ms(calls):", label,
                         (unsigned long long)(fs[2] - p_fs[2]), (double)(fs[1] - p_fs[1]) / 1048576.0, (double)(fs[0] - p_fs[0]) / 1e6,
                         (unsigned long long)(fs[3] - p_fs[3]), (double)(fs[4] - p_fs[4]) / 1e6);
        for (int k = 0; k < 8 && o < 820; k++) { /* the eight calls with the most time */
            int best = -1;
            uint64_t bd = 0;
            for (int i = 0; i < 0x80; i++) {
                uint64_t d = __atomic_load_n(&g_svc_ns[i], __ATOMIC_RELAXED) - p_ns[i];
                if (d > bd) bd = d, best = i;
            }
            if (best < 0 || bd < 1000000) break;
            o += snprintf(line + o, sizeof line - (size_t)o, " %s %.0f(%llu)", kNames[best] ? kNames[best] : "?", (double)bd / 1e6,
                          (unsigned long long)(__atomic_load_n(&g_svc_cnt[best], __ATOMIC_RELAXED) - p_cnt[best]));
            p_ns[best] = __atomic_load_n(&g_svc_ns[best], __ATOMIC_RELAXED); /* not picked again */
        }
        som_log("%s", line);
        char nv[480];
        nvn_time_report(nv, sizeof nv);
        som_log("  nvn api ms this frame:%s", nv[0] ? nv : " (nothing over 1 ms)");
    } else {
        nvn_time_report(NULL, 0); /* keep the per-frame baseline current */
    }
    {   /* CPU time each guest thread used since the previous present: is the frame CPU-bound, and on which thread? */
        static uint64_t p_cpu[128];
        uint64_t d[128];
        for (int i = 0; i < 128; i++) {
            d[i] = 0;
            if (!g_guest_tid[i]) continue;
            struct timespec ts;
            if (clock_gettime((clockid_t)((~(unsigned)g_guest_tid[i]) << 3 | 6), &ts)) continue;
            uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
            d[i] = now - p_cpu[i];
            p_cpu[i] = now;
        }
        if (label) {
            char cl[300];
            int o = snprintf(cl, sizeof cl, "  guest thread cpu ms this frame:");
            for (int k = 0; k < 6 && o < 260; k++) {
                int best = -1;
                for (int i = 0; i < 128; i++)
                    if (d[i] > 2000000 && (best < 0 || d[i] > d[best])) best = i;
                if (best < 0) break;
                o += snprintf(cl + o, sizeof cl - (size_t)o, " t%d %.1f", best, (double)d[best] / 1e6);
                d[best] = 0;
            }
            som_log("%s", cl);
        }
    }
    for (int i = 0; i < 0x80; i++) p_ns[i] = __atomic_load_n(&g_svc_ns[i], __ATOMIC_RELAXED), p_cnt[i] = __atomic_load_n(&g_svc_cnt[i], __ATOMIC_RELAXED);
    memcpy(p_fs, fs, sizeof fs);
}

static void svc_exec(GuestCtx *c) {
    uint32_t n = (uint32_t)c->svc_num;
    uint64_t *x = c->x;
    c->svc_count++;
    const char *name = (n < 0x80 && kNames[n]) ? kNames[n] : "?";
    if (som_trace_on) {
    uint64_t off;
    const char *mod = som_module_of(x[30], &off);
    /* collapse polling loops (same svc from the same place, again and again) to one line */
    extern __thread int som_trace_quiet;
    static __thread uint64_t last_key, repeats;
    uint64_t key = ((uint64_t)n << 48) ^ x[30];
    som_trace_quiet = 0;
    if (key == last_key && (n == 0x21 || n == 0x0b || n == 0x18 || n == 0x17 || n == 0x1c)) {
        if (++repeats > 4) {
            som_trace_quiet = 1;
            if ((repeats & 0xfffff) == 0) {
                som_trace_quiet = 0;
                som_trace("t%d ... same call repeated %llu times so far", c->id, (unsigned long long)repeats);
                som_trace_quiet = 1;
            }
        }
    } else {
        if (repeats > 4) som_trace("t%d ... previous call repeated %llu times", c->id, (unsigned long long)repeats);
        repeats = 0;
        last_key = key;
    }
    som_trace("t%d svc #%llu 0x%02x %-26s x0=%llx x1=%llx x2=%llx x3=%llx lr=%s+%llx", c->id,
              (unsigned long long)c->svc_count, n, name, (unsigned long long)x[0], (unsigned long long)x[1],
              (unsigned long long)x[2], (unsigned long long)x[3], mod, (unsigned long long)off);
    }

    switch (n) {
    case 0x01: x[0] = svc_set_heap_size(c); guest_map_bump(); return; /* after the change too: see g_map_epoch */
    case 0x02: { /* SetMemoryPermission(addr, size, perm) */
        int prot = 0;
        if (x[2] & PERM_R) prot |= PROT_READ;
        if (x[2] & PERM_W) prot |= PROT_WRITE;
        if (x[2] & PERM_X) prot |= PROT_EXEC;
        guest_map_bump();
        x[0] = mprotect((void *)x[0], x[1], prot) == 0 ? 0 : RES_INVALID_MEMORY;
        guest_map_bump();
        return;
    }
    case 0x03: x[0] = 0; return; /* SetMemoryAttribute */
    case 0x04: x[0] = svc_map_memory(c); guest_map_bump(); return; /* after the change too: see g_map_epoch */
    case 0x05: x[0] = svc_unmap_memory(c); guest_map_bump(); return; /* after the change too: see g_map_epoch */
    case 0x06:
        guest_query(x[2], (MemInfo *)x[0]);
        x[0] = 0;
        x[1] = 0; /* page info */
        return;
    case 0x07: som_stop("ExitProcess");
    case 0x08: { /* CreateThread(entry, arg, stack_top, priority, core) */
        uint32_t h = 0;
        x[0] = kthread_create(x[1], x[2], x[3], &h);
        x[1] = h;
        return;
    }
    case 0x09: x[0] = kthread_start((uint32_t)x[0]); return;
    case 0x0A: kthread_exit_current(c);
    case 0x0B: /* SleepThread */
        if ((int64_t)x[0] > 0) {
            struct timespec ts = {(time_t)(x[0] / 1000000000ull), (long)(x[0] % 1000000000ull)};
            nanosleep(&ts, NULL);
        } else {
            sched_yield();
        }
        return;
    case 0x0C: x[0] = 0; x[1] = 44; return;                        /* GetThreadPriority */
    case 0x0D: x[0] = 0; return;                                   /* SetThreadPriority */
    case 0x0E: x[0] = 0; x[1] = 0; x[2] = 0x7; return;             /* GetThreadCoreMask */
    case 0x0F: x[0] = 0; return;                                   /* SetThreadCoreMask */
    case 0x10: x[0] = 0; return;                                   /* GetCurrentProcessorNumber */
    case 0x11: case 0x12: { /* SignalEvent / ClearEvent (writable handle) */
        KEvent *e = kobj_get((uint32_t)x[0], KO_EVENT_W);
        if (!e) { x[0] = RES_INVALID_HANDLE; return; }
        if (n == 0x11) kevent_signal(e); else kevent_clear(e);
        x[0] = 0;
        return;
    }
    case 0x15: { /* CreateTransferMemory(addr, size, perm) -> handle */
        uint64_t *t = calloc(3, sizeof *t);
        t[0] = x[1];
        t[1] = x[2];
        t[2] = x[3];
        uint32_t h = kobj_new(KO_TMEM, t);
        x[0] = h ? 0 : RES_OUT_OF_MEMORY;
        x[1] = h;
        return;
    }
    case 0x16: x[0] = svc_close_handle((uint32_t)x[0]); return;
    case 0x17: { /* ResetSignal(readable handle) */
        KEvent *e = kobj_get((uint32_t)x[0], KO_EVENT_R);
        if (!e) { x[0] = RES_INVALID_HANDLE; return; }
        x[0] = e->signaled ? 0 : RES_INVALID_STATE;
        kevent_clear(e);
        return;
    }
    case 0x18: { /* WaitSynchronization(handles, count, timeout) */
        int32_t idx = -1;
        if (som_trace_on && (int64_t)x[3] < 0) log_backtrace(c, "waits (sync)"); /* debug only: a stack walk + log write per wait hitched the main thread */
        x[0] = sync_wait_synchronization((const uint32_t *)x[1], (int32_t)x[2], timeout_arg(x[3]), &idx);
        x[1] = (uint64_t)(uint32_t)idx;
        return;
    }
    case 0x19: x[0] = 0; return; /* CancelSynchronization */
    case 0x1A: x[0] = sync_arbitrate_lock((uint32_t)x[0], x[1], (uint32_t)x[2]); return;
    case 0x1B: x[0] = sync_arbitrate_unlock(x[0]); return;
    case 0x1C:
        if (som_trace_on && ((int64_t)x[3] < 0 || x[3] > 0xFFFFFFFFFull)) log_backtrace(c, "waits (cond var)");
        x[0] = sync_wait_cv(x[0], x[1], (uint32_t)x[2], timeout_arg(x[3])); return;
    case 0x1D: sync_signal_cv(x[0], (int32_t)x[1]); x[0] = 0; return;
    case 0x1E: x[0] = som_guest_tick(); return; /* GetSystemTick: 19.2 MHz ticks (guest.h) */
    case 0x24: x[0] = 0; x[1] = 0x51; return; /* GetProcessId */
    case 0x25: { /* GetThreadId(handle) */
        KThread *t = (uint32_t)x[1] == HANDLE_CUR_THREAD ? kthread_current() : kobj_get((uint32_t)x[1], KO_THREAD);
        x[0] = t ? 0 : RES_INVALID_HANDLE;
        x[1] = t ? t->id : 0;
        return;
    }
    case 0x29: svc_get_info(c); return;
    case 0x2A: case 0x2B: x[0] = 0; return; /* FlushEntireDataCache / FlushDataCache */
    case 0x2C: x[0] = svc_map_physical(c); guest_map_bump(); return; /* after the change too: see g_map_epoch */
    case 0x2D: x[0] = svc_unmap_physical(c); guest_map_bump(); return; /* after the change too: see g_map_epoch */
    case 0x32: x[0] = kthread_set_activity((uint32_t)x[0], (uint32_t)x[1]); return;
    case 0x33: x[0] = kthread_get_context((uint32_t)x[1], (void *)x[0]); return;
    case 0x34: x[0] = sync_wait_for_address(x[0], (uint32_t)x[1], (int32_t)x[2], timeout_arg(x[3])); return;
    case 0x35: x[0] = sync_signal_to_address(x[0], (uint32_t)x[1], (int32_t)x[2], (int32_t)x[3]); return;
    case 0x36: x[0] = 0; return; /* SynchronizePreemptionState */
    case 0x45: { /* CreateEvent -> writable x1, readable x2 */
        uint32_t w = 0, r = 0;
        x[0] = kevent_create_handles(&w, &r);
        x[1] = w;
        x[2] = r;
        return;
    }
    case 0x1F: { /* ConnectToNamedPort(name) -> handle */
        char port[16] = {0};
        memcpy(port, (const void *)x[1], 12);
        uint32_t h = 0;
        x[0] = ipc_connect_named_port(port, &h);
        x[1] = h;
        return;
    }
    case 0x73: x[0] = svc_set_process_permission(c); guest_map_bump(); return; /* after the change too: see g_map_epoch */
    case 0x74: x[0] = svc_map_process_memory(c); guest_map_bump(); return; /* after the change too: see g_map_epoch */
    case 0x75: x[0] = svc_unmap_process_memory(c); guest_map_bump(); return; /* after the change too: see g_map_epoch */
    case 0x40: gs_create(c); return;
    case 0x43: x[0] = gs_receive(c); return;
    case 0x21: {
        GSess *g = kobj_get((uint32_t)x[0], KO_GS_CLIENT);
        if (g) { x[0] = gs_send(c, g); return; }
        x[0] = ipc_send_sync_request((uint32_t)x[0], (uint32_t *)c->tlr);
        return;
    }
    case 0x22: x[0] = ipc_send_sync_request((uint32_t)x[2], (uint32_t *)x[0]); return;
    case 0x27: { /* OutputDebugString */
        char buf[256];
        size_t len = x[1] < sizeof buf - 1 ? x[1] : sizeof buf - 1;
        memcpy(buf, (const void *)x[0], len);
        buf[len] = 0;
        som_log("guest debug: %s", buf);
        x[0] = 0;
        return;
    }
    case 0x26: /* Break */
        if (x[0] & 0x80000000u) { /* notification only: the guest continues */
            som_log("guest Break notification (reason 0x%llx)", (unsigned long long)x[0]);
            return;
        }
        som_stop("Break(reason 0x%llx, addr 0x%llx, size 0x%llx)", (unsigned long long)x[0], (unsigned long long)x[1],
                 (unsigned long long)x[2]);
    default:
        som_stop("unimplemented svc 0x%02x (%s)", n, name);
    }
}

void nvn_wd_register(GuestCtx *c);
void som_svc_dispatch(GuestCtx *c) {
    uint32_t n = (uint32_t)c->svc_num;
    nvn_wd_register(c);
    if (n & 0x8000) { /* a hooked SDK function, not a kernel call */
        hook_dispatch(n & 0x7FFF, c);
        kthread_check_pause();
        return;
    }
    {
        struct timespec a_, b_;
        clock_gettime(CLOCK_MONOTONIC, &a_);
        svc_exec(c);
        clock_gettime(CLOCK_MONOTONIC, &b_);
        if (n < 0x80) {
            __atomic_fetch_add(&g_svc_ns[n], (uint64_t)((b_.tv_sec - a_.tv_sec) * 1000000000ll + (b_.tv_nsec - a_.tv_nsec)), __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_svc_cnt[n], 1, __ATOMIC_RELAXED);
        }
    }
    som_trace("      -> x0=%llx x1=%llx x2=%llx", (unsigned long long)c->x[0], (unsigned long long)c->x[1],
              (unsigned long long)c->x[2]);
    kthread_check_pause();
}

/* ---- fatal signal reporting ----
 * ART installs its own SIGSEGV handler ahead of anything registered through
 * libc, and it reads its thread from tpidr_el0, which holds our GuestCtx while
 * guest code runs. So our handler goes in with a raw rt_sigaction, runs first
 * (after the asm shim restores the host TLS), reports faults on guest threads,
 * and hands everything else, and its own reports, to the previous handler. */

struct ksigaction {
    void *handler;
    unsigned long flags;
    void *restorer;
    uint64_t mask;
};
static struct ksigaction g_old[65];

static void chain(int sig, siginfo_t *info, void *uc) {
    struct ksigaction *o = &g_old[sig];
    if (o->handler == SIG_DFL || o->handler == SIG_IGN || !o->handler) {
        struct ksigaction dfl = {SIG_DFL, 0, NULL, 0};
        syscall(SYS_rt_sigaction, sig, &dfl, NULL, 8); /* refault under the default action */
        return;
    }
    if (o->flags & SA_SIGINFO) ((void (*)(int, siginfo_t *, void *))o->handler)(sig, info, uc);
    else ((void (*)(int))o->handler)(sig);
}

void som_crash_handler(int sig, siginfo_t *info, void *uc, int in_guest) {
    (void)in_guest;
    if (!t_ctx) { /* not a guest thread: not ours (but the opt-in crash report still gets the faulting library+offset) */
        ucontext_t *hu = (ucontext_t *)uc;
        Dl_info di;
        if (dladdr((void *)hu->uc_mcontext.pc, &di) && di.dli_fname && di.dli_fbase) {
            const char *base = strrchr(di.dli_fname, '/');
            char rep[256];
            snprintf(rep, sizeof rep, "kind=host\nsignal=%d\nframe0=%s+0x%llx\n", sig, base ? base + 1 : di.dli_fname,
                     (unsigned long long)(hu->uc_mcontext.pc - (uint64_t)di.dli_fbase));
            tel_write_crash(rep);
        }
        chain(sig, info, uc);
        return;
    }
    ucontext_t *u = (ucontext_t *)uc;
    uint64_t pc = u->uc_mcontext.pc, off = 0;
    const char *m = som_module_of(pc, &off);
    {
        char rep[1024];
        int n = snprintf(rep, sizeof rep, "kind=guest\nsignal=%d\nframe0=%s+0x%llx\n", sig, m, (unsigned long long)off);
        uint64_t rlr = u->uc_mcontext.regs[30], roff = 0;
        const char *rm = som_module_of(rlr, &roff);
        n += snprintf(rep + n, sizeof rep - n, "frame1=%s+0x%llx\n", rm, (unsigned long long)roff);
        uint64_t rfp = u->uc_mcontext.regs[29];
        for (int k = 2; k < 10 && rfp && !(rfp & 7) && rfp >= AS_BASE && rfp < AS_END && n < (int)sizeof rep - 80; k++) {
            uint64_t nx = ((uint64_t *)rfp)[0], flr = ((uint64_t *)rfp)[1];
            n += snprintf(rep + n, sizeof rep - n, "frame%d=%s+0x%llx\n", k, som_module_of(flr, &roff), (unsigned long long)roff);
            if (nx <= rfp || nx - rfp > (64u << 20)) break;
            rfp = nx;
        }
        tel_write_crash(rep);
    }
    som_log("CRASH: signal %d fault addr %p pc 0x%llx (%s+0x%llx) sp 0x%llx", sig, info->si_addr,
            (unsigned long long)pc, m, (unsigned long long)off, (unsigned long long)u->uc_mcontext.sp);
    for (int i = 0; i < 31; i++) som_log("  x%d=0x%llx", i, (unsigned long long)u->uc_mcontext.regs[i]);
    uint64_t lr = u->uc_mcontext.regs[30];
    som_log("  lr = %s+0x%llx", som_module_of(lr, &off), (unsigned long long)off);
    uint64_t fp = u->uc_mcontext.regs[29];
    for (int n = 0; n < 12 && fp && !(fp & 7) && fp >= AS_BASE && fp < AS_END; n++) {
        uint64_t next = ((uint64_t *)fp)[0], flr = ((uint64_t *)fp)[1];
        som_log("  frame %2d: %s+0x%llx", n, som_module_of(flr, &off), (unsigned long long)off);
        if (next <= fp || next - fp > (64u << 20)) break;
        fp = next;
    }
    chain(sig, info, uc);
}

void som_install_crash_handlers(void) {
    static int done;
    if (__atomic_exchange_n(&done, 1, __ATOMIC_SEQ_CST)) return;
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        struct ksigaction act = {(void *)som_crash_shim, SA_SIGINFO | SA_ONSTACK | SA_NODEFER, NULL, 0};
        syscall(SYS_rt_sigaction, sigs[i], &act, &g_old[sigs[i]], 8);
    }
    struct ksigaction sus = {(void *)som_suspend_shim, SA_SIGINFO | SA_ONSTACK, NULL, 0};
    syscall(SYS_rt_sigaction, SOM_SIG_SUSPEND, &sus, &g_old[SOM_SIG_SUSPEND], 8);
}








