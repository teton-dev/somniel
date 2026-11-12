/* nn::hid replacements: one connected controller, neutral until M4 feeds it
 * from SDL3. Docked it is a full-key controller (id 0, style bit 0). Vibration
 * is accepted and discarded, the six-axis sensor reads at rest, and the mouse
 * and keyboard report no input. State layouts follow libnx HID, and the
 * handle tables and call conventions follow engage-pc-recomp runtime/ov_input.c
 * (MIT, see hook.h). */

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "debug_log.h"
#include "hook.h"

typedef struct {
    int64_t sampling;
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
    uint32_t attributes, pad;
} NpadState;
typedef struct {
    int64_t sampling;
    int32_t x, y, dx, dy, wx, wy;
    uint32_t buttons, attributes;
} MouseState;
typedef struct {
    int64_t sampling;
    uint32_t modifiers, attributes;
    uint64_t keys[4];
} KeyboardState;
typedef struct {
    int8_t players;
    uint8_t pad[3];
    uint32_t selected;
} ControllerSupportResult;
typedef struct {
    int64_t delta_ns, sampling;
    float acc[3], gyro[3], angle[3], dir[9];
    uint32_t attributes, reserved;
} SixAxisState;
typedef struct {
    float amp_low, freq_low, amp_high, freq_high;
} VibrationValue;
typedef struct {
    uint32_t type, position;
} VibrationDeviceInfo;

_Static_assert(sizeof(NpadState) == 40 && sizeof(MouseState) == 40 && sizeof(KeyboardState) == 48, "hid state ABI");
_Static_assert(sizeof(SixAxisState) == 0x60 && sizeof(VibrationValue) == 16, "hid ABI");

enum { STYLE_FULL_KEY = 1, STYLE_HANDHELD = 2, ATTR_CONNECTED = 1 };
enum { PAD_ID = 0, PAD_STYLE_INDEX = 3, SIX_AXIS_DEVICE = 2 };

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_styles = STYLE_FULL_KEY;
static int g_enabled, g_allowed = 1;
static int64_t g_sample;
static NpadState g_pad; /* written by the input code, read by the game */

/* called by the SDL input thread (M4) */
void som_hid_set_pad(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    pthread_mutex_lock(&g_lock);
    g_pad.buttons = buttons;
    g_pad.lx = lx;
    g_pad.ly = ly;
    g_pad.rx = rx;
    g_pad.ry = ry;
    pthread_mutex_unlock(&g_lock);
}

static uint32_t need_id(GuestCtx *c, int i) {
    const uint32_t *id = arg_p(c, i);
    return id ? *id : 0xFFFFFFFFu;
}

static void hid_initialize_npad(GuestCtx *c) {
    (void)c;
    pthread_mutex_lock(&g_lock);
    g_enabled = 1;
    pthread_mutex_unlock(&g_lock);
    som_log("hid: controllers initialized (one connected full-key controller)");
}

static void hid_supported_ids(GuestCtx *c) {
    const uint32_t *ids = arg_p(c, 0);
    uint64_t n = arg_x(c, 1);
    int ok = 0;
    for (uint64_t i = 0; ids && i < n && i < 10; i++) ok |= ids[i] == PAD_ID;
    pthread_mutex_lock(&g_lock);
    g_allowed = ok;
    pthread_mutex_unlock(&g_lock);
}

static void hid_supported_styles(GuestCtx *c) {
    pthread_mutex_lock(&g_lock);
    g_styles = (uint32_t)arg_x(c, 0);
    pthread_mutex_unlock(&g_lock);
}

static void hid_style_set(GuestCtx *c) {
    uint32_t id = need_id(c, 0);
    pthread_mutex_lock(&g_lock);
    uint32_t v = g_enabled && g_allowed && id == PAD_ID ? g_styles & STYLE_FULL_KEY : 0;
    pthread_mutex_unlock(&g_lock);
    ret_x(c, v);
}

/* int GetNpadStates(<State> *out, int count, const NpadIdType &id): the newest state only */
static void npad_states(GuestCtx *c, uint32_t want) {
    NpadState *out = arg_p(c, 0);
    int max = (int)arg_x(c, 1);
    uint32_t id = need_id(c, 2);
    if (max <= 0 || !out) {
        ret_x(c, 0);
        return;
    }
    pthread_mutex_lock(&g_lock);
    NpadState s = {.sampling = ++g_sample};
    int live = want && g_enabled && g_allowed && id == PAD_ID && want == STYLE_FULL_KEY && (g_styles & want);
    if (live) {
        s.attributes = ATTR_CONNECTED;
        s.buttons = g_pad.buttons;
        s.lx = g_pad.lx;
        s.ly = g_pad.ly;
        s.rx = g_pad.rx;
        s.ry = g_pad.ry;
    }
    pthread_mutex_unlock(&g_lock);
    *out = s;
    ret_x(c, 1);
}

