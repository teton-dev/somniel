/* IFileSystem / IFile / IDirectory over a host directory: the save data file system.
 * Command ids and struct layouts follow the public switchbrew FS service notes. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "debug_log.h"
#include "guest.h"
#include "ipc.h"

#define FSR(desc) (2u | ((uint32_t)(desc) << 9))
#define FS_NOT_FOUND FSR(1)
#define FS_EXISTS FSR(2)
#define FS_NOT_EMPTY FSR(1002)

typedef struct { char root[480]; } HostFs;
typedef struct { int fd; } HostFile;
typedef struct { DIR *d; char path[512]; } HostDir;

static void hostfs_close(void *st) { free(st); }

static const char *guest_path(const IpcBuffer *b, char *tmp, size_t cap) {
    const char *p = ipc_guest(b->addr, b->size);
    size_t len = strnlen(p, b->size);
    if (len >= cap) return NULL;
    memcpy(tmp, p, len);
    tmp[len] = 0;
    if (strstr(tmp, "..")) return NULL;
    char *q = tmp;
    while (*q == '/') q++;
    size_t n = strlen(q);
    while (n && q[n - 1] == '/') q[--n] = 0;
    return q;
}

/* the i-th path buffer -> host path under root; 0 on a bad path */
static int host_path(const HostFs *fs, const IpcRequest *rq, unsigned i, char *out, size_t cap) {
    if (rq->nx <= i) som_stop("fs: IFileSystem command %u without path buffer %u", rq->cmd, i);
    char tmp[0x400];
    const char *q = guest_path(&rq->x[i], tmp, sizeof tmp);
    if (!q) return 0;
    snprintf(out, cap, *q ? "%s/%s" : "%s%s", fs->root, q);
    return 1;
}

static uint32_t errno_result(void) {
    return errno == EEXIST ? FS_EXISTS : errno == ENOTEMPTY ? FS_NOT_EMPTY : FS_NOT_FOUND;
}

static void hostfile_close(void *st) {
    HostFile *f = st;
    if (f->fd >= 0) close(f->fd);
    free(f);
}

static void hostfile_handler(IpcObject *self, const IpcRequest *rq, IpcResponse *rs) {
    HostFile *f = self->state;
    switch (rq->cmd) {
    case 0: { /* Read(u32 option, s64 offset, s64 size) -> s64 read; output buffer */
        int64_t off = IPC_ARG(rq, int64_t, 8), size = IPC_ARG(rq, int64_t, 16);
        if (rq->nb < 1) som_stop("fs: IFile::Read without an output buffer");
        if (size > (int64_t)rq->b[0].size) size = (int64_t)rq->b[0].size;
        ssize_t n = size > 0 ? pread(f->fd, ipc_guest(rq->b[0].addr, (uint64_t)size), (size_t)size, (off_t)off) : 0;
        int64_t got = n < 0 ? 0 : n;
        IPC_OUT(rs, got);
        return;
    }
    case 1: { /* Write(u32 option, s64 offset, s64 size); input buffer */
        int64_t off = IPC_ARG(rq, int64_t, 8), size = IPC_ARG(rq, int64_t, 16);
        if (rq->na < 1) som_stop("fs: IFile::Write without an input buffer");
        if (size > (int64_t)rq->a[0].size) size = (int64_t)rq->a[0].size;
        const uint8_t *src = ipc_guest(rq->a[0].addr, (uint64_t)size);
        int64_t done = 0;
        while (done < size) {
            ssize_t n = pwrite(f->fd, src + done, (size_t)(size - done), (off_t)(off + done));
            if (n <= 0) {
                rs->result = FSR(2602);
                return;
            }
            done += n;
        }
        return;
    }
    case 2: fsync(f->fd); return; /* Flush */
    case 3: {                     /* SetSize */
        int64_t sz = IPC_ARG(rq, int64_t, 0);
        if (ftruncate(f->fd, (off_t)sz)) rs->result = FSR(2602);
        return;
    }
    case 4: { /* GetSize */
        struct stat st;
        fstat(f->fd, &st);
        int64_t sz = st.st_size;
        IPC_OUT(rs, sz);
        return;
    }
    default: ipc_unimplemented(self, rq);
    }
}

static void hostdir_close(void *st) {
    HostDir *d = st;
    if (d->d) closedir(d->d);
    free(d);
}

