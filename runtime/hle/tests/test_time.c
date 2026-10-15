/* CRT time functions, strftime, and the kernel32/winmm clocks. */
#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "harness.h"

uint32_t hle_quantised_ms(uint64_t ns, double q);
void hle_timer_quantum_reload(void);

/* SPORE_TIMER_QUANTUM_MS (DEVIATIONS 2.8): Windows-style 15.625 ms clock steps. */
static int on_grid(uint32_t v, double q) { return v == (uint32_t)(uint64_t)(floor((v + 0.999) / q) * q); }
static void test_timer_quantum(void) {
    uint32_t tp = gbuf(8), a, b, seen = 0, steps_ok = 1, prev;
    struct timespec ts = {0, 2 * 1000000};
    /* the pure function: 15/16 ms steps, exact multiples of 15.625 truncated */
    CHECK_EQ(hle_quantised_ms(0, 15.625), 0);
    CHECK_EQ(hle_quantised_ms(15624999, 15.625), 0);
    CHECK_EQ(hle_quantised_ms(15625000, 15.625), 15);
    CHECK_EQ(hle_quantised_ms(31250000, 15.625), 31);
    CHECK_EQ(hle_quantised_ms(46875000, 15.625), 46);
    CHECK_EQ(hle_quantised_ms(62500000, 15.625), 62);
    CHECK_EQ(hle_quantised_ms(1000000000000ull, 15.625), 1000000);
    CHECK_EQ(hle_quantised_ms(16999999, 0.0), 16);
    CHECK_EQ(hle_quantised_ms(16999999, 1.0), 16);
    /* default (unset): 1 ms steps, unchanged */
    unsetenv("SPORE_TIMER_QUANTUM_MS"); unsetenv("SPORE_TIMER_QUANTUM_STRICT");
    hle_timer_quantum_reload();
    a = CALL0(sg_imp_kernel32_GetTickCount, 0);
    nanosleep(&ts, NULL);
    b = CALL0(sg_imp_kernel32_GetTickCount, 0);
    CHECK(b - a >= 1 && b - a < 50);
    /* quantised: GetTickCount only ever shows 15.625 ms grid values and moves in 15/16 ms steps */
    setenv("SPORE_TIMER_QUANTUM_MS", "15.625", 1);
    hle_timer_quantum_reload();
    prev = CALL0(sg_imp_kernel32_GetTickCount, 0);
    CHECK(on_grid(prev, 15.625));
    for (int i = 0; i < 25; i++) {
        uint32_t v;
        nanosleep(&ts, NULL);
        v = CALL0(sg_imp_kernel32_GetTickCount, 0);
        if (!on_grid(v, 15.625)) steps_ok = 0;
        if (v != prev) { if (v - prev != 15 && v - prev != 16 && v - prev != 31 && v - prev != 32) steps_ok = 0; seen++; }
        prev = v;
    }
    CHECK(steps_ok);
    CHECK(seen >= 1); /* ~50 ms of sleeps cross at least one tick */
    /* timeGetTime: quantised until timeBeginPeriod(1), as on Windows; 1 ms again after it */
    CHECK(on_grid(CALL0(sg_imp_winmm_timeGetTime, 0), 15.625));
    CHECK_EQ(CALL(sg_imp_winmm_timeBeginPeriod, 4, 1), 0);
    {
        int off_grid = 0;
        for (int i = 0; i < 40 && !off_grid; i++) {
            if (!on_grid(CALL0(sg_imp_winmm_timeGetTime, 0), 15.625)) off_grid = 1;
            nanosleep(&ts, NULL);
        }
        CHECK(off_grid); /* 1 ms resolution while the period is active */
        CHECK(on_grid(CALL0(sg_imp_kernel32_GetTickCount, 0), 15.625)); /* GetTickCount ignores it */
        setenv("SPORE_TIMER_QUANTUM_STRICT", "1", 1);
        hle_timer_quantum_reload();
        CHECK(on_grid(CALL0(sg_imp_winmm_timeGetTime, 0), 15.625)); /* strict: always quantised */
    }
    CHECK_EQ(CALL(sg_imp_winmm_timeEndPeriod, 4, 1), 0);
    unsetenv("SPORE_TIMER_QUANTUM_STRICT");
    hle_timer_quantum_reload();
    CHECK(on_grid(CALL0(sg_imp_winmm_timeGetTime, 0), 15.625));
    /* QueryPerformanceCounter is never quantised: two reads 2 ms apart differ by ~20000 */
    CALL(sg_imp_kernel32_QueryPerformanceCounter, 4, tp);
    a = (uint32_t)sg_ld64(M, tp);
    nanosleep(&ts, NULL);
    CALL(sg_imp_kernel32_QueryPerformanceCounter, 4, tp);
    b = (uint32_t)sg_ld64(M, tp);
    CHECK(b - a >= 19000u && b - a < 500000u);
    unsetenv("SPORE_TIMER_QUANTUM_MS");
    hle_timer_quantum_reload();
}

static uint32_t make_tm(int y, int mo, int d, int h, int mi, int s, int wd, int yd) {
    uint32_t t = gbuf(36);
    int32_t v[9] = {s, mi, h, d, mo - 1, y - 1900, wd, yd, 0};
    for (int i = 0; i < 9; i++) sg_st32(M, t + 4u * (uint32_t)i, (uint32_t)v[i]);
    return t;
}

