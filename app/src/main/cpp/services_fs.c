#include <stdio.h>
#include <time.h>
#include <pthread.h>
/* fsp-srv: the file system proxy. Only what the game has asked for so far:
 * the program's RomFS as an IStorage served from the packed image
 * <data dir>/romfs.bin (tools/pack_romfs.py), and the access-log queries. */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "debug_log.h"
#include "guest.h"
#include "ipc.h"
#include "movies.h"

static char g_data_dir[400];

const char *som_data_dir(void) { return g_data_dir; }
void services_set_data_dir(const char *dir) { snprintf(g_data_dir, sizeof g_data_dir, "%s", dir); }

/* Where exefs/ and romfs.bin live: the data dir, or another volume (an SD card) chosen in the setup screen. Saves, flags
 * and caches always stay in the data dir. */
static char g_game_dir[400];
const char *som_game_dir(void) { return g_game_dir[0] ? g_game_dir : g_data_dir; }
void services_set_game_dir(const char *dir) { snprintf(g_game_dir, sizeof g_game_dir, "%s", dir); }

/* ---- IStorage over a host file ---- */

typedef struct {
    int fd;
    uint64_t size;
} FileStorage;

static void storage_close(void *state) {
    FileStorage *s = state;
    if (s->fd >= 0) close(s->fd);
    free(s);
}

static void storage_handler(IpcObject *self, const IpcRequest *rq, IpcResponse *rs) {
    FileStorage *s = self->state;
    switch (rq->cmd) {
    case 0: { /* Read(offset, size) into an output buffer */
        uint64_t off = IPC_ARG(rq, uint64_t, 0), size = IPC_ARG(rq, uint64_t, 8);
        if (rq->nb < 1 || rq->b[0].size < size) som_stop("fs: IStorage::Read with no output buffer (%u) for 0x%llx bytes", rq->nb, (unsigned long long)size);
        if (off + size > s->size || off + size < off) {
            rs->result = 0x2F8002; /* out of range (fs module 2, description 3005) */
            return;
        }
        static int read_log = -1; /* <data>/fs_reads.on: write the read log */
        if (read_log < 0) {
            char p[600];
            snprintf(p, sizeof p, "%s/fs_reads.on", g_data_dir);
            read_log = access(p, F_OK) == 0;
        }
        if (read_log) {   /* read log (files/fs_reads.log): one line per new region, mapped to file names offline */
            static FILE *rl;
            static uint64_t last_end;
            static pthread_mutex_t rl_mu = PTHREAD_MUTEX_INITIALIZER;
            pthread_mutex_lock(&rl_mu);
            if (!rl) {
                char p[600];
                snprintf(p, sizeof p, "%s/fs_reads.log", g_data_dir);
                rl = fopen(p, "w");
            }
            if (rl && off != last_end) {
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                fprintf(rl, "%lld.%03ld %llx %llx\n", (long long)ts.tv_sec, ts.tv_nsec / 1000000, (unsigned long long)off,
                        (unsigned long long)size);
                fflush(rl);
            }
            last_end = off + size;
            pthread_mutex_unlock(&rl_mu);
        }
        movies_note_read(off, size); /* which movie the game is about to play (movies.c) */
        uint8_t *dst = ipc_guest(rq->b[0].addr, size);
        uint64_t done = 0;
        extern uint64_t g_fs_read_ns, g_fs_read_bytes, g_fs_read_calls;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (done < size) {
            ssize_t n = pread(s->fd, dst + done, size - done, (off_t)(off + done));
            if (n <= 0) {
                som_log("fs: pread failed at 0x%llx: %s", (unsigned long long)(off + done), strerror(errno));
                rs->result = 0x2F8002;
                return;
            }
            done += (uint64_t)n;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        __atomic_fetch_add(&g_fs_read_ns, (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec)), __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_fs_read_bytes, size, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_fs_read_calls, 1, __ATOMIC_RELAXED);
        return;
    }
    case 2: return; /* Flush */
    case 4: {       /* GetSize */
        IPC_OUT(rs, s->size);
        return;
    }
    default: ipc_unimplemented(self, rq);
    }
}

static int open_romfs(IpcResponse *rs, const IpcRequest *rq) {
    char path[512];
    snprintf(path, sizeof path, "%s/romfs.bin", som_game_dir());
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        som_log("fs: cannot open %s: %s", path, strerror(errno));
        rs->result = 0x2F8002;
        return 0;
    }
    struct stat st;
    fstat(fd, &st);
    FileStorage *s = calloc(1, sizeof *s);
    s->fd = fd;
    s->size = (uint64_t)st.st_size;
    som_log("fs: serving RomFS image %s (%llu bytes)", path, (unsigned long long)s->size);
    ipc_return_object(rq, rs, "fs:romfs", storage_handler, s, storage_close);
    return 1;
}

/* ---- fsp-srv ---- */

static void fsp_handler(IpcObject *self, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 1: return;    /* SetCurrentProcess(pid) */
    case 18:           /* OpenSdCardFileSystem -> IFileSystem (<data>/sd) */
        hostfs_open_sd(g_data_dir, rq, rs);
        return;
    case 22:           /* CreateSaveDataFileSystem(attr 0x40, creation info 0x40, meta 0x20) */
        hostfs_create_save(g_data_dir, ipc_arg(rq, 0, 0x40));
        return;
    case 51:           /* OpenSaveDataFileSystem(u8 space, attr 0x40) -> IFileSystem */
        hostfs_open_save(g_data_dir, ipc_arg(rq, 8, 0x40), rq, rs);
        return;
    case 200:          /* OpenDataStorageByCurrentProcess -> IStorage */
        open_romfs(rs, rq);
        return;
    case 1005: {       /* GetGlobalAccessLogMode */
        uint32_t mode = 0;
        IPC_OUT(rs, mode);
        return;
    }
    case 1006: return; /* OutputAccessLogToSdCard */
    default: ipc_unimplemented(self, rq);
    }
}

void services_fs_register(void) { ipc_register_service("fsp-srv", fsp_handler); }