static void hostdir_handler(IpcObject *self, const IpcRequest *rq, IpcResponse *rs) {
    HostDir *d = self->state;
    switch (rq->cmd) {
    case 0: { /* Read -> s64 count; DirectoryEntry (0x310 bytes) records into the output buffer */
        if (rq->nb < 1) som_stop("fs: IDirectory::Read without an output buffer");
        uint64_t cap = rq->b[0].size / 0x310, n = 0;
        uint8_t *out = ipc_guest(rq->b[0].addr, rq->b[0].size);
        struct dirent *e;
        while (n < cap && (e = readdir(d->d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            uint8_t *ent = out + n * 0x310;
            memset(ent, 0, 0x310);
            snprintf((char *)ent, 0x300, "%s", e->d_name);
            char full[1100];
            struct stat st;
            snprintf(full, sizeof full, "%s/%s", d->path, e->d_name);
            int isdir = 0;
            int64_t size = 0;
            if (!stat(full, &st)) {
                isdir = S_ISDIR(st.st_mode);
                size = isdir ? 0 : st.st_size;
            }
            ent[0x304] = isdir ? 0 : 1;
            memcpy(ent + 0x308, &size, 8);
            n++;
        }
        int64_t cnt = (int64_t)n;
        IPC_OUT(rs, cnt);
        return;
    }
    case 1: { /* GetEntryCount */
        int64_t cnt = 0;
        DIR *c = opendir(d->path);
        struct dirent *e;
        while (c && (e = readdir(c)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) cnt++;
        if (c) closedir(c);
        IPC_OUT(rs, cnt);
        return;
    }
    default: ipc_unimplemented(self, rq);
    }
}

static void rm_rf(const char *path, int keep_root) {
    struct stat st;
    if (lstat(path, &st)) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char sub[1100];
            snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
            rm_rf(sub, 0);
        }
        if (d) closedir(d);
        if (!keep_root) rmdir(path);
    } else {
        unlink(path);
    }
}

static void hostfs_handler(IpcObject *self, const IpcRequest *rq, IpcResponse *rs) {
    HostFs *fs = self->state;
    char p[1100], p2[1100];
    struct stat st;
    switch (rq->cmd) {
    case 0: { /* CreateFile(u32 mode, s64 size, path) */
        int64_t size = IPC_ARG(rq, int64_t, 8);
        if (!host_path(fs, rq, 0, p, sizeof p)) { rs->result = FS_NOT_FOUND; return; }
        int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) { rs->result = errno_result(); return; }
        if (size > 0 && ftruncate(fd, (off_t)size)) rs->result = FSR(2602);
        close(fd);
        return;
    }
    case 1: /* DeleteFile */
        if (!host_path(fs, rq, 0, p, sizeof p) || unlink(p)) rs->result = FS_NOT_FOUND;
        return;
    case 2: /* CreateDirectory */
        if (!host_path(fs, rq, 0, p, sizeof p) || mkdir(p, 0755)) rs->result = errno_result();
        return;
    case 3: /* DeleteDirectory */
        if (!host_path(fs, rq, 0, p, sizeof p) || rmdir(p)) rs->result = errno_result();
        return;
    case 4: case 13: /* DeleteDirectoryRecursively / CleanDirectoryRecursively */
        if (!host_path(fs, rq, 0, p, sizeof p)) { rs->result = FS_NOT_FOUND; return; }
        rm_rf(p, rq->cmd == 13);
        return;
    case 5: case 6: /* RenameFile / RenameDirectory(old path, new path) */
        if (!host_path(fs, rq, 0, p, sizeof p) || !host_path(fs, rq, 1, p2, sizeof p2)) { rs->result = FS_NOT_FOUND; return; }
        if (rename(p, p2)) rs->result = errno_result();
        return;
    case 7: { /* GetEntryType -> u32 (0 directory, 1 file) */
        if (!host_path(fs, rq, 0, p, sizeof p) || stat(p, &st)) { rs->result = FS_NOT_FOUND; return; }
        uint32_t t = S_ISDIR(st.st_mode) ? 0 : 1;
        IPC_OUT(rs, t);
        return;
    }
    case 8: { /* OpenFile(u32 mode) -> IFile */
        uint32_t mode = IPC_ARG(rq, uint32_t, 0);
        if (!host_path(fs, rq, 0, p, sizeof p)) { rs->result = FS_NOT_FOUND; return; }
        int fd = open(p, (mode & 2) ? O_RDWR : O_RDONLY);
        if (fd < 0) { rs->result = FS_NOT_FOUND; return; }
        HostFile *f = calloc(1, sizeof *f);
        f->fd = fd;
        ipc_return_object(rq, rs, "fs:file", hostfile_handler, f, hostfile_close);
        return;
    }
    case 9: { /* OpenDirectory(u32 filter) -> IDirectory */
        if (!host_path(fs, rq, 0, p, sizeof p)) { rs->result = FS_NOT_FOUND; return; }
        DIR *dd = opendir(p);
        if (!dd) { rs->result = FS_NOT_FOUND; return; }
        HostDir *d = calloc(1, sizeof *d);
        d->d = dd;
        snprintf(d->path, sizeof d->path, "%s", p);
        ipc_return_object(rq, rs, "fs:dir", hostdir_handler, d, hostdir_close);
        return;
    }
    case 14: { /* GetFileTimeStampRaw(path) -> {created, modified, accessed: s64, is_valid: u8, pad} (0x20 bytes) */
        if (!host_path(fs, rq, 0, p, sizeof p) || stat(p, &st)) { rs->result = FS_NOT_FOUND; return; }
        int64_t ts[4] = {(int64_t)st.st_ctime, (int64_t)st.st_mtime, (int64_t)st.st_atime, 1};
        IPC_OUT(rs, ts);
        return;
    }
    case 10: return; /* Commit */
    case 11: case 12: { /* GetFreeSpaceSize / GetTotalSpaceSize */
        int64_t sz = 1ll << 33;
        IPC_OUT(rs, sz);
        return;
    }
    default: ipc_unimplemented(self, rq);
    }
}