void test_time(void) {
    uint32_t tp = gbuf(8), r, buf = gbuf(256), tm, ft = gbuf(8), st = gbuf(16);
    int64_t now = (int64_t)time(NULL), got;

    CALL(sg_imp_msvcr90__time64, 0, tp);
    got = (int64_t)(((uint64_t)T.edx << 32) | T.eax);
    CHECK(got >= now && got <= now + 5);
    CHECK_EQ((int64_t)sg_ld64(M, tp), got);
    CHECK((int32_t)CALL(sg_imp_msvcr90__time32, 0, 0) >= (int32_t)now);

    /* gmtime: 1970-01-01 was a Thursday */
    sg_st64(M, tp, 0);
    r = CALL(sg_imp_msvcr90__gmtime64, 0, tp);
    CHECK(r != 0);
    CHECK_EQ(sg_ld32(M, r + 20), 70);
    CHECK_EQ(sg_ld32(M, r + 24), 4);
    CHECK_EQ(sg_ld32(M, r + 12), 1);
    sg_st64(M, tp, (uint64_t)-1);
    CHECK_EQ(CALL(sg_imp_msvcr90__gmtime64, 0, tp), 0); /* negative: NULL */
    sg_st64(M, tp, 32535216000ull);
    CHECK_EQ(CALL(sg_imp_msvcr90__gmtime64, 0, tp), 0); /* past year 3000 */

    /* localtime -> mktime round trip */
    sg_st64(M, tp, (uint64_t)now);
    r = CALL(sg_imp_msvcr90__localtime64, 0, tp);
    CHECK(r != 0);
    tm = gbuf(36);
    memcpy(M + tm, M + r, 36);
    CALL(sg_imp_msvcr90__mktime64, 0, tm);
    got = (int64_t)(((uint64_t)T.edx << 32) | T.eax);
    CHECK_EQ(got, now);

    /* strftime, "C" locale */
    tm = make_tm(1995, 3, 14, 12, 41, 29, 2, 72);
    r = CALL(sg_imp_msvcr90_strftime, 0, buf, 256, gstr("%Y-%m-%d %H:%M:%S %a %A %b %B %j %p %I %y %w %U %W %%"), tm);
    CHECK_STR(hstr(buf), "1995-03-14 12:41:29 Tue Tuesday Mar March 073 PM 12 95 2 11 11 %");
    CHECK_EQ(r, strlen(hstr(buf)));
    CALL(sg_imp_msvcr90_strftime, 0, buf, 256, gstr("%c|%x|%X|%#d|%#j"), tm);
    CHECK_STR(hstr(buf), "03/14/95 12:41:29|03/14/95|12:41:29|14|73");
    CALL(sg_imp_msvcr90_strftime, 0, buf, 256, gstr("%#c|%#x"), tm);
    CHECK_STR(hstr(buf), "Tuesday, March 14, 1995, 12:41:29|Tuesday, March 14, 1995");
    CHECK_EQ(CALL(sg_imp_msvcr90_strftime, 0, buf, 5, gstr("%Y-%m"), tm), 0); /* does not fit */
    CHECK_EQ(CALL(sg_imp_msvcr90_strftime, 0, buf, 256, gstr("%Q"), tm), 0); /* invalid parameter */

    /* clocks */
    CHECK_EQ(CALL(sg_imp_kernel32_QueryPerformanceFrequency, 4, tp), 1);
    CHECK(sg_ld64(M, tp) == 10000000ull);
    CALL(sg_imp_kernel32_QueryPerformanceCounter, 4, tp);
    {
        uint64_t q1 = sg_ld64(M, tp);
        uint32_t t1 = CALL0(sg_imp_kernel32_GetTickCount, 0), t2;
        struct timespec ts = {0, 20 * 1000000};
        nanosleep(&ts, NULL);
        CALL(sg_imp_kernel32_QueryPerformanceCounter, 4, tp);
        t2 = CALL0(sg_imp_winmm_timeGetTime, 0);
        CHECK(sg_ld64(M, tp) - q1 >= 190000u && sg_ld64(M, tp) - q1 < 2000000u); /* ~20 ms in 100 ns units */
        CHECK(t2 - t1 >= 19 && t2 - t1 < 200);
    }
    CHECK_EQ(CALL(sg_imp_winmm_timeBeginPeriod, 4, 1), 0);
    CHECK_EQ(CALL(sg_imp_winmm_timeEndPeriod, 4, 1), 0);
    test_timer_quantum();

    /* FILETIME / SYSTEMTIME */
    {
        uint16_t w[8] = {1970, 1, 0, 1, 0, 0, 0, 0};
        for (int i = 0; i < 8; i++) sg_st16(M, st + 2u * (uint32_t)i, w[i]);
        CHECK_EQ(CALL(sg_imp_kernel32_SystemTimeToFileTime, 8, st, ft), 1);
        CHECK(sg_ld64(M, ft) == 116444736000000000ull);
        sg_st16(M, st, 1601);
        sg_st16(M, st + 14, 7);
        CALL(sg_imp_kernel32_SystemTimeToFileTime, 8, st, ft);
        CHECK(sg_ld64(M, ft) == 70000ull);
        sg_st16(M, st, 2023); sg_st16(M, st + 2, 2); sg_st16(M, st + 6, 29); /* not a leap year */
        CHECK_EQ(CALL(sg_imp_kernel32_SystemTimeToFileTime, 8, st, ft), 0);
        CHECK_EQ(hle_get_last_error(&T, M), 87);
        sg_st16(M, st, 2024);
        CHECK_EQ(CALL(sg_imp_kernel32_SystemTimeToFileTime, 8, st, ft), 1);
        CALL(sg_imp_kernel32_GetSystemTime, 4, st);
        CHECK(sg_ld16(M, st) >= 2026 && sg_ld16(M, st + 2) >= 1 && sg_ld16(M, st + 2) <= 12);
        CALL(sg_imp_kernel32_GetSystemTimeAsFileTime, 4, ft);
        CHECK(sg_ld64(M, ft) / 10000000ull - 11644473600ull >= (uint64_t)now);
    }
}
