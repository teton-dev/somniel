/* nn::swkbd::ShowKeyboard: text entry for the game (Engage asks for the player's name in the New Game flow, after
 * Appearance). On the Switch the call starts the keyboard library applet; Somniel has no applet, so the host answers
 * at once:
 *   1. the first line of <external files>/swkbd.txt (UTF-8) when that file exists ("<cancel>" cancels),
 *   2. else the game's initial text,
 *   3. else "Alear" (the default name).
 * An on-screen text dialog (Android EditText or an overlay) can later replace answer().
 *
 * The ShowKeyboardArg offsets, the UTF-8 conversion, the disabled-key filter and the cancel result follow
 * engage-pc-recomp runtime/swkbd.c, adapted from MacNativeGames games/totk/runtime/swkbd_host.c (MIT, Copyright (c)
 * 2026 MacNativeGames contributors). Engage 2.0.0 is built with the SDK whose KeyboardConfig is 0x4d0 bytes, so the
 * work buffer {pointer, size} follows at +0x4d0. */

#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "debug_log.h"
#include "hook.h"

const char *SDL_GetAndroidExternalStoragePath(void);
void *SDL_GetAndroidJNIEnv(void);
void *SDL_GetAndroidActivity(void);

#define KB_LIMIT 500
#define ARG_WORK 0x4d0
enum {
    KB_TYPE = 0x00, KB_DISABLED = 0x1c, KB_MAX_LEN = 0x3ac, KB_MIN_LEN = 0x3b0, KB_PASSWORD = 0x3b4, KB_UTF8 = 0x3bd,
    KB_INIT_OFF = 0x3c0, KB_INIT_LEN = 0x3c4, KB_TYPE_NUMPAD = 1, SWKBD_CANCELED = 0x29f,
};

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static size_t u16_len(const uint16_t *s, size_t max) { size_t n = 0; while (n < max && s[n]) n++; return n; }

static size_t utf16_to_utf8(const uint16_t *s, size_t n, char *out, size_t cap) {
    size_t o = 0;
    if (!cap) return 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (s[++i] - 0xdc00);
        else if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
        char b[4];
        size_t k;
        if (c < 0x80) { b[0] = (char)c; k = 1; }
        else if (c < 0x800) { b[0] = (char)(0xc0 | c >> 6); b[1] = (char)(0x80 | (c & 0x3f)); k = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xe0 | c >> 12); b[1] = (char)(0x80 | ((c >> 6) & 0x3f)); b[2] = (char)(0x80 | (c & 0x3f)); k = 3; }
        else { b[0] = (char)(0xf0 | c >> 18); b[1] = (char)(0x80 | ((c >> 12) & 0x3f)); b[2] = (char)(0x80 | ((c >> 6) & 0x3f)); b[3] = (char)(0x80 | (c & 0x3f)); k = 4; }
        if (o + k + 1 > cap) break;
        memcpy(out + o, b, k);
        o += k;
    }
    out[o] = 0;
    return o;
}

static size_t utf8_to_utf16(const char *s, uint16_t *out, size_t cap) {
    const unsigned char *p = (const unsigned char *)s;
    size_t n = 0;
    while (*p) {
        uint32_t c;
        int k;
        if (*p < 0x80) { c = *p; k = 0; }
        else if ((*p & 0xe0) == 0xc0) { c = *p & 0x1f; k = 1; }
        else if ((*p & 0xf0) == 0xe0) { c = *p & 0x0f; k = 2; }
        else if ((*p & 0xf8) == 0xf0) { c = *p & 0x07; k = 3; }
        else { c = 0xfffd; k = 0; }
        p++;
        for (int i = 0; i < k; i++, p++) {
            if ((*p & 0xc0) != 0x80) { c = 0xfffd; break; }
            c = c << 6 | (*p & 0x3f);
        }
        if (c > 0x10ffff || (c >= 0xd800 && c < 0xe000)) c = 0xfffd;
        if (c >= 0x10000) {
            if (n + 2 > cap) break;
            out[n++] = (uint16_t)(0xd800 + ((c - 0x10000) >> 10));
            out[n++] = (uint16_t)(0xdc00 + ((c - 0x10000) & 0x3ff));
        } else {
            if (n + 1 > cap) break;
            out[n++] = (uint16_t)c;
        }
    }
    return n;
}

/* Disabled-key bits as the applet's: 1 space, 2 '@', 3 '%', 4 '/', 5 '\', 6 digits. */
static int allowed(uint16_t c, uint32_t disabled, int digits_only) {
    int digit = c >= '0' && c <= '9';
    if (digits_only && !digit) return 0;
    if (c < 0x20 || c == 0x7f) return 0;
    if ((disabled & 1u << 1 && c == ' ') || (disabled & 1u << 2 && c == '@') || (disabled & 1u << 3 && c == '%')) return 0;
    if ((disabled & 1u << 4 && c == '/') || (disabled & 1u << 5 && c == '\\') || (disabled & 1u << 6 && digit)) return 0;
    return 1;
}

