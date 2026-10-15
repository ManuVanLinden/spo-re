/* fixture.h -- test harness for the runtime core: a scratch guest window, a run-time dispatch
 * table for our own fixture functions, a resume-point table, guest-call helpers, an event log.
 * Every "translated function" in these tests is hand-written C in the exact shape the translator emits
 * (SG_FUNC, SG_CALL, SG_LANDING_BEGIN/SG_RESUME_DISPATCH); every EH table is built here. No game
 * code or game bytes are involved. */
#ifndef CORE_FIXTURE_H
#define CORE_FIXTURE_H

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sg_core.h"

extern int t_pass, t_fail;
extern const char *t_name;
#define CHECK(c, ...) do { if (c) t_pass++; else { t_fail++; fprintf(stderr, "FAIL %s:%d [%s] ", __FILE__, __LINE__, t_name); \
        fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define CHECK_EQ(a, b) do { unsigned long long a_ = (unsigned long long)(a), b_ = (unsigned long long)(b); \
        CHECK(a_ == b_, "%s = 0x%llx, expected 0x%llx", #a, a_, b_); } while (0)

extern uint8_t *M;          /* window base */
extern sg_ctx *C;           /* the main test thread's context */

/* Guest data allocator for tables and objects (fixture region 0x01800000 - 0x01c00000). */
#define FX_DATA 0x01800000u
uint32_t fx_alloc(uint32_t n);
uint32_t fx_words(unsigned n, ...);
uint32_t fx_str(const char *s);
void fx_data_reset(void);

/* Fixture functions: 0x00410000 + 0x100 * k. Labels inside a function: entry + offset. */
#define FX_FN(k) (0x00410000u + 0x100u * (uint32_t)(k))
void fx_register(uint32_t guest, sg_func *fn);
void fx_resume_point(uint32_t resume, uint32_t owner);  /* adds to the test resume table */

/* Calls a host function as translated code does (SG_CALL): pushes args right to left and a
 * return address, runs it, checks the return; cdecl: the caller pops. Returns eax. */
uint32_t gcall(sg_ctx *ctx, sg_func *fn, int cdecl_, unsigned n, const uint32_t *args);
#define GSTD(fn, ...) gcall(C, fn, 0, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__})
#define GCDECL(fn, ...) gcall(C, fn, 1, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__})
#define GSTD0(fn) gcall(C, fn, 0, 0, NULL)

/* Event log. */
enum { EV_NONE, EV_THROW, EV_CATCH, EV_DTOR, EV_COPY, EV_UNWIND, EV_AFTER, EV_FILTER, EV_FINALLY, EV_EXCEPT,
       EV_RETHROW, EV_COOKIE, EV_MISC, EV_CLEANUP };
typedef struct { int ev; uint32_t a, b; } fx_event;
extern fx_event fx_log_buf[256];
extern unsigned fx_nlog;
void fx_log(int ev, uint32_t a, uint32_t b);
void fx_log_reset(void);
void fx_log_dump(void);
/* Compares the log's event kinds with `n` expected kinds. */
int fx_log_kinds(const int *kinds, unsigned n);

/* Expecting a fatal error: run `body` and report whether sg_core_fatal fired (message kept). */
extern jmp_buf fx_fatal_jb;
extern int fx_fatal_armed;
extern char fx_fatal_msg[512];
#define EXPECT_FATAL(body) ({ int f_ = 0; sg_ctx save_ = *C; uint32_t fs_ = sg_ld32(M, C->fs_base); fx_fatal_armed = 1; \
        if (!setjmp(fx_fatal_jb)) { body; } else f_ = 1; fx_fatal_armed = 0; *C = save_; sg_st32(M, C->fs_base, fs_); f_; })

/* Test suites */
void test_eh(void);
void test_seh(void);
void test_threads(void);
void test_tls(void);
void test_misc(void);
void test_bench(void);

#endif
