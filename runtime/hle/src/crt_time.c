/* crt_time.c -- msvcr90 time functions and the kernel32/winmm clocks.
 *
 * - __time64_t / time_t are 64-bit (msvcr90 _time64) or 32-bit (_time32), seconds since 1970.
 * - struct tm is 9 ints (sec, min, hour, mday, mon, year, wday, yday, isdst), as on the host.
 * - _localtime64/_gmtime64 return a per-thread buffer, NULL for negative times or past
 *   _MAX__TIME64_T (3000-12-31 23:59:59 UTC).
 * - strftime runs in the "C" locale with MSVC's definitions: %c = "%m/%d/%y %H:%M:%S",
 *   %x = "%m/%d/%y", %X = "%H:%M:%S", '#' removes leading zeros or selects the long date
 *   (%#c = "Tuesday, March 14, 1995, 12:41:29"; the "C" locale long date is
 *   "dddd, MMMM dd, yyyy", so the day keeps two digits [inf]);
 *   %z and %Z both give the time-zone name. An unknown conversion is an invalid
 *   parameter (returns 0).
 * - Clocks: QueryPerformanceFrequency reports 10 MHz (what current Windows reports);
 *   QueryPerformanceCounter, GetTickCount and timeGetTime count from one monotonic host clock.
 * - SPORE_TIMER_QUANTUM_MS=q (default off; e.g. 15.625) makes the millisecond clocks step like the
 *   Windows XP/7 clock interrupt (DEVIATIONS 2.8): GetTickCount
 *   always returns floor(t / q) * q, as Windows does whatever timeBeginPeriod asks; timeGetTime does
 *   the same while no timeBeginPeriod(p < q) is active and steps by the smallest active period
 *   otherwise (the game asks for 1 ms at 0x75e95b). SPORE_TIMER_QUANTUM_STRICT=1 quantises
 *   timeGetTime to q regardless. QueryPerformanceCounter is never quantised.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hle_imports.h"

#define A(i) SG_ARG(i)
#define MAX_TIME64 32535215999LL /* 3000-12-31 23:59:59 */

static void tm_to_guest(uint8_t *mem, uint32_t a, const struct tm *t) {
    int32_t v[9] = {t->tm_sec, t->tm_min, t->tm_hour, t->tm_mday, t->tm_mon, t->tm_year, t->tm_wday, t->tm_yday, t->tm_isdst};
    int i;
    for (i = 0; i < 9; i++) sg_st32(mem, a + 4u * (uint32_t)i, (uint32_t)v[i]);
}
static void tm_from_guest(const uint8_t *mem, uint32_t a, struct tm *t) {
    memset(t, 0, sizeof *t);
    t->tm_sec = (int32_t)sg_ld32(mem, a);
    t->tm_min = (int32_t)sg_ld32(mem, a + 4);
    t->tm_hour = (int32_t)sg_ld32(mem, a + 8);
    t->tm_mday = (int32_t)sg_ld32(mem, a + 12);
    t->tm_mon = (int32_t)sg_ld32(mem, a + 16);
    t->tm_year = (int32_t)sg_ld32(mem, a + 20);
    t->tm_wday = (int32_t)sg_ld32(mem, a + 24);
    t->tm_yday = (int32_t)sg_ld32(mem, a + 28);
    t->tm_isdst = (int32_t)sg_ld32(mem, a + 32);
}

SG_IMPORT(msvcr90, _time64) {
    int64_t t = (int64_t)time(NULL);
    if (A(0)) sg_st64(mem, A(0), (uint64_t)t);
    SG_RETURN_U64(t, 0);
}
SG_IMPORT(msvcr90, _time32) {
    int32_t t = (int32_t)time(NULL);
    if (A(0)) sg_st32(mem, A(0), (uint32_t)t);
    SG_RETURN_CDECL(t);
}

static uint32_t to_tm(uint8_t *mem, uint32_t tp, int local) {
    int64_t t;
    time_t ht;
    struct tm r;
    if (!tp) { hle_set_errno(mem, MSVC_EINVAL); return 0; }
    t = (int64_t)sg_ld64(mem, tp);
    if (t < 0 || t > MAX_TIME64) { hle_set_errno(mem, MSVC_EINVAL); return 0; }
    ht = (time_t)t;
    if (!(local ? localtime_r(&ht, &r) : gmtime_r(&ht, &r))) return 0;
    tm_to_guest(mem, hle_thread()->tm_va, &r);
    return hle_thread()->tm_va;
}
SG_IMPORT(msvcr90, _localtime64) { SG_RETURN_CDECL(to_tm(mem, A(0), 1)); }
SG_IMPORT(msvcr90, _gmtime64) { SG_RETURN_CDECL(to_tm(mem, A(0), 0)); }