/* Drop disallowed characters and keep at most max units without splitting a surrogate pair. */
static size_t clean(uint16_t *t, size_t n, size_t max, uint32_t disabled, int digits_only) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        int pair = t[i] >= 0xd800 && t[i] < 0xdc00 && i + 1 < n;
        size_t k = pair ? 2 : 1;
        if (!pair && !allowed(t[i], disabled, digits_only)) continue;
        if (o + k > max) break;
        t[o++] = t[i];
        if (pair) t[o++] = t[++i];
    }
    return o;
}

/* ---- the on-screen dialog: SomnielActivity.requestText (an AlertDialog with an EditText) ----
 * JNI must not run on guest threads (tpidr_el0 holds the GuestCtx there), so a host thread makes the call; the guest
 * thread waits on a condvar until Java answers through nativeTextResult (or the call fails). */
static pthread_mutex_t g_ui_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ui_cv = PTHREAD_COND_INITIALIZER;
static int g_ui_state;                 /* 0 idle, 1 requested, 2 dialog shown, 3 answered */
static char g_ui_initial[KB_LIMIT * 4 + 1];
static int g_ui_min, g_ui_max, g_ui_numeric;
static char g_ui_result[KB_LIMIT * 4 + 1];
static int g_ui_ok;                    /* answered with text (not cancelled / failed) */
volatile int g_text_input_open;        /* main.c holds the game's input at neutral while set */

JNIEXPORT void JNICALL Java_dev_teton_somniel_SomnielActivity_nativeTextResult(JNIEnv *env, jclass cls, jstring text) {
    pthread_mutex_lock(&g_ui_mu);
    g_ui_ok = text != NULL;
    g_ui_result[0] = 0;
    if (text) {
        const char *s = (*env)->GetStringUTFChars(env, text, NULL);
        if (s) { snprintf(g_ui_result, sizeof g_ui_result, "%s", s); (*env)->ReleaseStringUTFChars(env, text, s); }
    }
    g_ui_state = 3;
    pthread_cond_broadcast(&g_ui_cv);
    pthread_mutex_unlock(&g_ui_mu);
}

static void *ui_thread(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "swkbd-ui");
    for (;;) {
        pthread_mutex_lock(&g_ui_mu);
        while (g_ui_state != 1) pthread_cond_wait(&g_ui_cv, &g_ui_mu);
        char initial[sizeof g_ui_initial];
        memcpy(initial, g_ui_initial, sizeof initial);
        int mn = g_ui_min, mx = g_ui_max, num = g_ui_numeric;
        g_ui_state = 2;
        pthread_mutex_unlock(&g_ui_mu);

        int failed = 1;
        JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
        jobject act = env ? (jobject)SDL_GetAndroidActivity() : NULL;
        if (env && act) {
            jclass k = (*env)->GetObjectClass(env, act);
            jmethodID m = k ? (*env)->GetMethodID(env, k, "requestText", "(Ljava/lang/String;IIZ)V") : NULL;
            jstring js = (*env)->NewStringUTF(env, initial);
            if (m && js) {
                g_text_input_open = 1;
                (*env)->CallVoidMethod(env, act, m, js, (jint)mn, (jint)mx, (jboolean)(num ? 1 : 0));
                failed = (*env)->ExceptionCheck(env) != 0;
            }
            if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
            if (js) (*env)->DeleteLocalRef(env, js);
            if (k) (*env)->DeleteLocalRef(env, k);
            (*env)->DeleteLocalRef(env, act);
        }
        if (failed) {
            som_log("swkbd: could not show the text dialog");
            g_text_input_open = 0;
            pthread_mutex_lock(&g_ui_mu);
            g_ui_ok = 0;
            g_ui_state = 4;   /* failed: the caller falls back to the default answer */
            pthread_cond_broadcast(&g_ui_cv);
            pthread_mutex_unlock(&g_ui_mu);
        }
    }
    return NULL;
}

/* Shows the dialog and waits. Returns 1 answered (g_ui_ok tells text or cancel), 0 when the dialog is unavailable. */
static int ui_prompt(const uint16_t *init, size_t init_n, int mn, int mx, int numeric) {
    pthread_mutex_lock(&g_ui_mu);
    if (g_ui_state != 0) { pthread_mutex_unlock(&g_ui_mu); return 0; }  /* one dialog at a time */
    utf16_to_utf8(init, init_n, g_ui_initial, sizeof g_ui_initial);
    g_ui_min = mn; g_ui_max = mx; g_ui_numeric = numeric;
    g_ui_state = 1;
    pthread_cond_broadcast(&g_ui_cv);
    while (g_ui_state != 3 && g_ui_state != 4) pthread_cond_wait(&g_ui_cv, &g_ui_mu);
    int answered = g_ui_state == 3;
    g_ui_state = 0;
    pthread_mutex_unlock(&g_ui_mu);
    g_text_input_open = 0;
    return answered;
}

