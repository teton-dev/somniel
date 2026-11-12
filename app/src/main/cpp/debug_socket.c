#include "debug_socket.h"

#include "debug_log.h"

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

SomStats g_som_stats;

static const char kName[] = "somniel_debug";

static void reply(int fd, const char *s) {
    size_t n = strlen(s);
    if (write(fd, s, n) < 0) return;
    if (write(fd, "\n", 1) < 0) return;
}

static void handle_line(int fd, const char *line) {
    char out[256];
    if (strstr(line, "\"ping\"")) {
        reply(fd, "{\"ok\":true,\"reply\":\"pong\"}");
    } else if (strstr(line, "\"stats\"")) {
        snprintf(out, sizeof out,
                 "{\"ok\":true,\"frames\":%llu,\"last_frame_ms\":%.3f,\"avg_fps\":%.2f}",
                 g_som_stats.frames, g_som_stats.last_frame_ms, g_som_stats.avg_fps);
        reply(fd, out);
    } else {
        reply(fd, "{\"ok\":false,\"error\":\"unknown command\"}");
    }
}

static void *server_thread(void *arg) {
    (void)arg;
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) return NULL;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path + 1, kName, sizeof kName - 1); /* abstract namespace */
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + sizeof kName - 1);
    if (bind(srv, (struct sockaddr *)&addr, len) < 0 || listen(srv, 2) < 0) {
        som_log("debug socket: bind failed");
        close(srv);
        return NULL;
    }
    som_log("debug socket: listening on @%s", kName);
    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) continue;
        char buf[512];
        size_t used = 0;
        ssize_t n;
        while ((n = read(c, buf + used, sizeof buf - 1 - used)) > 0) {
            used += (size_t)n;
            buf[used] = 0;
            char *nl;
            char *start = buf;
            while ((nl = strchr(start, '\n')) != NULL) {
                *nl = 0;
                handle_line(c, start);
                start = nl + 1;
            }
            used = strlen(start);
            memmove(buf, start, used + 1);
            if (used >= sizeof buf - 1) used = 0;
        }
        close(c);
    }
    return NULL;
}

void som_debug_socket_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, server_thread, NULL) == 0) pthread_detach(t);
}
