#pragma once
/* In-process Horizon IPC: HIPC messages carrying CMIF (plain or domain) or
 * TIPC payloads, answered in place in the caller's message buffer. Services
 * are host objects reached through sm:.
 *
 * Message framing follows libnx sf/hipc.h and cmif.h (ISC, layout only) and the
 * parsing approach of the MacNativeGames TotK recomp ipc.c (MIT, Copyright (c)
 * 2026 MacNativeGames contributors), rewritten for domain sessions and
 * native execution. */

#include <stddef.h>
#include <stdint.h>

#define IPC_MAX_BUF 8
#define IPC_MAX_OBJS 8
#define IPC_MSG_BYTES 0x100
#define IPC_R_UNKNOWN_COMMAND 0xF60Au /* sf: module 10, description 123 */

typedef struct IpcBuffer {
    uint64_t addr, size;
    unsigned index;
} IpcBuffer;

struct IpcSession;
struct IpcObject;

typedef struct IpcRequest {
    unsigned type, cmd; /* HIPC type (4/6 request, 5/7 control, >=16 TIPC) and command id */
    struct IpcSession *session;
    int has_pid;
    uint64_t pid;
    uint32_t copy[8], move[8];
    unsigned ncopy, nmove;
    IpcBuffer x[IPC_MAX_BUF], a[IPC_MAX_BUF], b[IPC_MAX_BUF], w[IPC_MAX_BUF], c[IPC_MAX_BUF];
    unsigned nx, na, nb, nw, nc;
    uint32_t in_objs[IPC_MAX_OBJS]; /* domain object ids passed as arguments */
    unsigned nobjs;
    const uint8_t *data; /* command arguments after the CMIF header */
    uint32_t data_size;
} IpcRequest;

typedef struct IpcResponse {
    uint32_t result;
    uint8_t data[0x100];
    uint32_t data_size;
    uint32_t copy[8], move[8];
    unsigned ncopy, nmove;
    IpcBuffer xout[IPC_MAX_BUF];
    unsigned nxout;
    uint32_t objs[IPC_MAX_OBJS]; /* domain object ids returned */
    unsigned nobjs;
} IpcResponse;

typedef void (*IpcHandler)(struct IpcObject *self, const IpcRequest *rq, IpcResponse *rs);

typedef struct IpcObject {
    const char *name;
    IpcHandler handler;
    void *state;
    void (*close)(void *state);
} IpcObject;

#define IPC_MAX_DOMAIN_OBJS 256

typedef struct IpcSession {
    IpcObject root;
    int refs;
    int is_domain;
    IpcObject *objs[IPC_MAX_DOMAIN_OBJS]; /* ids start at 1; objs[1] is the root once converted */
} IpcSession;

/* entry points used by the svc layer */
uint32_t ipc_connect_named_port(const char *name, uint32_t *handle_out);
uint32_t ipc_send_sync_request(uint32_t handle, uint32_t *msg);
void ipc_close_session(IpcSession *s);

/* helpers for service implementations */
void ipc_register_service(const char *name, IpcHandler handler);
uint32_t ipc_new_session_handle(const char *name, IpcHandler handler, void *state, void (*close)(void *));
void ipc_return_object(const IpcRequest *rq, IpcResponse *rs, const char *name, IpcHandler handler, void *state,
                       void (*close)(void *));
IpcObject *ipc_domain_object(const IpcRequest *rq, uint32_t id);
const void *ipc_arg(const IpcRequest *rq, uint32_t off, uint32_t n);
void ipc_out(IpcResponse *rs, const void *p, uint32_t n);
void *ipc_guest(uint64_t addr, uint64_t size);
__attribute__((noreturn)) void ipc_unimplemented(const IpcObject *self, const IpcRequest *rq);
#define IPC_OUT(rs, v) ipc_out((rs), &(v), sizeof(v))
#define IPC_ARG(rq, T, off) (*(const T *)ipc_arg((rq), (off), sizeof(T)))

/* services (services*.c) */
void services_register_all(void);
void services_fs_register(void);
void services_audio_register(void);
void hostfs_create_save(const char *base, const uint8_t *attr);
int hostfs_open_save(const char *base, const uint8_t *attr, const IpcRequest *rq, IpcResponse *rs);
int hostfs_open_sd(const char *base, const IpcRequest *rq, IpcResponse *rs);
void services_set_data_dir(const char *dir);
void services_set_game_dir(const char *dir);   /* where exefs/ and romfs.bin are, if not in the data dir */
const char *som_game_dir(void);
void ipc_stub_handler(IpcObject *self, const IpcRequest *rq, IpcResponse *rs);

