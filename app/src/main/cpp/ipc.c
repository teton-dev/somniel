#include <pthread.h>
#include "ipc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug_log.h"
#include "guest.h"
#include "kobj.h"
#include "thread.h"

#define CMIF_IN_MAGIC 0x49434653u  /* "SFCI" */
#define CMIF_OUT_MAGIC 0x4f434653u /* "SFCO" */
#define POINTER_BUFFER_SIZE 0x8000

enum { T_CLOSE = 2, T_REQUEST = 4, T_CONTROL = 5, T_REQUEST_CTX = 6, T_CONTROL_CTX = 7, T_TIPC_CLOSE = 15,
       T_TIPC_BASE = 16 };

/* ---- service registry ---- */

#define MAX_SERVICES 64
static pthread_mutex_t g_obj_mu = PTHREAD_MUTEX_INITIALIZER; /* domain object tables are shared by every thread of a session */
static struct {
    char name[16];
    IpcHandler handler;
} g_services[MAX_SERVICES];
static int g_nservices;

void ipc_register_service(const char *name, IpcHandler handler) {
    if (g_nservices < MAX_SERVICES) {
        snprintf(g_services[g_nservices].name, 16, "%s", name);
        g_services[g_nservices].handler = handler;
        g_nservices++;
    }
}

/* ---- sessions ---- */

uint32_t ipc_new_session_handle(const char *name, IpcHandler handler, void *state, void (*close)(void *)) {
    IpcSession *s = calloc(1, sizeof *s);
    if (!s) som_stop("ipc: session allocation failed");
    s->root = (IpcObject){name, handler, state, close};
    s->refs = 1;
    return kobj_new(KO_SESSION, s);
}

static uint32_t session_clone(IpcSession *s) {
    __atomic_add_fetch(&s->refs, 1, __ATOMIC_SEQ_CST);
    return kobj_new(KO_SESSION, s);
}

void ipc_close_session(IpcSession *s) {
    if (__atomic_sub_fetch(&s->refs, 1, __ATOMIC_SEQ_CST) != 0) return;
    if (s->root.close) s->root.close(s->root.state);
    for (int i = 2; i < IPC_MAX_DOMAIN_OBJS; i++)
        if (s->objs[i]) {
            if (s->objs[i]->close) s->objs[i]->close(s->objs[i]->state);
            free(s->objs[i]);
        }
    free(s);
}

IpcObject *ipc_domain_object(const IpcRequest *rq, uint32_t id) {
    IpcSession *s = rq->session;
    if (!s->is_domain || id == 0 || id >= IPC_MAX_DOMAIN_OBJS || !s->objs[id]) return NULL;
    return s->objs[id];
}

void ipc_return_object(const IpcRequest *rq, IpcResponse *rs, const char *name, IpcHandler handler, void *state,
                       void (*close)(void *)) {
    IpcSession *s = rq->session;
    if (s->is_domain) {
        IpcObject *o = calloc(1, sizeof *o);
        *o = (IpcObject){name, handler, state, close};
        pthread_mutex_lock(&g_obj_mu);
        for (uint32_t id = 2; id < IPC_MAX_DOMAIN_OBJS; id++)
            if (!s->objs[id]) {
                s->objs[id] = o;
                pthread_mutex_unlock(&g_obj_mu);
                rs->objs[rs->nobjs++] = id;
                return;
            }
        pthread_mutex_unlock(&g_obj_mu);
        som_stop("ipc: domain object table full");
    }
    rs->move[rs->nmove++] = ipc_new_session_handle(name, handler, state, close);
}

/* ---- helpers ---- */

void *ipc_guest(uint64_t addr, uint64_t size) {
    if ((!addr && size) || addr + size < addr || addr + size > AS_END) som_stop("ipc: bad client buffer 0x%llx+0x%llx", (unsigned long long)addr, (unsigned long long)size);
    return (void *)(uintptr_t)addr;
}

const void *ipc_arg(const IpcRequest *rq, uint32_t off, uint32_t n) {
    if (off + n > rq->data_size) som_stop("ipc: command %u argument %u+%u beyond %u bytes", rq->cmd, off, n, rq->data_size);
    return rq->data + off;
}

void ipc_out(IpcResponse *rs, const void *p, uint32_t n) {
    if (rs->data_size + n > sizeof rs->data) som_stop("ipc: response larger than %u bytes", (unsigned)sizeof rs->data);
    memcpy(rs->data + rs->data_size, p, n);
    rs->data_size += n;
}

