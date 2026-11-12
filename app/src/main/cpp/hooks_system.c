/* System-service replacements: nn::oe, applet, am, prepo, err, pctl, diag,
 * aoc and account. Nothing here talks to a Horizon service; the host answers
 * each call. Queries and initialisation succeed with neutral values.
 * Behaviour follows engage-pc-recomp runtime/ov_system.c (MIT, see hook.h). */

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

#include "debug_log.h"
#include "hook.h"
#include "thread.h"

extern const char *som_data_dir(void);
uint32_t bind_system_event(GuestCtx *c, uint64_t ev, int autoclear);

#define ACCOUNT_R_INVALID 0x7c67u

static uint64_t ticks(void) {
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

/* ---- nn::oe ---- */

static uint64_t g_start_ticks;

static void oe_initialize(GuestCtx *c) {
    (void)c;
    g_start_ticks = ticks();
    som_log("oe: application environment is a local host process");
}

static void oe_desired_language(GuestCtx *c) {
    uint64_t v = 0;
    memcpy(&v, "en-US", 5); /* LanguageCode is char[8] returned in x0 */
    ret_x(c, v);
}

static void oe_performance_config(GuestCtx *c) {
    som_log("oe: performance request mode %lld config 0x%x", (long long)(int32_t)arg_x(c, 0), (uint32_t)arg_x(c, 1));
}
static void oe_noop(GuestCtx *c) { (void)c; }
static void oe_ret_zero(GuestCtx *c) { ret_x(c, 0); }
static void oe_docked(GuestCtx *c) { ret_x(c, 1); } /* OperationMode::Console / PerformanceMode::Boost */
static void oe_in_focus(GuestCtx *c) { ret_x(c, 1); }

/* void GetDisplayVersion(DisplayVersion *out): the 16-byte string of the application's version (Cobalt checks it,
 * and runs before nn::oe is initialised, where the SDK's own call would dereference a null service). */
static void oe_display_version(GuestCtx *c) {
    char *out = arg_p(c, 0);
    if (out) {
        memset(out, 0, 16);
        memcpy(out, "2.0.0", 5);
    }
}

static void oe_focus_mode(GuestCtx *c) {
    som_log("oe: focus handling mode %lld", (long long)(int32_t)arg_x(c, 0));
}

static pthread_mutex_t g_applet_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_message_cv = PTHREAD_COND_INITIALIZER;
static uint32_t g_messages[32];
static unsigned g_head, g_tail;

void oe_push_message(uint32_t m) {
    pthread_mutex_lock(&g_applet_lock);
    if (g_tail - g_head < 32) {
        g_messages[g_tail++ % 32] = m;
        pthread_cond_broadcast(&g_message_cv);
    }
    pthread_mutex_unlock(&g_applet_lock);
}

static uint32_t g_exit_sections;
static void oe_enter_exit_section(GuestCtx *c) {
    (void)c;
    pthread_mutex_lock(&g_applet_lock);
    g_exit_sections++;
    pthread_mutex_unlock(&g_applet_lock);
}
static void oe_leave_exit_section(GuestCtx *c) {
    (void)c;
    pthread_mutex_lock(&g_applet_lock);
    if (g_exit_sections) g_exit_sections--;
    pthread_mutex_unlock(&g_applet_lock);
}

static float g_balance[2] = {1.0f, 1.0f};
static void oe_get_balance(GuestCtx *c) {
    float *l = arg_p(c, 0), *r = arg_p(c, 1);
    if (l) *l = g_balance[0];
    if (r) *r = g_balance[1];
}
static void oe_set_balance(GuestCtx *c) {
    g_balance[0] = arg_s(c, 0);
    g_balance[1] = arg_s(c, 1);
}

/* TimeSpan in nanoseconds: 19.2 MHz ticks * 625 / 12 */
static void oe_active_time(GuestCtx *c) {
    ret_x(c, (uint64_t)((unsigned __int128)(ticks() - g_start_ticks) * 625 / 12));
}

static void oe_finish_logo(GuestCtx *c) {
    (void)c;
    som_log("oe: the game finished its startup logo");
}

/* bool TryPopLaunchParameter(size_t *size, void *buffer, size_t capacity): launched directly, none */
static void oe_launch_parameter(GuestCtx *c) {
    uint64_t *size = arg_p(c, 0);
    if (size) *size = 0;
    ret_x(c, 0);
}

static void oe_try_pop(GuestCtx *c) {
    uint32_t *out = arg_p(c, 0);
    pthread_mutex_lock(&g_applet_lock);
    int ok = g_head != g_tail;
    if (ok) {
        uint32_t m = g_messages[g_head++ % 32];
        if (out) *out = m;
    }
    pthread_mutex_unlock(&g_applet_lock);
    ret_x(c, (uint64_t)ok);
}

static void oe_pop(GuestCtx *c) {
    pthread_mutex_lock(&g_applet_lock);
    while (g_head == g_tail) pthread_cond_wait(&g_message_cv, &g_applet_lock);
    uint32_t m = g_messages[g_head++ % 32];
    pthread_mutex_unlock(&g_applet_lock);
    ret_x(c, m);
}

static void applet_previous_program(GuestCtx *c) { ret_x(c, (uint64_t)(int64_t)-1); }
static void applet_aruid(GuestCtx *c) { ret_x(c, 1); }

/* ---- prepo, err, pctl, diag, aoc ---- */

static void prepo_initialize(GuestCtx *c) { (void)c; }
static void prepo_save(GuestCtx *c) { ret_x(c, 0); }

static void err_show(GuestCtx *c) {
    uint32_t r = (uint32_t)arg_x(c, 0);
    uint64_t off;
    const char *m = som_module_of(c->x[30], &off);
    som_log("err: game error dialog %04u-%04u (result 0x%x) from %s+0x%llx", 2000 + (r & 0x1ff), (r >> 9) & 0x1fff, r,
            m, (unsigned long long)off);
}
static void err_context(GuestCtx *c) {
    (void)c;
    som_log("err: error contexts stay local");
}

static void pctl_initialize(GuestCtx *c) {
    (void)c;
    som_log("pctl: no parental controls");
}

static void diag_observer(GuestCtx *c) {
    (void)c;
    som_log("diag: host diagnostics");
}

/* Add-on content. The two DLC packs of the game (title ids ...F001 = E0, ...F002 = H0; AddOnContentIndex 1 and 2,
 * see DLCManager.GetAddOnContentIndex) hold only empty marker files, and the game decides by the list alone.
 * On by default; <data>/no_dlc turns the packs off. */
#define AOC_BASE_ID 0x0100A6301214F000ull
static const int g_aoc_indices[] = {1, 2};

static int aoc_enabled(void) {
    static int v = -1;
    if (v < 0) {
        char p[600];
        snprintf(p, sizeof p, "%s/no_dlc", som_data_dir());
        v = access(p, F_OK) != 0;
        som_log("aoc: %s add-on content packs (touch <data>/no_dlc to hide them)", v ? "exposing 2" : "hiding");
    }
    return v;
}
static int aoc_count(void) { return aoc_enabled() ? (int)(sizeof g_aoc_indices / sizeof g_aoc_indices[0]) : 0; }

static void aoc_initialize(GuestCtx *c) {
    (void)c;
    som_log("aoc: %d add-on content packs", aoc_count());
}
static void aoc_count_hook(GuestCtx *c) { ret_x(c, aoc_count()); }

/* int ListAddOnContent(AddOnContentIndex *out, int offset, int count) */
static void aoc_list(GuestCtx *c) {
    int *out = arg_p(c, 0);
    int offset = (int)arg_x(c, 1), count = (int)arg_x(c, 2), n = aoc_count(), w = 0;
    for (int i = offset; out && i < n && w < count; i++) out[w++] = g_aoc_indices[i];
    ret_x(c, w);
}
static void aoc_base_id(GuestCtx *c) { ret_x(c, AOC_BASE_ID); }
static void aoc_prepare(GuestCtx *c) { ret_x(c, 0); } /* Result: success, content is local */

/* Result GetAddOnContentListChangedEvent(os::SystemEventType *): the list never changes, the event never fires */
static uint32_t g_aoc_event_wh;
static void aoc_changed_event(GuestCtx *c) {
    uint64_t ev = arg_x(c, 0);
    if (ev && !g_aoc_event_wh) g_aoc_event_wh = bind_system_event(c, ev, 1);
    ret_x(c, 0);
}

/* Result QueryMountAddOnContentCacheSize(size_t *out, int index) */
static void fs_aoc_cache_size(GuestCtx *c) {
    uint64_t *out = arg_p(c, 0);
    if (out) *out = 0x4000;
    ret_x(c, 0);
}
/* Result MountAddOnContent(const char *name, int index, void *cache, size_t size): the packs are empty */
static void fs_aoc_mount(GuestCtx *c) {
    som_log("aoc: mount \"%s\" index %d", (const char *)arg_p(c, 0), (int)arg_x(c, 1));
    ret_x(c, 0);
}

/* ---- nn::account: one offline local user ---- */

typedef struct {
    uint64_t lo, hi;
} Uid;
typedef struct {
    Uid uid;
    uint64_t token;
} UserHandle;

static const Uid g_local = {0x5eed0001a11ce001ull, 0x5eed0002b0b00002ull};
static uint64_t g_tokens[64], g_next_token = 1;
static pthread_mutex_t g_account_lock = PTHREAD_MUTEX_INITIALIZER;

static int same_uid(const Uid *u) { return u && u->lo == g_local.lo && u->hi == g_local.hi; }

static int valid_handle(const UserHandle *h) {
    if (!h || !same_uid(&h->uid)) return 0;
    for (int i = 0; i < 64; i++)
        if (g_tokens[i] && g_tokens[i] == h->token) return 1;
    return 0;
}

static void account_open_user(GuestCtx *c) {
    UserHandle *out = arg_p(c, 0);
    int ok = 0;
    pthread_mutex_lock(&g_account_lock);
    for (int i = 0; i < 64 && !ok && out; i++)
        if (!g_tokens[i]) {
            g_tokens[i] = g_next_token++;
            out->uid = g_local;
            out->token = g_tokens[i];
            ok = 1;
        }
    pthread_mutex_unlock(&g_account_lock);
    ret_x(c, (uint64_t)ok);
}

static void account_close_user(GuestCtx *c) {
    const UserHandle *h = arg_p(c, 0);
    pthread_mutex_lock(&g_account_lock);
    if (valid_handle(h))
        for (int i = 0; i < 64; i++)
            if (g_tokens[i] == h->token) g_tokens[i] = 0;
    pthread_mutex_unlock(&g_account_lock);
}

static void account_user_id(GuestCtx *c) {
    Uid *out = arg_p(c, 0);
    pthread_mutex_lock(&g_account_lock);
    int ok = out && valid_handle(arg_p(c, 1));
    if (ok) *out = g_local;
    pthread_mutex_unlock(&g_account_lock);
    ret_x(c, ok ? 0 : ACCOUNT_R_INVALID);
}

/* the local user has no Nintendo network account */
static void account_network_available(GuestCtx *c) {
    uint8_t *out = arg_p(c, 0);
    pthread_mutex_lock(&g_account_lock);
    int ok = out && valid_handle(arg_p(c, 1));
    if (ok) *out = 0;
    pthread_mutex_unlock(&g_account_lock);
    ret_x(c, ok ? 0 : ACCOUNT_R_INVALID);
}

static void account_user_existence(GuestCtx *c) {
    uint8_t *out = arg_p(c, 0);
    const Uid *u = arg_p(c, 1);
    if (!out || !u) {
        ret_x(c, ACCOUNT_R_INVALID);
        return;
    }
    *out = (uint8_t)same_uid(u);
    ret_x(c, 0);
}

static void account_nickname(GuestCtx *c) {
    char *out = arg_p(c, 0);
    int ok = out && same_uid(arg_p(c, 1));
    if (ok) {
        memset(out, 0, 33);
        memcpy(out, "Somniel", 7);
    }
    ret_x(c, ok ? 0 : ACCOUNT_R_INVALID);
}

/* ---- network (the port runs offline) and nn::ro ---- */

static void ro_lookup(GuestCtx *c) {
    uint64_t *out = arg_p(c, 0);
    const char *name = arg_p(c, 1);
    uint64_t sym = name ? module_symbol(name) : 0;
    if (out && sym) *out = sym;
    ret_x(c, out && sym ? 0 : 0x216); /* nn::ro result: module 22, description 1 */
}

static void ro_register(GuestCtx *c) {
    som_log("ro: RegisterModuleInfo (nrr at 0x%llx)", (unsigned long long)arg_x(c, 1));
    ret_x(c, 0);
}


/* ---- registration ---- */

void hooks_system_register(void) {
    hook_register("_ZN2nn2oe10InitializeEv", oe_initialize);
    hook_register("_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", oe_display_version);
    hook_register("_ZN2nn2oe18GetDesiredLanguageEv", oe_desired_language);
    hook_register("_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", oe_performance_config);
    hook_register("_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE", oe_noop);
    hook_register("_ZN2nn2oe16GetOperationModeEv", oe_docked);
    hook_register("_ZN2nn2oe18GetPerformanceModeEv", oe_docked);
    hook_register("_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", oe_focus_mode);
    hook_register("_ZN2nn2oe20GetCurrentFocusStateEv", oe_in_focus);
    hook_register("_ZN2nn2oe28SetResumeNotificationEnabledEb", oe_noop);
    hook_register("_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", oe_noop);
    hook_register("_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", oe_noop);
    hook_register("_ZN2nn2oe31EnterExitRequestHandlingSectionEv", oe_enter_exit_section);
    hook_register("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", oe_leave_exit_section);
    hook_register("_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_", oe_get_balance);
    hook_register("_ZN2nn2oe24SetExpectedVolumeBalanceEff", oe_set_balance);
    hook_register("_ZN2nn2oe25GetProgramTotalActiveTimeEv", oe_active_time);
    hook_register("_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", oe_noop);
    hook_register("_ZN2nn2oe44SetUserInactivityDetectionTimeExtendedUnsafeEb", oe_noop);
    hook_register("_ZN2nn2oe17FinishStartupLogoEv", oe_finish_logo);
    hook_register("_ZN2nn2oe21TryPopLaunchParameterEPmPvm", oe_launch_parameter);
    hook_register("_ZN2nn2oe25TryPopNotificationMessageEPj", oe_try_pop);
    hook_register("_ZN2nn2oe22PopNotificationMessageEv", oe_pop);

    hook_register("_ZN2nn6applet23GetPreviousProgramIndexEv", applet_previous_program);
    hook_register("_ZN2nn6applet23GetAppletResourceUserIdEv", applet_aruid);
    hook_register("_ZN2nn2am23GetAppletResourceUserIdEv", applet_aruid);

    hook_register("_ZN2nn5prepo10InitializeEv", prepo_initialize);
    hook_register("_ZN2nn5prepo10PlayReport4SaveEv", prepo_save);
    hook_register("_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE", prepo_save);
    hook_register("_ZN2nn5prepo28RequestImmediateTransmissionEv", prepo_save);
    hook_register("_ZN2nn5prepo27AddInternetConnectionStatusEPNS0_10PlayReportEPKcS4_S4_", prepo_save);

    hook_register("_ZN2nn3err9ShowErrorENS_6ResultE", err_show);
    hook_register("_ZN2nn3err26InitializeContextWriterApiEv", err_context);
    hook_register("_ZN2nn4pctl10InitializeEv", pctl_initialize);
    hook_register("_ZN2nn4pctl28IsFreeCommunicationAvailableEv", oe_ret_zero);
    hook_register("_ZN2nn4pctl25TryBeginFreeCommunicationEb", oe_ret_zero);
    hook_register("_ZN2nn4pctl20EndFreeCommunicationEv", oe_noop);
    hook_register("_ZN2nn4diag34InitializeApplicationAbortObserverEv", diag_observer);
    hook_register("_ZN2nn3aoc10InitializeEv", aoc_initialize);
    hook_register("_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE", oe_noop); /* no eShop: returns at once */
    hook_register("_ZN2nn3aoc17CountAddOnContentEv", aoc_count_hook);
    hook_register("_ZN2nn3aoc16ListAddOnContentEPiii", aoc_list);
    hook_register("_ZN2nn3aoc21GetAddOnContentBaseIdEv", aoc_base_id);
    hook_register("_ZN2nn3aoc19PrepareAddOnContentEi", aoc_prepare);
    hook_register("_ZN2nn3aoc31GetAddOnContentListChangedEventEPNS_2os11SystemEventE", aoc_changed_event);
    hook_register("_ZN2nn3aoc37GetAddOnContentListChangedEventLegacyEPNS_2os11SystemEventE", aoc_changed_event);
    hook_register("_ZN2nn2fs31QueryMountAddOnContentCacheSizeEPmi", fs_aoc_cache_size);
    hook_register("_ZN2nn2fs17MountAddOnContentEPKciPvm", fs_aoc_mount);

    hook_register("_ZN2nn4nifm10InitializeEv", oe_ret_zero);
    hook_register("_ZN2nn4nifm18IsNetworkAvailableEv", oe_ret_zero);
    hook_register("_ZN2nn6socket10InitializeEPvmmi", oe_ret_zero);
    hook_register("_ZN2nn6socket10InitializeERKNS0_6ConfigE", oe_ret_zero);
    hook_register("nnsslInitialize", oe_ret_zero);
    hook_register("nnsslFinalize", oe_ret_zero);
    hook_register("_ZN2nn3ssl10InitializeEv", oe_ret_zero);
    hook_register("_ZN2nn3ssl8FinalizeEv", oe_ret_zero);
    hook_register("_ZN2nn2ro10InitializeEv", oe_ret_zero);
    hook_register("_ZN2nn2ro12LookupSymbolEPmPKc", ro_lookup);
    /* plugin NROs (Cobalt's loader): registration needs no service here; loading is in nro_loader.c */
    hook_register("_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", ro_register);
    hook_register("_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKvj", ro_register);
    hook_register("_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE", oe_ret_zero);
    nro_loader_register();

    hook_register("_ZN2nn7account10InitializeEv", oe_noop);
    hook_register("_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE", account_open_user);
    hook_register("_ZN2nn7account9CloseUserERKNS0_10UserHandleE", account_close_user);
    hook_register("_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE", account_user_id);
    hook_register("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", account_network_available);
    hook_register("_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE", account_user_existence);
    hook_register("_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE", account_nickname);
}