/* SaveDataAttribute (0x40): program_id u64, user_id u128, system_save_data_id u64, type u8, rank u8, index u16 */
static void save_dir_for(const char *base, const uint8_t *attr, char *out, size_t cap) {
    uint64_t user_lo, user_hi, sysid;
    memcpy(&user_lo, attr + 8, 8);
    memcpy(&user_hi, attr + 16, 8);
    memcpy(&sysid, attr + 24, 8);
    uint16_t index;
    memcpy(&index, attr + 34, 2);
    snprintf(out, cap, "%s/save/t%u_%016llx%016llx_%llx_%u", base, attr[32], (unsigned long long)user_hi,
             (unsigned long long)user_lo, (unsigned long long)sysid, index);
}

static void mkdirs(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *q = tmp + 1; *q; q++)
        if (*q == '/') {
            *q = 0;
            mkdir(tmp, 0755);
            *q = '/';
        }
    mkdir(tmp, 0755);
}

void hostfs_create_save(const char *base, const uint8_t *attr) {
    char dir[512];
    save_dir_for(base, attr, dir, sizeof dir);
    mkdirs(dir);
    som_log("fs: save data created at %s", dir);
}

int hostfs_open_save(const char *base, const uint8_t *attr, const IpcRequest *rq, IpcResponse *rs) {
    char dir[512];
    struct stat st;
    save_dir_for(base, attr, dir, sizeof dir);
    if (stat(dir, &st)) { /* a fresh install: the save data exists as soon as it is asked for */
        mkdirs(dir);
        som_log("fs: new save data at %s", dir);
    }
    som_log("fs: save data opened at %s", dir);
    HostFs *fs = calloc(1, sizeof *fs);
    snprintf(fs->root, sizeof fs->root, "%s", dir);
    ipc_return_object(rq, rs, "fs:save", hostfs_handler, fs, hostfs_close);
    return 1;
}

/* The SD card file system (fsp-srv OpenSdCardFileSystem): <data>/sd, as a mod installer lays it out. */
int hostfs_open_sd(const char *base, const IpcRequest *rq, IpcResponse *rs) {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/sd", base);
    mkdirs(dir);
    HostFs *fs = calloc(1, sizeof *fs);
    snprintf(fs->root, sizeof fs->root, "%s", dir);
    ipc_return_object(rq, rs, "fs:sd", hostfs_handler, fs, hostfs_close);
    return 1;
}