void ipc_unimplemented(const IpcObject *self, const IpcRequest *rq) {
    som_stop("ipc: %s command %u not implemented (type %u, data %u bytes, x%u a%u b%u w%u c%u, copy %u move %u)",
             self->name, rq->cmd, rq->type, rq->data_size, rq->nx, rq->na, rq->nb, rq->nw, rq->nc, rq->ncopy,
             rq->nmove);
}

/* ---- HIPC framing ---- */

static uint64_t static_addr(const uint32_t *d) {
    return d[1] | ((uint64_t)((d[0] >> 12) & 0xf) << 32) | ((uint64_t)((d[0] >> 6) & 0x3f) << 36);
}
static uint64_t buffer_addr(const uint32_t *d) {
    return d[1] | ((uint64_t)((d[2] >> 28) & 0xf) << 32) | ((uint64_t)((d[2] >> 2) & 0x3fffff) << 36);
}
static uint64_t buffer_size(const uint32_t *d) { return d[0] | ((uint64_t)((d[2] >> 24) & 0xf) << 32); }

static void parse(const uint32_t *m, IpcRequest *rq, const uint32_t **data_words, unsigned *ndata) {
    rq->type = m[0] & 0xffff;
    rq->nx = (m[0] >> 16) & 0xf;
    rq->na = (m[0] >> 20) & 0xf;
    rq->nb = (m[0] >> 24) & 0xf;
    rq->nw = (m[0] >> 28) & 0xf;
    *ndata = m[1] & 0x3ff;
    unsigned recv_mode = (m[1] >> 10) & 0xf, special = m[1] >> 31;
    if (rq->nx > IPC_MAX_BUF || rq->na > IPC_MAX_BUF || rq->nb > IPC_MAX_BUF || rq->nw > IPC_MAX_BUF)
        som_stop("ipc: too many buffers (x%u a%u b%u w%u)", rq->nx, rq->na, rq->nb, rq->nw);
    const uint32_t *p = m + 2;
    if (special) {
        uint32_t h = *p++;
        rq->has_pid = h & 1;
        rq->ncopy = (h >> 1) & 0xf;
        rq->nmove = (h >> 5) & 0xf;
        if (rq->ncopy > 8 || rq->nmove > 8) som_stop("ipc: too many handles (copy %u move %u)", rq->ncopy, rq->nmove);
        if (rq->has_pid) {
            memcpy(&rq->pid, p, 8);
            p += 2;
        }
        for (unsigned i = 0; i < rq->ncopy; i++) rq->copy[i] = *p++;
        for (unsigned i = 0; i < rq->nmove; i++) rq->move[i] = *p++;
    }
    for (unsigned i = 0; i < rq->nx; i++, p += 2) rq->x[i] = (IpcBuffer){static_addr(p), p[0] >> 16, p[0] & 0x3f};
    IpcBuffer *lists[3] = {rq->a, rq->b, rq->w};
    unsigned counts[3] = {rq->na, rq->nb, rq->nw};
    for (unsigned k = 0; k < 3; k++)
        for (unsigned i = 0; i < counts[k]; i++, p += 3) lists[k][i] = (IpcBuffer){buffer_addr(p), buffer_size(p), i};
    *data_words = p;
    const uint32_t *recv = p + *ndata;
    rq->nc = recv_mode == 2 ? 1 : recv_mode > 2 ? recv_mode - 2 : 0;
    if (rq->nc > IPC_MAX_BUF) som_stop("ipc: %u receive buffers", rq->nc);
    for (unsigned i = 0; i < rq->nc; i++, recv += 2)
        rq->c[i] = (IpcBuffer){recv[0] | ((uint64_t)(recv[1] & 0xffff) << 32), recv[1] >> 16, i};
    if ((const uint8_t *)recv - (const uint8_t *)m > IPC_MSG_BYTES) som_stop("ipc: request exceeds the message buffer");
}

