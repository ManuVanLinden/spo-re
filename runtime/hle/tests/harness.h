/* harness.h -- host-only test harness for the HLE shims. No game code or bytes.
 *
 * It reserves a 4 GiB guest window like the runtime will (spore_guest.h section 1), maps a few
 * regions read/write, and calls shims exactly as translated code does: arguments pushed on
 * the guest stack, a return address, then the sg_func. After the call it checks that the
 * shim popped the return address into eip and released the right number of argument bytes.
 */
#ifndef HLE_TEST_HARNESS_H
#define HLE_TEST_HARNESS_H

#include <stdio.h>
#include <string.h>

#include "hle_imports.h"

extern sg_ctx T;        /* the test thread's context */
extern uint8_t *M;      /* guest window base */
extern int t_fail, t_pass;

#define G_HEAP 0x20000000u
#define G_STACK_TOP 0x30100000u
#define G_TEB 0x7ff00000u
#define G_RET 0x00401234u /* return address pushed by call_shim (never executed) */

void harness_init(void);

/* Calls `fn` with n 32-bit stack slots (args[0] is the first argument). Checks the stack
 * discipline for `cleanup` bytes and returns eax. */
uint32_t call_shim(sg_func *fn, unsigned cleanup, unsigned n, const uint32_t *args, const char *what);
#define CALL(fn, cleanup, ...) \
    call_shim((sg_func *)fn, cleanup, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__}, #fn)
#define CALL0(fn, cleanup) call_shim((sg_func *)fn, cleanup, 0, NULL, #fn)

/* Guest data helpers (bump-allocated in the guest heap). */
uint32_t gstr(const char *s);              /* narrow string */
uint32_t gwstr(const char *utf8_ascii);    /* UTF-16 copy of an ASCII/Latin-1 string */
uint32_t gwstr16(const uint16_t *s);       /* UTF-16 from units (NUL-terminated) */
uint32_t gbuf(uint32_t n);                 /* zeroed buffer */
uint32_t gdouble(double d, uint32_t *lo, uint32_t *hi); /* splits a double into two slots */
const char *hstr(uint32_t a);              /* guest narrow string as a host pointer */
int wstreq(uint32_t a, const char *ascii); /* compares a guest UTF-16 string with ASCII */

/* Register a host comparator / callback reachable through sg_call_guest at a guest address. */
typedef uint32_t (*host_cb)(sg_ctx *ctx, unsigned nargs, const uint32_t *args);
uint32_t register_callback(host_cb cb);

#define CHECK(cond) do { if (cond) t_pass++; else { t_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(got, want) do { const char *g_ = (got), *w_ = (want); \
    if (!strcmp(g_, w_)) t_pass++; else { t_fail++; \
    fprintf(stderr, "FAIL %s:%d: got \"%s\" want \"%s\"\n", __FILE__, __LINE__, g_, w_); } } while (0)
#define CHECK_EQ(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); \
    if (g_ == w_) t_pass++; else { t_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__, #got, g_, w_); } } while (0)

void test_table(void);
void test_string(void);
void test_conv(void);
void test_format(void);
void test_scan(void);
void test_net(void);
void test_steam(void);
void test_boot_stops(void);
void test_math(void);
void test_time(void);
void test_nls(void);
void test_misc(void);
void test_shell(void);   /* ShellExecuteW */

#endif
