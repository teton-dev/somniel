#include "kobj.h"

#include <pthread.h>

#define MAX_HANDLES 16384

static struct {
    int kind;
    uint32_t gen;
    void *obj;
} g_tab[MAX_HANDLES];
static int g_free[MAX_HANDLES];
static int g_nfree = -1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void init_free(void) {
    g_nfree = 0;
    for (int i = MAX_HANDLES - 1; i >= 1; i--) g_free[g_nfree++] = i;
}

uint32_t kobj_new(int kind, void *obj) {
    pthread_mutex_lock(&g_lock);
    if (g_nfree < 0) init_free();
    if (g_nfree == 0) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    int i = g_free[--g_nfree];
    g_tab[i].kind = kind;
    g_tab[i].obj = obj;
    if (g_tab[i].gen == 0) g_tab[i].gen = 1;
    uint32_t h = (g_tab[i].gen << 15) | (uint32_t)i;
    pthread_mutex_unlock(&g_lock);
    return h;
}

static int lookup(uint32_t h) {
    uint32_t i = h & 0x7FFF;
    if (i == 0 || i >= MAX_HANDLES || g_tab[i].kind == KO_NONE || g_tab[i].gen != (h >> 15)) return -1;
    return (int)i;
}

void *kobj_get(uint32_t h, int kind) {
    pthread_mutex_lock(&g_lock);
    int i = lookup(h);
    void *o = (i >= 0 && (kind == KO_NONE || g_tab[i].kind == kind)) ? g_tab[i].obj : NULL;
    pthread_mutex_unlock(&g_lock);
    return o;
}

int kobj_kind(uint32_t h) {
    pthread_mutex_lock(&g_lock);
    int i = lookup(h);
    int k = i >= 0 ? g_tab[i].kind : KO_NONE;
    pthread_mutex_unlock(&g_lock);
    return k;
}

void *kobj_take(uint32_t h, int *kind) {
    pthread_mutex_lock(&g_lock);
    int i = lookup(h);
    void *o = NULL;
    if (i >= 0) {
        o = g_tab[i].obj;
        if (kind) *kind = g_tab[i].kind;
        g_tab[i].kind = KO_NONE;
        g_tab[i].obj = NULL;
        g_tab[i].gen = (g_tab[i].gen + 1) & 0x3FFF;
        if (g_tab[i].gen == 0) g_tab[i].gen = 1;
        g_free[g_nfree++] = i;
    }
    pthread_mutex_unlock(&g_lock);
    return o;
}