static void respond(uint32_t *m, int tipc, int domain, const IpcResponse *rs) {
    uint32_t out[IPC_MSG_BYTES / 4];
    memset(out, 0, sizeof out);
    unsigned special = rs->ncopy || rs->nmove;
    uint32_t *p = out + 2;
    if (special) {
        *p++ = (rs->ncopy << 1) | (rs->nmove << 5);
        for (unsigned i = 0; i < rs->ncopy; i++) *p++ = rs->copy[i];
        for (unsigned i = 0; i < rs->nmove; i++) *p++ = rs->move[i];
    }
    for (unsigned i = 0; i < rs->nxout; i++, p += 2) {
        uint64_t a = rs->xout[i].addr;
        p[0] = (rs->xout[i].index & 0x3f) | (uint32_t)(((a >> 36) & 0x3f) << 6) | (uint32_t)(((a >> 32) & 0xf) << 12) |
               (uint32_t)(rs->xout[i].size << 16);
        p[1] = (uint32_t)a;
    }
    uint8_t *words = (uint8_t *)p, *payload;
    payload = tipc ? words : (uint8_t *)out + (((words - (uint8_t *)out) + 15) & ~15);
    uint8_t *w = payload;
    if (domain) {
        uint32_t dh[4] = {rs->nobjs, 0, 0, 0};
        memcpy(w, dh, 16);
        w += 16;
    }
    if (tipc) {
        memcpy(w, &rs->result, 4);
        w += 4;
    } else {
        uint32_t h[4] = {CMIF_OUT_MAGIC, 0, rs->result, 0};
        memcpy(w, h, 16);
        w += 16;
    }
    memcpy(w, rs->data, rs->data_size);
    w += rs->data_size;
    if (domain) {
        memcpy(w, rs->objs, rs->nobjs * 4);
        w += rs->nobjs * 4;
    }
    unsigned used = (unsigned)(w - words);
    unsigned nwords = tipc ? (used + 3) / 4 : (used + 16 + 3) / 4; /* CMIF keeps alignment slack like libnx */
    if ((size_t)(words - (uint8_t *)out) + nwords * 4 > IPC_MSG_BYTES) som_stop("ipc: response exceeds the message buffer");
    out[0] = (rs->nxout & 0xf) << 16;
    out[1] = (nwords & 0x3ff) | (special ? 1u << 31 : 0);
    memcpy(m, out, IPC_MSG_BYTES);
}

/* CMIF control requests */
static void control(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: { /* ConvertCurrentObjectToDomain */
        if (!s->is_domain) {
            s->is_domain = 1;
            s->objs[1] = &s->root;
        }
        uint32_t id = 1;
        IPC_OUT(rs, id);
        return;
    }
    case 1: { /* CopyFromCurrentDomain(object id) */
        uint32_t id = IPC_ARG(rq, uint32_t, 0);
        if (id == 0 || id >= IPC_MAX_DOMAIN_OBJS || !s->objs[id]) {
            rs->result = 0x3F001; /* invalid domain object */
            return;
        }
        IpcSession *ns = calloc(1, sizeof *ns);
        ns->root = *s->objs[id];
        ns->root.close = NULL; /* state stays owned by the domain */
        ns->refs = 1;
        rs->move[rs->nmove++] = kobj_new(KO_SESSION, ns);
        return;
    }
    case 2: case 4: /* CloneCurrentObject(Ex) */
        rs->move[rs->nmove++] = session_clone(s);
        return;
    case 3: { /* QueryPointerBufferSize */
        uint16_t n = POINTER_BUFFER_SIZE;
        IPC_OUT(rs, n);
        return;
    }
    default:
        som_stop("ipc: %s control command %u not implemented", s->root.name, rq->cmd);
    }
}