SG_IMPORT(msvcr90, _mktime64) {
    struct tm t;
    time_t r;
    if (!A(0)) { hle_set_errno(mem, MSVC_EINVAL); SG_RETURN_U64(-1, 0); }
    tm_from_guest(mem, A(0), &t);
    r = mktime(&t);
    if (r == (time_t)-1 || r < 0 || (int64_t)r > MAX_TIME64) SG_RETURN_U64(-1, 0);
    tm_to_guest(mem, A(0), &t);
    SG_RETURN_U64((int64_t)r, 0);
}

/* ---- strftime ("C" locale, MSVC definitions) ----------------------------------------- */

static const char *const wday_s[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const wday_l[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const mon_s[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const mon_l[] = {"January", "February", "March", "April", "May", "June", "July",
                                    "August", "September", "October", "November", "December"};

static int num(char *o, int v, int width, int alt) {
    return alt ? snprintf(o, 16, "%d", v) : snprintf(o, 16, "%0*d", width, v);
}

/* Formats into host buffer `out`; returns length, or -1 on an unknown conversion. */
static int msvc_strftime(char *out, size_t cap, const char *f, const struct tm *t) {
    size_t n = 0;
    char tmp[128];
    for (; *f; f++) {
        int k = 0, alt = 0;
        if (*f != '%') { tmp[0] = *f; k = 1; goto add; }
        f++;
        if (*f == '#') { alt = 1; f++; }
        switch (*f) {
        case 'a': k = snprintf(tmp, sizeof tmp, "%s", wday_s[t->tm_wday % 7]); break;
        case 'A': k = snprintf(tmp, sizeof tmp, "%s", wday_l[t->tm_wday % 7]); break;
        case 'b': k = snprintf(tmp, sizeof tmp, "%s", mon_s[t->tm_mon % 12]); break;
        case 'B': k = snprintf(tmp, sizeof tmp, "%s", mon_l[t->tm_mon % 12]); break;
        case 'c':
            if (alt) k = snprintf(tmp, sizeof tmp, "%s, %s %02d, %d, %02d:%02d:%02d", wday_l[t->tm_wday % 7], mon_l[t->tm_mon % 12],
                                 t->tm_mday, t->tm_year + 1900, t->tm_hour, t->tm_min, t->tm_sec);
            else k = snprintf(tmp, sizeof tmp, "%02d/%02d/%02d %02d:%02d:%02d", t->tm_mon + 1, t->tm_mday, t->tm_year % 100,
                             t->tm_hour, t->tm_min, t->tm_sec);
            break;
        case 'd': k = num(tmp, t->tm_mday, 2, alt); break;
        case 'H': k = num(tmp, t->tm_hour, 2, alt); break;
        case 'I': k = num(tmp, t->tm_hour % 12 ? t->tm_hour % 12 : 12, 2, alt); break;
        case 'j': k = num(tmp, t->tm_yday + 1, 3, alt); break;
        case 'm': k = num(tmp, t->tm_mon + 1, 2, alt); break;
        case 'M': k = num(tmp, t->tm_min, 2, alt); break;
        case 'p': k = snprintf(tmp, sizeof tmp, "%s", t->tm_hour < 12 ? "AM" : "PM"); break;
        case 'S': k = num(tmp, t->tm_sec, 2, alt); break;
        case 'U': k = num(tmp, (t->tm_yday + 7 - t->tm_wday) / 7, 2, alt); break;
        case 'w': k = snprintf(tmp, sizeof tmp, "%d", t->tm_wday); break;
        case 'W': k = num(tmp, (t->tm_yday + 7 - (t->tm_wday + 6) % 7) / 7, 2, alt); break;
        case 'x':
            if (alt) k = snprintf(tmp, sizeof tmp, "%s, %s %02d, %d", wday_l[t->tm_wday % 7], mon_l[t->tm_mon % 12], t->tm_mday,
                                 t->tm_year + 1900);
            else k = snprintf(tmp, sizeof tmp, "%02d/%02d/%02d", t->tm_mon + 1, t->tm_mday, t->tm_year % 100);
            break;
        case 'X': k = snprintf(tmp, sizeof tmp, "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec); break;
        case 'y': k = num(tmp, t->tm_year % 100, 2, alt); break;
        case 'Y': k = snprintf(tmp, sizeof tmp, "%d", t->tm_year + 1900); break;
        case 'z': case 'Z': {
            /* MSVC prints the Windows zone name ("Pacific Standard Time"); the host only has the
             * abbreviation. Deviation recorded in hle-crt.md. */
            tzset();
            k = snprintf(tmp, sizeof tmp, "%s", tzname[t->tm_isdst > 0]);
            break;
        }
        case '%': tmp[0] = '%'; k = 1; break;
        default: return -1;
        }
    add:
        if (n + (size_t)k >= cap) return -2;
        memcpy(out + n, tmp, (size_t)k);
        n += (size_t)k;
    }
    out[n] = 0;
    return (int)n;
}

/* size_t strftime(char *buf, size_t max, const char *fmt, const struct tm *t): returns the
 * length without the NUL, or 0 when it does not fit. */
SG_IMPORT(msvcr90, strftime) {
    char fmt[1024], out[4096];
    struct tm t;
    uint32_t buf = A(0), max = A(1);
    int r;
    if (!buf || !max || !A(2) || !A(3)) { hle_invalid_parameter(ctx, "strftime", "NULL argument"); hle_set_errno(mem, MSVC_EINVAL); SG_RETURN_CDECL(0); }
    hle_get_str(mem, A(2), fmt, sizeof fmt);
    tm_from_guest(mem, A(3), &t);
    r = msvc_strftime(out, max < sizeof out ? max : sizeof out, fmt, &t);
    if (r == -1) { hle_invalid_parameter(ctx, "strftime", "unknown conversion"); hle_set_errno(mem, MSVC_EINVAL); mem[buf] = 0; SG_RETURN_CDECL(0); }
    if (r < 0) { mem[buf] = 0; hle_set_errno(mem, MSVC_ERANGE); SG_RETURN_CDECL(0); }
    memcpy(mem + buf, out, (size_t)r + 1);
    SG_RETURN_CDECL(r);
}

/* ---- clocks ------------------------------------------------------------------------- */

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_UPTIME_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
SG_IMPORT(kernel32, QueryPerformanceFrequency) {
    if (A(0)) sg_st64(mem, A(0), 10000000ull);
    SG_RETURN_STDCALL(1, 4);
}
SG_IMPORT(kernel32, QueryPerformanceCounter) {
    if (A(0)) sg_st64(mem, A(0), mono_ns() / 100u);
    SG_RETURN_STDCALL(1, 4);
}

/* SPORE_TIMER_QUANTUM_MS (see the header). Read once; 0 = off (the default: 1 ms steps). */
static int timer_env_state; /* 0 unread, 1 read */
static int timer_strict = -1;
static double timer_quantum_ms(void) {
    static double q;
    int *state = &timer_env_state;
    if (!__atomic_load_n(state, __ATOMIC_ACQUIRE)) {
        const char *e = getenv("SPORE_TIMER_QUANTUM_MS");
        double v = e && *e ? strtod(e, NULL) : 0.0;
        q = (v > 0.0 && v <= 1000.0) ? v : 0.0;
        timer_strict = -1;
        __atomic_store_n(state, 1, __ATOMIC_RELEASE);
    }
    return q;
}
static int timer_quantum_strict(void) {
    if (timer_strict < 0) { const char *e = getenv("SPORE_TIMER_QUANTUM_STRICT"); timer_strict = e && *e == '1'; }
    return timer_strict;
}
/* HLE suite only: read the two variables again on the next clock call. */
void hle_timer_quantum_reload(void) { __atomic_store_n(&timer_env_state, 0, __ATOMIC_RELEASE); }
/* Active timeBeginPeriod requests, counted per period 1..16 ms (Windows accepts 1..1000000; only the
 * smallest matters and anything >= 16 cannot beat the 15.625 ms interrupt). */
static int timer_periods[17];
/* Milliseconds since the clock's origin, stepped by `q` ms when q > 0 (Windows: tick count x
 * increment, truncated to whole milliseconds). Exposed for the HLE suite. */
uint32_t hle_quantised_ms(uint64_t ns, double q) {
    if (q <= 0.0) return (uint32_t)(ns / 1000000u);
    {
        double ticks = floor((double)ns / (q * 1e6));
        return (uint32_t)(uint64_t)(ticks * q);
    }
}
static double timegettime_step(void) {
    double q = timer_quantum_ms();
    if (q <= 0.0 || timer_quantum_strict()) return q;
    for (int p = 1; p <= 16; p++)
        if (p < q && __atomic_load_n(&timer_periods[p], __ATOMIC_RELAXED) > 0) return p == 1 ? 0.0 : (double)p;
    return q;
}
SG_IMPORT(kernel32, GetTickCount) { SG_RETURN_STDCALL(hle_quantised_ms(mono_ns(), timer_quantum_ms()), 0); }
SG_IMPORT(winmm, timeGetTime) { SG_RETURN_STDCALL(hle_quantised_ms(mono_ns(), timegettime_step()), 0); }
SG_IMPORT(winmm, timeBeginPeriod) {
    uint32_t p = A(0);
    if (p >= 1 && p <= 16) __atomic_fetch_add(&timer_periods[p], 1, __ATOMIC_RELAXED);
    SG_RETURN_STDCALL(0 /* TIMERR_NOERROR */, 4);
}
SG_IMPORT(winmm, timeEndPeriod) {
    uint32_t p = A(0);
    if (p >= 1 && p <= 16 && __atomic_load_n(&timer_periods[p], __ATOMIC_RELAXED) > 0)
        __atomic_fetch_sub(&timer_periods[p], 1, __ATOMIC_RELAXED);
    SG_RETURN_STDCALL(0, 4);
}

/* FILETIME: 100 ns units since 1601-01-01 UTC. */
#define EPOCH_DIFF_100NS 116444736000000000ull
static uint64_t now_filetime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return EPOCH_DIFF_100NS + (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100u;
}
SG_IMPORT(kernel32, GetSystemTimeAsFileTime) {
    if (A(0)) sg_st64(mem, A(0), now_filetime());
    SG_RETURN_STDCALL(0, 4);
}

/* SYSTEMTIME: 8 WORDs: year, month (1-12), day of week (0 = Sunday), day, hour, minute,
 * second, milliseconds. */
static void put_systemtime(uint8_t *mem, uint32_t a, uint64_t ft) {
    uint64_t t100 = ft - EPOCH_DIFF_100NS;
    time_t s = (time_t)(t100 / 10000000ull);
    struct tm t;
    gmtime_r(&s, &t);
    uint16_t w[8] = {(uint16_t)(t.tm_year + 1900), (uint16_t)(t.tm_mon + 1), (uint16_t)t.tm_wday, (uint16_t)t.tm_mday,
                     (uint16_t)t.tm_hour, (uint16_t)t.tm_min, (uint16_t)t.tm_sec, (uint16_t)((t100 / 10000u) % 1000u)};
    for (int i = 0; i < 8; i++) sg_st16(mem, a + 2u * (uint32_t)i, w[i]);
}
SG_IMPORT(kernel32, GetSystemTime) {
    if (A(0)) put_systemtime(mem, A(0), now_filetime());
    SG_RETURN_STDCALL(0, 4);
}
/* Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's days_from_civil); the
 * host timegm rejects years before 1900 on macOS. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    int64_t era;
    unsigned yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* BOOL SystemTimeToFileTime(const SYSTEMTIME *, FILETIME *): wDayOfWeek is ignored; out-of-
 * range fields fail with ERROR_INVALID_PARAMETER (87). Years 1601..30827. */
SG_IMPORT(kernel32, SystemTimeToFileTime) {
    uint32_t st = A(0), ft = A(1);
    int y, mo, d, h, mi, s, ms;
    static const int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (!st || !ft) { hle_set_last_error(ctx, mem, 87); SG_RETURN_STDCALL(0, 8); }
    y = sg_ld16(mem, st); mo = sg_ld16(mem, st + 2); d = sg_ld16(mem, st + 6); h = sg_ld16(mem, st + 8);
    mi = sg_ld16(mem, st + 10); s = sg_ld16(mem, st + 12); ms = sg_ld16(mem, st + 14);
    if (y < 1601 || y > 30827 || mo < 1 || mo > 12 || d < 1 || h > 23 || mi > 59 || s > 59 || ms > 999 ||
        d > mdays[mo - 1] + (mo == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0))) {
        hle_set_last_error(ctx, mem, 87);
        SG_RETURN_STDCALL(0, 8);
    }
    {
        int64_t secs = days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + s;
        sg_st64(mem, ft, (uint64_t)((int64_t)EPOCH_DIFF_100NS + secs * 10000000LL + (int64_t)ms * 10000LL));
    }
    SG_RETURN_STDCALL(1, 8);
}
