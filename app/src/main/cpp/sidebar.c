/* JNI bridge for the in-game sidebar (SomnielActivity.java). The panel itself is Android views; this file only carries
 * its state into the native side: whether it is open (the game's input is held at neutral while it is), and which
 * performance monitors the overlay draws. */
#include <jni.h>

#include "debug_log.h"
#include "overlay.h"

volatile int g_sidebar_open;

JNIEXPORT void JNICALL Java_dev_teton_somniel_SomnielActivity_nativeSidebarOpen(JNIEnv *env, jclass cls, jboolean open) {
    g_sidebar_open = open ? 1 : 0;
    som_log("sidebar: %s", open ? "open (game input held)" : "closed");
}

void som_set_frame_rate(int fps);    /* game_patches.c */
void som_set_render_scale(int pct);  /* game_patches.c */

/* The sidebar's frame rate (30, 40 or 60) and render scale (percent, 50 to 125): applied at once. */
JNIEXPORT void JNICALL Java_dev_teton_somniel_SomnielActivity_nativeSetFrameRate(JNIEnv *env, jclass cls, jint fps) {
    som_set_frame_rate(fps);
}

JNIEXPORT void JNICALL Java_dev_teton_somniel_SomnielActivity_nativeSetRenderScale(JNIEnv *env, jclass cls, jint pct) {
    som_set_render_scale(pct);
}

void som_set_graphics(int key, int value); /* game_patches.c: 0 shadows, 1 model detail, 2 texture sharpness, 3 shader precision, 4 console mode, 5 texture size, 6 anti-aliasing, 7 aspect ratio */

JNIEXPORT void JNICALL Java_dev_teton_somniel_SomnielActivity_nativeSetGraphics(JNIEnv *env, jclass cls, jint key, jint value) {
    som_set_graphics(key, value);
}

int som_handheld_profile(void); /* hooks_system.c */

/* The frame rate the sidebar starts with until the user picks one: 30 on the handheld profile (Mali GPUs), else 60. */
JNIEXPORT jint JNICALL Java_dev_teton_somniel_SomnielActivity_nativeDefaultFrameRate(JNIEnv *env, jclass cls) {
    return som_handheld_profile() ? 30 : 60;
}

extern uint64_t som_gfx_presents;

/* True once the game has presented a frame: the Java splash overlay goes away then. */
JNIEXPORT jboolean JNICALL Java_dev_teton_somniel_SomnielActivity_nativeGameHasFrames(JNIEnv *env, jclass cls) {
    return __atomic_load_n(&som_gfx_presents, __ATOMIC_RELAXED) > 0;
}

JNIEXPORT void JNICALL Java_dev_teton_somniel_SomnielActivity_nativeSetMonitorLevel(JNIEnv *env, jclass cls, jint level) {
    overlay_set_level(level);
}

void som_touch_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry); /* main.c */

/* The on-screen controls' state (TouchControls.java): button bits as main.c's NP_* enum, sticks -32767..32767, up positive. */
JNIEXPORT void JNICALL Java_dev_teton_somniel_SomnielActivity_nativeTouchPad(JNIEnv *env, jclass cls, jlong buttons,
                                                                              jint lx, jint ly, jint rx, jint ry) {
    som_touch_set((uint64_t)buttons, lx, ly, rx, ry);
}