static void dispatch(IpcSession *s, uint32_t *m) {
    IpcRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.session = s;
    const uint32_t *words;
    unsigned ndata;
    parse(m, &rq, &words, &ndata);
    if (rq.type == T_CLOSE || rq.type == T_TIPC_CLOSE) return;
    IpcResponse rs;
    memset(&rs, 0, sizeof rs);
    int tipc = rq.type >= T_TIPC_BASE;
    int is_control = rq.type == T_CONTROL || rq.type == T_CONTROL_CTX;
    int domain_msg = 0;
    IpcObject *target = &s->root;
    const uint8_t *bytes = (const uint8_t *)words, *end = bytes + ndata * 4;

    if (tipc) {
        rq.cmd = rq.type - T_TIPC_BASE;
        rq.data = bytes;
        rq.data_size = (uint32_t)(end - bytes);
    } else if (rq.type == T_REQUEST || rq.type == T_REQUEST_CTX || is_control) {
        const uint8_t *payload = (const uint8_t *)m + (((bytes - (const uint8_t *)m) + 15) & ~15);
        const uint8_t *cmif = payload;
        uint32_t dom_data_size = 0, obj_id = 0;
        unsigned dom_type = 0, in_objs = 0;
        if (s->is_domain && !is_control) {
            uint32_t dh[4];
            memcpy(dh, payload, 16);
            dom_type = dh[0] & 0xff;
            in_objs = (dh[0] >> 8) & 0xff;
            dom_data_size = dh[0] >> 16;
            obj_id = dh[1];
            domain_msg = 1;
            cmif = payload + 16;
            if (dom_type == 2) { /* close one object */
                if (obj_id > 1 && obj_id < IPC_MAX_DOMAIN_OBJS) {
                    pthread_mutex_lock(&g_obj_mu);
                    IpcObject *dead = s->objs[obj_id];
                    s->objs[obj_id] = NULL;
                    pthread_mutex_unlock(&g_obj_mu);
                    if (dead) {
                        if (dead->close) dead->close(dead->state);
                        free(dead);
                    }
                }
                respond(m, 0, 1, &rs);
                return;
            }
            target = obj_id < IPC_MAX_DOMAIN_OBJS ? s->objs[obj_id] : NULL;
            if (!target) som_stop("ipc: %s: request for unknown domain object %u", s->root.name, obj_id);
        }
        uint32_t h[4];
        memcpy(h, cmif, 16);
        if (h[0] != CMIF_IN_MAGIC) som_stop("ipc: %s: CMIF magic 0x%x", s->root.name, h[0]);
        rq.cmd = h[2];
        rq.data = cmif + 16;
        if (domain_msg) {
            rq.data_size = dom_data_size >= 16 ? dom_data_size - 16 : 0;
            const uint32_t *ids = (const uint32_t *)(cmif + dom_data_size);
            rq.nobjs = in_objs > IPC_MAX_OBJS ? IPC_MAX_OBJS : in_objs;
            for (unsigned i = 0; i < rq.nobjs; i++) rq.in_objs[i] = ids[i];
        } else {
            rq.data_size = rq.data <= end ? (uint32_t)(end - rq.data) : 0;
        }
    } else {
        som_log("ipc: %s message type %u:", s->root.name, rq.type);
        for (unsigned i = 0; i < 16; i++) som_log("  %08x", m[i]);
        som_stop("ipc: %s message type %u not implemented", s->root.name, rq.type);
    }

    if (is_control) control(s, &rq, &rs);
    else target->handler(target, &rq, &rs);
    som_trace("ipc: %s%s %s %u -> 0x%x (copy %u move %u objs %u)", target->name, domain_msg ? "[d]" : "",
              tipc ? "tipc" : is_control ? "control" : "cmd", rq.cmd, rs.result, rs.ncopy, rs.nmove, rs.nobjs);
    respond(m, tipc, domain_msg, &rs);
}

/* ---- sm: ---- */

static void sm_handler(IpcObject *self, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: return; /* RegisterClient / Initialize (pid) */
    case 1: {       /* GetServiceHandle(name) */
        char name[9] = {0};
        memcpy(name, ipc_arg(rq, 0, 8), 8);
        for (int i = 0; i < g_nservices; i++)
            if (!strcmp(g_services[i].name, name)) {
                rs->move[rs->nmove++] = ipc_new_session_handle(g_services[i].name, g_services[i].handler, NULL, NULL);
                som_log("ipc: opened service %s", g_services[i].name);
                return;
            }
        /* census mode: hand out a logging stub instead of stopping */
        som_log("ipc: service \"%s\" is not implemented, using a stub", name);
        {
            static char names[32][16];
            static int nn;
            char *keep = names[nn++ & 31];
            snprintf(keep, 16, "%s", name);
            rs->move[rs->nmove++] = ipc_new_session_handle(keep, ipc_stub_handler, NULL, NULL);
        }
        return;
    }
    default: ipc_unimplemented(self, rq);
    }
}

/* ---- svc entry points ---- */

uint32_t ipc_connect_named_port(const char *name, uint32_t *handle_out) {
    if (strcmp(name, "sm:")) som_stop("ConnectToNamedPort(\"%s\"): only sm: is served", name);
    *handle_out = ipc_new_session_handle("sm:", sm_handler, NULL, NULL);
    return *handle_out ? RES_OK : RES_OUT_OF_MEMORY;
}

uint32_t ipc_send_sync_request(uint32_t handle, uint32_t *msg) {
    IpcSession *s = kobj_get(handle, KO_SESSION);
    if (!s) {
        som_log("ipc: SendSyncRequest on 0x%x, not a session handle", handle);
        return RES_INVALID_HANDLE;
    }
    dispatch(s, msg);
    return RES_OK;
}
