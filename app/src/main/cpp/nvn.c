/* NVN entry points: nvnBootstrapLoader and nvnDeviceGetProcAddress answer from the proc table in nvn/nvn_procs.c. */

#include "nvn.h"

#include "nvn/gfx.h"

uint64_t nvn_resolve_proc(const char *name) { return nvn_proc_address(name); }