/* the single-state SDK 12 ABI takes the id as its second argument */
static void npad_state(GuestCtx *c, uint32_t want) {
    c->x[2] = c->x[1];
    c->x[1] = 1;
    npad_states(c, want);
}
static void hid_state_full_key(GuestCtx *c) { npad_state(c, STYLE_FULL_KEY); }
static void hid_state_handheld(GuestCtx *c) { npad_state(c, STYLE_HANDHELD); }
static void hid_state_joycon(GuestCtx *c) { npad_state(c, 0); }
static void hid_states_full_key(GuestCtx *c) { npad_states(c, STYLE_FULL_KEY); }
static void hid_states_handheld(GuestCtx *c) { npad_states(c, STYLE_HANDHELD); }
static void hid_states_joycon(GuestCtx *c) { npad_states(c, 0); }

/* ---- touch screen (probe) ----
 * nn::hid::TouchScreenState<N>: { i64 sampling; i32 count; i32 reserved; TouchState touches[N]; } and
 * TouchState: { i64 delta_ns; u32 attributes (bit0 start, bit1 end); i32 finger, x, y, diam_x, diam_y, angle, reserved }.
 * Coordinates are the 1280x720 handheld panel. The game polls slower than a tap lasts, so every change is kept as one
 * sample in a ring and GetTouchScreenStates returns the newest ones first (the SDK's own contract). */
#define TOUCH_MAX 16
#define TOUCH_RING 32
typedef struct {
    int64_t delta_ns;
    uint32_t attributes;
    int32_t finger, x, y, dx, dy, angle, reserved;
} TouchPoint;
typedef struct {
    int64_t sampling;
    int32_t count, reserved;
    TouchPoint t[TOUCH_MAX];
} TouchSample;
_Static_assert(sizeof(TouchPoint) == 40 && sizeof(TouchSample) == 16 + 40 * TOUCH_MAX, "touch ABI");

static TouchSample g_tring[TOUCH_RING];
static int64_t g_tseq;               /* samples ever pushed */
static TouchPoint g_tlive[TOUCH_MAX]; /* fingers currently down (finger == -1: free slot) */
static int g_touch_inited, g_touch_polls, g_touch_polls_log;
static int64_t g_touch_last_ns;

void som_hid_touch(int64_t finger, float nx, float ny, int phase /* 0 down, 1 move, 2 up */, int64_t now_ns) {
    pthread_mutex_lock(&g_lock);
    int slot = -1, free_slot = -1;
    for (int i = 0; i < TOUCH_MAX; i++) {
        if (g_tlive[i].finger == (int32_t)finger && g_tlive[i].reserved == 1) slot = i;
        else if (g_tlive[i].reserved == 0 && free_slot < 0) free_slot = i;
    }
    if (phase == 0 && slot < 0) slot = free_slot;
    if (slot < 0) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    TouchPoint *p = &g_tlive[slot];
    int x = (int)(nx * 1280.f), y = (int)(ny * 720.f);
    p->x = x < 0 ? 0 : x > 1279 ? 1279 : x;
    p->y = y < 0 ? 0 : y > 719 ? 719 : y;
    p->finger = (int32_t)finger;
    p->dx = p->dy = 15;
    p->reserved = 1;
    TouchSample *s = &g_tring[g_tseq % TOUCH_RING];
    memset(s, 0, sizeof *s);
    s->sampling = ++g_tseq;
    int n = 0;
    for (int i = 0; i < TOUCH_MAX; i++) {
        if (g_tlive[i].reserved != 1) continue;
        TouchPoint q = g_tlive[i];
        q.delta_ns = g_touch_last_ns ? now_ns - g_touch_last_ns : 0;
        q.attributes = i == slot ? (phase == 0 ? 1u : phase == 2 ? 2u : 0u) : 0u;
        q.reserved = 0;
        s->t[n++] = q;
    }
    s->count = n;
    g_touch_last_ns = now_ns;
    if (phase == 2) memset(p, 0, sizeof *p); /* the End sample above still carries the finger */
    pthread_mutex_unlock(&g_lock);
    som_log("touch: finger %lld phase %d -> (%d,%d) fingers=%d polled=%d", (long long)finger, phase, p->x, p->y, n,
            g_touch_polls);
}

static void hid_touch_init(GuestCtx *c) {
    (void)c;
    g_touch_inited = 1;
    som_log("hid: InitializeTouchScreen called by the game");
}