/* The answer in u (n units); returns 0 when cancelled. */
static int answer(const uint8_t *arg, uint16_t *u, size_t cap, size_t *n, const char **src, int min_len, int max_len, int numeric) {
    const char *ext = SDL_GetAndroidExternalStoragePath();
    char path[600], line[KB_LIMIT * 4 + 2];
    snprintf(path, sizeof path, "%s/swkbd.txt", ext ? ext : ".");
    FILE *f = fopen(path, "r");
    if (f) {
        int got = fgets(line, sizeof line, f) != NULL;
        fclose(f);
        if (got) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strcmp(line, "<cancel>")) { *src = "swkbd.txt"; return 0; }
            if (line[0]) { *n = utf8_to_utf16(line, u, cap); *src = "swkbd.txt"; return 1; }
        }
    }
    uint64_t work = rd64(arg + ARG_WORK), work_size = rd64(arg + ARG_WORK + 8);
    int32_t init_off = (int32_t)rd32(arg + KB_INIT_OFF), init_len = (int32_t)rd32(arg + KB_INIT_LEN);
    *src = "game initial text";
    *n = 0;
    if (init_len > 0 && work && init_off >= 0 && (uint64_t)init_off + (uint64_t)init_len * 2 <= work_size) {
        const uint16_t *t = (const uint16_t *)(uintptr_t)(work + (uint64_t)init_off);
        *n = u16_len(t, (size_t)init_len < cap ? (size_t)init_len : cap);
        memcpy(u, t, *n * 2);
    }
    if (!*n) { *n = utf8_to_utf16("Alear", u, cap); *src = "default"; }
    /* The dialog starts from that text; its answer replaces it. */
    if (ui_prompt(u, *n, min_len, max_len, numeric)) {
        if (!g_ui_ok) { *src = "dialog"; return 0; }
        *n = utf8_to_utf16(g_ui_result, u, cap);
        *src = "dialog";
    }
    return 1;
}

/* Result ShowKeyboard(String *out, const ShowKeyboardArg &arg[, Trigger]); String = {pointer, byte size} */
static void show_keyboard(GuestCtx *c) {
    uint64_t *str = arg_p(c, 0);
    const uint8_t *arg = arg_p(c, 1);
    if (!str || !arg) { som_log("swkbd: ShowKeyboard with a null argument: cancelled"); ret_x(c, SWKBD_CANCELED); return; }
    int32_t max_len = (int32_t)rd32(arg + KB_MAX_LEN), min_len = (int32_t)rd32(arg + KB_MIN_LEN);
    if (max_len <= 0 || max_len > KB_LIMIT) max_len = KB_LIMIT;
    if (min_len < 0) min_len = 0;
    uint32_t disabled = rd32(arg + KB_DISABLED);
    int numeric = rd32(arg + KB_TYPE) == KB_TYPE_NUMPAD, utf8 = arg[KB_UTF8] != 0;
    uint16_t u[KB_LIMIT * 2 + 1];
    size_t n = 0;
    const char *src = "";
    int ok = answer(arg, u, KB_LIMIT * 2, &n, &src, min_len, max_len, numeric);
    n = ok ? clean(u, n, (size_t)max_len, disabled, numeric) : 0;
    if (ok && (int)n < min_len) ok = 0, n = 0;
    uint64_t buf = str[0], size = str[1];
    if (ok && buf && size) {
        if (utf8) utf16_to_utf8(u, n, (char *)(uintptr_t)buf, size);
        else {
            size_t units = size / 2;   /* one kept for the terminator */
            if (units) {
                if (n > units - 1) { n = units - 1; if (n && u[n - 1] >= 0xd800 && u[n - 1] < 0xdc00) n--; }
                memcpy((void *)(uintptr_t)buf, u, n * 2);
                ((uint16_t *)(uintptr_t)buf)[n] = 0;
            }
        }
    }
    char shown[KB_LIMIT * 3 + 1];
    utf16_to_utf8(u, n, shown, sizeof shown);
    som_log("swkbd: ShowKeyboard (%d-%d units, disabled keys %#x, %s out) -> %s%s%s (from %s)", min_len, max_len,
            disabled, utf8 ? "UTF-8" : "UTF-16", ok ? "\"" : "cancelled", ok ? shown : "", ok ? "\"" : "", src);
    ret_x(c, ok ? 0 : SWKBD_CANCELED);
}

void hooks_swkbd_register(void) {
    pthread_t t;
    if (!pthread_create(&t, NULL, ui_thread, NULL)) pthread_detach(t);
    hook_register("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", show_keyboard);
    hook_register("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgENS0_7TriggerE", show_keyboard);
}
