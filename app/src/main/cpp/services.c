/* Service registry: every service sm: can hand out is registered here.
 * Services the guest asks for that are not implemented yet get a logging stub
 * (result 0, no outputs), so one run lists every service and command the game
 * touches. Stubs are a census tool, not an implementation. */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "debug_log.h"
#include "guest.h"
#include "ipc.h"

void ipc_stub_handler(IpcObject *self, const IpcRequest *rq, IpcResponse *rs) {
    (void)rs;
    som_log("STUB %s cmd %u (type %u, data %u bytes, x%u a%u b%u w%u c%u, copy %u move %u)", self->name, rq->cmd,
            rq->type, rq->data_size, rq->nx, rq->na, rq->nb, rq->nw, rq->nc, rq->ncopy, rq->nmove);
    if (rq->data_size >= 8) {
        uint64_t a = 0;
        memcpy(&a, rq->data, 8);
        som_log("  first arg 0x%llx", (unsigned long long)a);
    }
}

/* ---- lm: logging ---- */

static void logger_handler(IpcObject *self, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: { /* Log(buffer a[0]) */
        if (rq->na < 1) return;
        const uint8_t *p = ipc_guest(rq->a[0].addr, rq->a[0].size);
        char text[200];
        size_t n = 0;
        for (size_t i = 0; i < rq->a[0].size && n < sizeof text - 1; i++)
            if (isprint(p[i]) || p[i] == ' ') text[n++] = (char)p[i];
        text[n] = 0;
        if (n > 4) som_log("guest log: %s", text);
        return;
    }
    case 1: return; /* SetDestination */
    default: ipc_unimplemented(self, rq);
    }
}

static void lm_handler(IpcObject *self, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: /* OpenLogger(pid) -> ILogger */
        ipc_return_object(rq, rs, "lm:logger", logger_handler, NULL, NULL);
        return;
    default: ipc_unimplemented(self, rq);
    }
}

void services_register_all(void) {
    ipc_register_service("lm", lm_handler);
    services_fs_register();
    services_audio_register();
}

