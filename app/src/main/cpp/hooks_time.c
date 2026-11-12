/* nn::time replacements: the host clock and time zone. The user and network
 * clocks follow the device wall clock; the steady clock counts seconds since
 * boot with one random source id per process. ClockSnapshot has the public
 * 0xD0-byte layout because game code reads its fields through inline accessors.
 * Behaviour follows engage-pc-recomp runtime/ov_system.c (MIT, see hook.h);
 * the civil-calendar helpers are H. Hinnant's public-domain algorithms. */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "debug_log.h"
#include "hook.h"

#define TIME_R_INVALID 0x74u
#define TIME_R_SOURCE_MISMATCH 0x18074u

typedef struct {
    int64_t value;
    uint8_t source[16];
} SteadyClockTimePoint;
typedef struct {
    int64_t offset;
    SteadyClockTimePoint point;
} SystemClockContext;
typedef struct {
    int16_t year;
    int8_t month, day, hour, minute, second, pad;
} CalendarTime;
typedef struct {
    int32_t day_of_week, day_of_year;
    char name[8];
    uint8_t dst, pad[3];
    int32_t utc_offset;
} CalendarInfo;
typedef struct {
    SystemClockContext user, network;
    int64_t user_time, network_time;
    CalendarTime user_calendar, network_calendar;
    CalendarInfo user_info, network_info;
    SteadyClockTimePoint steady;
    char location[0x24];
    uint8_t automatic_correction, type, pad[2];
} ClockSnapshot;

_Static_assert(sizeof(SteadyClockTimePoint) == 0x18 && sizeof(CalendarTime) == 8 && sizeof(CalendarInfo) == 0x18,
               "nn::time types");
_Static_assert(sizeof(ClockSnapshot) == 0xd0, "nn::time::ClockSnapshot");

static uint8_t g_source_id[16];
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static volatile int32_t g_ready;

static void make_source(void) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(g_source_id, 1, 16, f) != 16) memset(g_source_id, 0x5a, 16);
        fclose(f);
    }
    tzset();
}

static SteadyClockTimePoint steady_now(void) {
    pthread_once(&g_once, make_source);
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    SteadyClockTimePoint p = {(int64_t)ts.tv_sec, {0}};
    memcpy(p.source, g_source_id, 16);
    return p;
}

static int64_t posix_now(void) { return (int64_t)time(NULL); }

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int64_t)yoe + era * 400 + (*m <= 2);
}

static int64_t floor_div(int64_t a, int64_t b) { return a / b - (a % b != 0 && (a < 0) != (b < 0)); }

static int utc_calendar(int64_t t, CalendarTime *c, CalendarInfo *info) {
    int64_t days = floor_div(t, 86400), sec = t - days * 86400, y;
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    *c = (CalendarTime){(int16_t)y, (int8_t)m, (int8_t)d, (int8_t)(sec / 3600), (int8_t)(sec / 60 % 60), (int8_t)(sec % 60), 0};
    if (info) {
        memset(info, 0, sizeof *info);
        info->day_of_week = (int32_t)(days + 4 - floor_div(days + 4, 7) * 7); /* 1970-01-01 was a Thursday */
        info->day_of_year = (int32_t)(days - days_from_civil(y, 1, 1));
        memcpy(info->name, "UTC", 3);
    }
    return y >= -32768 && y <= 32767;
}