static void hid_touch_states(GuestCtx *c) {
    TouchSample *out = arg_p(c, 0);
    int max = (int)arg_x(c, 1);
    if (++g_touch_polls == 1 || g_touch_polls - g_touch_polls_log >= 600) {
        g_touch_polls_log = g_touch_polls;
        som_log("hid: GetTouchScreenStates polls=%d (init=%d)", g_touch_polls, g_touch_inited);
    }
    if (!out || max <= 0) {
        ret_x(c, 0);
        return;
    }
    pthread_mutex_lock(&g_lock);
    int n = 0;
    int64_t have = g_tseq < TOUCH_RING ? g_tseq : TOUCH_RING;
    for (int64_t k = 0; k < have && n < max; k++) out[n++] = g_tring[(g_tseq - 1 - k) % TOUCH_RING];
    if (!n) { /* nothing ever touched: one empty sample so the caller sees a live device */
        memset(out, 0, sizeof *out);
        out[0].sampling = 0;
        n = 1;
    }
    pthread_mutex_unlock(&g_lock);
    ret_x(c, (uint64_t)n);
}

static void hid_gesture_init(GuestCtx *c) {
    (void)c;
    som_log("hid: InitializeGesture called by the game");
}

static void hid_noop(GuestCtx *c) { (void)c; }
static void hid_ret_zero(GuestCtx *c) { ret_x(c, 0); }

static void hid_disconnect(GuestCtx *c) {
    if (need_id(c, 0) != PAD_ID) return;
    pthread_mutex_lock(&g_lock);
    g_allowed = 0;
    pthread_mutex_unlock(&g_lock);
}

static void hid_controller_support(GuestCtx *c) {
    ControllerSupportResult *out = arg_p(c, 0);
    if (out) *out = (ControllerSupportResult){.players = 1, .selected = PAD_ID};
    ret_x(c, 0);
}

static void hid_mouse_states(GuestCtx *c) {
    MouseState *out = arg_p(c, 0);
    if ((int)arg_x(c, 1) <= 0 || !out) {
        ret_x(c, 0);
        return;
    }
    pthread_mutex_lock(&g_lock);
    *out = (MouseState){.sampling = ++g_sample};
    pthread_mutex_unlock(&g_lock);
    ret_x(c, 1);
}

static void hid_keyboard_state(GuestCtx *c) {
    KeyboardState *out = arg_p(c, 0);
    if (!out) return;
    pthread_mutex_lock(&g_lock);
    *out = (KeyboardState){.sampling = ++g_sample};
    pthread_mutex_unlock(&g_lock);
}

/* handle: byte 0 style index (3 full-key), byte 1 controller id, byte 2 device (vibration 0 left / 1 right, six-axis 2) */
static void hid_vibration_handles(GuestCtx *c) {
    uint32_t *out = arg_p(c, 0), style = (uint32_t)arg_x(c, 3);
    int max = (int)arg_x(c, 1);
    uint32_t id = need_id(c, 2);
    if (!out || max <= 0 || __builtin_popcount(style) != 1) {
        ret_x(c, 0);
        return;
    }
    static const struct {
        uint8_t bit, n;
        uint32_t base;
    } t[] = {{0, 2, 0x3}, {1, 2, 0x4}, {2, 2, 0x5}, {3, 1, 0x6}, {4, 1, 0x10007}, {5, 1, 0x8}, {10, 1, 0xd}, {8, 2, 0x4}, {30, 2, 0x21}, {29, 2, 0x20}};
    int n = 0;
    for (size_t i = 0; i < sizeof t / sizeof *t && !n; i++) {
        if (!(style >> t[i].bit & 1)) continue;
        n = t[i].n < max ? t[i].n : max;
        for (int k = 0; k < n; k++) out[k] = (t[i].base | id << 8) + ((uint32_t)k << 16);
    }
    ret_x(c, (uint64_t)n);
}

static void hid_six_axis_handles(GuestCtx *c) {
    uint32_t *out = arg_p(c, 0), style = (uint32_t)arg_x(c, 3);
    int max = (int)arg_x(c, 1);
    uint32_t id = need_id(c, 2);
    if (!out || max <= 0) {
        ret_x(c, 0);
        return;
    }
    uint32_t base = 0, n = 1;
    if (style & 1) base = 0x20003;
    else if (style & 2) base = 0x20004;
    else if (style & 4) base = 0x5, n = 2;
    else if (style & 8) base = 0x6;
    else if (style & 0x10) base = 0x10007;
    else if (style & 0x20) base = 0x20003;
    else if (style & 0xe80) n = 0;
    else if (style & 0x100) base = 0x20004;
    else if ((style & 0x60000040) == 0x40) base = 0x20003;
    else n = 0;
    if (n > (uint32_t)max) n = (uint32_t)max;
    for (uint32_t k = 0; k < n; k++) out[k] = (base | id << 8) + (k << 16);
    ret_x(c, n);
}

