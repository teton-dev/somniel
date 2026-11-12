/* Which Vulkan driver the renderer runs on.
 *
 * Default: the system driver (/system/lib64/libvulkan.so and the vendor driver behind it). A custom driver (Mesa Turnip,
 * other Qualcomm driver builds: the usual adrenotools package, a .so plus meta.json) is loaded with libadrenotools,
 * which opens a private copy of the Vulkan loader in its own linker namespace and points it at the custom driver.
 *
 *  - Installed drivers live in <internal files>/gpu_drivers/<name>/ (dlopen refuses libraries on shared storage).
 *  - <external data>/gpu_driver_import/<name>/ (meta.json + .so, e.g. pushed with adb) is copied there at start and
 *    then removed.
 *  - <external data>/gpu_driver holds the name of the driver to use; no file, an empty one or "system" = system driver.
 *  - If the custom driver cannot be opened the system driver is used (logged), so a bad driver never stops the game.
 *
 * All Vulkan calls go through volk's function pointers, initialised from the chosen driver's vkGetInstanceProcAddr. */
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include <adrenotools/driver.h>
#include <vulkan/vulkan.h>
#include "volk.h"

#include "debug_log.h"

char g_gpu_driver_name[128]; /* "" = system driver */

static int copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : NULL;
    int ok = a && b;
    char buf[1 << 16];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, a)) > 0) ok = fwrite(buf, 1, n, b) == n;
    if (a) fclose(a);
    if (b && fclose(b)) ok = 0;
    return ok;
}

/* <ext>/gpu_driver_import/<name>/ -> <internal>/gpu_drivers/<name>/ */
static void import_drivers(const char *ext, const char *in) {
    char src[700], dst[700];
    snprintf(src, sizeof src, "%s/gpu_driver_import", ext);
    DIR *d = opendir(src);
    if (!d) return;
    snprintf(dst, sizeof dst, "%s/gpu_drivers", in);
    mkdir(dst, 0700);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sd[900], dd[900];
        snprintf(sd, sizeof sd, "%s/%s", src, e->d_name);
        snprintf(dd, sizeof dd, "%s/%s", dst, e->d_name);
        DIR *f = opendir(sd);
        if (!f) continue;
        mkdir(dd, 0700);
        struct dirent *fe;
        int n = 0, bad = 0;
        while ((fe = readdir(f))) {
            if (fe->d_name[0] == '.') continue;
            char a[1200], b[1200];
            snprintf(a, sizeof a, "%s/%s", sd, fe->d_name);
            snprintf(b, sizeof b, "%s/%s", dd, fe->d_name);
            if (copy_file(a, b)) n++, unlink(a);
            else bad++;
        }
        closedir(f);
        if (!bad) rmdir(sd);
        som_log("gpu driver: imported %s (%d files%s)", e->d_name, n, bad ? ", some failed" : "");
    }
    closedir(d);
}

/* "libraryName" from meta.json, else the first .so in the folder */
static int driver_library(const char *dir, char *out, size_t cap) {
    char p[800];
    snprintf(p, sizeof p, "%s/meta.json", dir);
    FILE *f = fopen(p, "r");
    if (f) {
        char js[4096];
        size_t n = fread(js, 1, sizeof js - 1, f);
        fclose(f);
        js[n] = 0;
        const char *k = strstr(js, "\"libraryName\"");
        const char *q = k ? strchr(k + 13, '"') : NULL;
        const char *e = q ? strchr(q + 1, '"') : NULL;
        if (e && (size_t)(e - q - 1) < cap) {
            memcpy(out, q + 1, (size_t)(e - q - 1));
            out[e - q - 1] = 0;
            return 1;
        }
    }
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l > 3 && !strcmp(e->d_name + l - 3, ".so") && l < cap) {
            memcpy(out, e->d_name, l + 1);
            closedir(d);
            return 1;
        }
    }
    if (d) closedir(d);
    return 0;
}

PFN_vkGetInstanceProcAddr gpu_driver_open(void) {
    const char *ext = SDL_GetAndroidExternalStoragePath(), *in = SDL_GetAndroidInternalStoragePath();
    char name[128] = "";
    if (ext && in) {
        import_drivers(ext, in);
        char p[700];
        snprintf(p, sizeof p, "%s/gpu_driver", ext);
        FILE *f = fopen(p, "r");
        if (f) {
            if (fscanf(f, "%127s", name) != 1) name[0] = 0;
            fclose(f);
        }
        if (!strcmp(name, "system")) name[0] = 0;
    }
    if (name[0]) {
        char dir[700], lib[256], hooks[700];
        snprintf(dir, sizeof dir, "%s/gpu_drivers/%s/", in, name);
        Dl_info di;
        hooks[0] = 0;
        if (dladdr((void *)gpu_driver_open, &di) && di.dli_fname) {
            snprintf(hooks, sizeof hooks, "%s", di.dli_fname);
            char *s = strrchr(hooks, '/');
            if (s) s[1] = 0;
        }
        if (!driver_library(dir, lib, sizeof lib)) {
            som_log("gpu driver: %s not installed (no %s with a .so): using the system driver", name, dir);
        } else if (!hooks[0] || strstr(hooks, ".apk!")) {
            som_log("gpu driver: native libraries are not extracted (%s): using the system driver", hooks);
        } else {
            void *h = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL, hooks, dir, lib, NULL, NULL);
            PFN_vkGetInstanceProcAddr gipa = h ? (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr") : NULL;
            if (gipa) {
                snprintf(g_gpu_driver_name, sizeof g_gpu_driver_name, "%s", name);
                som_log("gpu driver: custom %s (%s%s)", name, dir, lib);
                return gipa;
            }
            som_log("gpu driver: could not open %s%s (%s): using the system driver", dir, lib, dlerror());
        }
    }
    void *h = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    PFN_vkGetInstanceProcAddr gipa = h ? (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr") : NULL;
    som_log("gpu driver: system%s", gipa ? "" : " (libvulkan.so not usable!)");
    return gipa;
}