static int local_calendar(int64_t t, CalendarTime *c, CalendarInfo *info) {
    pthread_once(&g_once, make_source);
    struct tm tm;
    time_t tt = (time_t)t;
    if (!localtime_r(&tt, &tm)) return 0;
    int32_t offset = (int32_t)tm.tm_gmtoff;
    *c = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday, (int8_t)tm.tm_hour,
                        (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    if (info) {
        memset(info, 0, sizeof *info);
        info->day_of_week = tm.tm_wday;
        info->day_of_year = tm.tm_yday;
        int a = offset < 0 ? -offset : offset;
        if (!offset) memcpy(info->name, "UTC", 3);
        else if (a % 3600) snprintf(info->name, sizeof info->name, "%c%02d%02d", offset < 0 ? '-' : '+', a / 3600, a / 60 % 60);
        else snprintf(info->name, sizeof info->name, "%c%02d", offset < 0 ? '-' : '+', a / 3600);
        info->dst = tm.tm_isdst > 0;
        info->utc_offset = offset;
    }
    return 1;
}

/* IANA name of the standard offset: Etc/GMT+5 is UTC-5 (POSIX sign); "UTC" off whole hours. */
static void location_name(char out[0x24]) {
    time_t now = time(NULL);
    struct tm tm;
    long west = localtime_r(&now, &tm) ? -tm.tm_gmtoff + (tm.tm_isdst > 0 ? 3600 : 0) : 0;
    memset(out, 0, 0x24);
    if (!west || west % 3600) snprintf(out, 0x24, "UTC");
    else snprintf(out, 0x24, "Etc/GMT%+ld", west / 3600);
}

static void time_initialize(GuestCtx *c) {
    pthread_once(&g_once, make_source);
    g_ready = 1;
    ret_x(c, 0);
}
static void time_finalize(GuestCtx *c) {
    g_ready = 0;
    ret_x(c, 0);
}
static void time_is_ready(GuestCtx *c) { ret_x(c, (uint64_t)g_ready); }

/* Result Standard{User,Network}SystemClock::GetCurrentTime(PosixTime *out) */
static void time_current(GuestCtx *c) {
    int64_t *out = arg_p(c, 0);
    if (!out) {
        ret_x(c, TIME_R_INVALID);
        return;
    }
    *out = posix_now();
    ret_x(c, 0);
}

/* time_point StandardSteadyClock::now(): the int64 seconds in x0 */
static void time_steady_now(GuestCtx *c) { ret_x(c, (uint64_t)steady_now().value); }

static void time_to_calendar(GuestCtx *c) {
    CalendarTime *out = arg_p(c, 0);
    CalendarInfo *info = arg_p(c, 1);
    const int64_t *t = arg_p(c, 2);
    CalendarTime ct;
    CalendarInfo ci;
    if (!out || !t || !local_calendar(*t, &ct, &ci)) {
        ret_x(c, TIME_R_INVALID);
        return;
    }
    *out = ct;
    if (info) *info = ci;
    ret_x(c, 0);
}

static void time_to_calendar_utc(GuestCtx *c) {
    const int64_t *t = arg_p(c, 0);
    CalendarTime ct;
    utc_calendar(t ? *t : 0, &ct, NULL);
    uint64_t v;
    memcpy(&v, &ct, 8);
    ret_x(c, v);
}

static void time_to_posix_utc(GuestCtx *c) {
    const CalendarTime *ct = arg_p(c, 0);
    if (!ct) {
        ret_x(c, 0);
        return;
    }
    int64_t mon = ct->month - 1, y = ct->year + floor_div(mon, 12);
    mon -= floor_div(mon, 12) * 12;
    int64_t t = (days_from_civil(y, (unsigned)mon + 1, 1) + ct->day - 1) * 86400 + ct->hour * 3600 + ct->minute * 60 + ct->second;
    ret_x(c, (uint64_t)t);
}

static void snapshot_init(GuestCtx *c) {
    ClockSnapshot *s = arg_p(c, 0);
    if (s) memset(s, 0, sizeof *s);
}

static void snapshot_create(GuestCtx *c) {
    ClockSnapshot *s = arg_p(c, 0);
    if (!s) {
        ret_x(c, TIME_R_INVALID);
        return;
    }
    SteadyClockTimePoint steady = steady_now();
    int64_t now = posix_now();
    ClockSnapshot n;
    memset(&n, 0, sizeof n);
    n.user = (SystemClockContext){now - steady.value, steady};
    n.network = n.user;
    n.user_time = n.network_time = now;
    local_calendar(now, &n.user_calendar, &n.user_info);
    n.network_calendar = n.user_calendar;
    n.network_info = n.user_info;
    n.steady = steady;
    location_name(n.location);
    n.automatic_correction = 1;
    *s = n;
    ret_x(c, 0);
}

static void time_span_between(GuestCtx *c) {
    int64_t *out = arg_p(c, 0);
    const ClockSnapshot *a = arg_p(c, 1), *b = arg_p(c, 2);
    if (!out || !a || !b) {
        ret_x(c, TIME_R_INVALID);
        return;
    }
    if (memcmp(a->steady.source, b->steady.source, 16)) {
        ret_x(c, TIME_R_SOURCE_MISMATCH);
        return;
    }
    *out = (b->steady.value - a->steady.value) * 1000000000ll;
    ret_x(c, 0);
}

static void time_user_difference(GuestCtx *c) {
    const ClockSnapshot *a = arg_p(c, 0), *b = arg_p(c, 1);
    if (!a || !b || memcmp(a->user.point.source, b->user.point.source, 16)) {
        ret_x(c, 0);
        return;
    }
    ret_x(c, (uint64_t)((b->user.offset - a->user.offset) * 1000000000ll));
}

void hooks_time_register(void) {
    hook_register("_ZN2nn4time10InitializeEv", time_initialize);
    hook_register("_ZN2nn4time8FinalizeEv", time_finalize);
    hook_register("_ZN2nn4time13IsInitializedEv", time_is_ready);
    hook_register("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", time_current);
    hook_register("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", time_current);
    hook_register("_ZN2nn4time19StandardSteadyClock3nowEv", time_steady_now);
    hook_register("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE",
                  time_to_calendar);
    hook_register("_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE", time_to_calendar_utc);
    hook_register("_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE", time_to_posix_utc);
    hook_register("_ZN2nn4time13ClockSnapshotC1Ev", snapshot_init);
    hook_register("_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_", snapshot_create);
    hook_register("_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_", time_span_between);
    hook_register("_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_",
                  time_user_difference);
}