static void hid_vib_actual(GuestCtx *c) {
    VibrationValue *out = arg_p(c, 0);
    if (out) *out = (VibrationValue){0, 160, 0, 320};
    ret_x(c, 0);
}

static void hid_vib_info(GuestCtx *c) {
    VibrationDeviceInfo *out = arg_p(c, 0);
    const uint32_t *h = arg_p(c, 1);
    if (out) {
        uint32_t dev = h ? (*h >> 16 & 0xff) : 0;
        *out = dev < 2 ? (VibrationDeviceInfo){1, dev + 1} : (VibrationDeviceInfo){0, 0};
    }
    ret_x(c, 0);
}

static void hid_vib_mounted(GuestCtx *c) {
    uint8_t *out = arg_p(c, 0);
    const uint32_t *h = arg_p(c, 1);
    if (out) *out = h && (*h & 0xff) == PAD_STYLE_INDEX && (*h >> 8 & 0xff) == PAD_ID;
    ret_x(c, 0);
}

static void hid_vib_permitted(GuestCtx *c) {
    uint8_t *out = arg_p(c, 0);
    if (out) *out = 1;
    ret_x(c, 0);
}

static void hid_six_states(GuestCtx *c) {
    int32_t *count = arg_p(c, 0);
    SixAxisState *out = arg_p(c, 1);
    int max = (int)arg_x(c, 2);
    static const SixAxisState rest = {.acc = {0, 0, -1}, .dir = {1, 0, 0, 0, 1, 0, 0, 0, 1}};
    if (max < 0) max = 0;
    for (int i = 0; out && i < max; i++) out[i] = rest; /* the game reads every entry it asks for */
    if (count) *count = max;
    ret_x(c, 0);
}

static void hid_six_state(GuestCtx *c) {
    SixAxisState *out = arg_p(c, 0);
    static const SixAxisState rest = {.acc = {0, 0, -1}, .dir = {1, 0, 0, 0, 1, 0, 0, 0, 1}};
    if (out) *out = rest;
    ret_x(c, 0);
}

void hooks_hid_register(void) {
    hook_register("_ZN2nn3hid14InitializeNpadEv", hid_initialize_npad);
    hook_register("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", hid_supported_ids);
    hook_register("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_supported_styles);
    hook_register("_ZN2nn3hid15GetNpadStyleSetERKj", hid_style_set);
    hook_register("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_state_full_key);
    hook_register("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_state_handheld);
    hook_register("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", hid_state_joycon);
    hook_register("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", hid_state_joycon);
    hook_register("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", hid_state_joycon);
    hook_register("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", hid_states_full_key);
    hook_register("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", hid_states_joycon);
    hook_register("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", hid_states_joycon);
    hook_register("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", hid_states_joycon);
    hook_register("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", hid_states_handheld);
    hook_register("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", hid_noop);
    hook_register("_ZN2nn3hid14DisconnectNpadERKj", hid_disconnect);
    hook_register("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", hid_controller_support);
    hook_register("_ZN2nn3hid21InitializeTouchScreenEv", hid_touch_init);
    hook_register("_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi", hid_touch_states);
    hook_register("_ZN2nn3hid17InitializeGestureEv", hid_gesture_init);
    hook_register("_ZN2nn3hid16GetGestureStatesEPNS0_12GestureStateEi", hid_ret_zero);
    hook_register("_ZN2nn3hid15InitializeMouseEv", hid_noop);
    hook_register("_ZN2nn3hid18InitializeKeyboardEv", hid_noop);
    hook_register("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", hid_mouse_states);
    hook_register("_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", hid_keyboard_state);
    hook_register("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_vibration_handles);
    hook_register("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_six_axis_handles);
    hook_register("_ZN2nn3hid6detail25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", hid_ret_zero);
    hook_register("_ZN2nn3hid6detail18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", hid_ret_zero);
    hook_register("_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi", hid_ret_zero);
    hook_register("_ZN2nn3hid6detail23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE", hid_vib_actual);
    hook_register("_ZN2nn3hid6detail22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", hid_vib_info);
    hook_register("_ZN2nn3hid6detail24IsVibrationDeviceMountedEPbRKNS0_21VibrationDeviceHandleE", hid_vib_mounted);
    hook_register("_ZN2nn3hid6detail20IsVibrationPermittedEPb", hid_vib_permitted);
    hook_register("_ZN2nn3hid6detail15PermitVibrationEb", hid_ret_zero);
    hook_register("_ZN2nn3hid6detail18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_ret_zero);
    hook_register("_ZN2nn3hid6detail17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_ret_zero);
    hook_register("_ZN2nn3hid6detail22GetSixAxisSensorStatesEPiPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", hid_six_states);
    hook_register("_ZN2nn3hid6detail21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", hid_six_state);
}
