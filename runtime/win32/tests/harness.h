/* harness.h -- host-only tests for runtime/win32. No game code or game bytes.
 *
 * Reserves the 4 GiB guest window, runs w32_init on it (so the real VM and heap are under
 * test), gives the test thread a TEB and a guest stack from the VM, and calls shims exactly
 * as translated code does (arguments on the guest stack, a return address, then the sg_func),
 * checking eip, the stack clean-up and the callee-saved registers afterwards.
 */
#ifndef W32_TEST_HARNESS_H
#define W32_TEST_HARNESS_H

#include <stdio.h>
#include <string.h>

#include "hle.h"
#include "w32.h"
#include "w32_shims.h"

extern sg_ctx T;
extern uint8_t *M;
extern int t_fail, t_pass;
extern char t_root[]; /* host scratch directory for this run (build/win32/test-root) */

#define G_RET 0x00401234u

void harness_init(void);
/* Re-runs w32_init with a fresh scratch tree (paths, registry); VM and heaps start over. */
void harness_reset(void);

uint32_t call_shim(sg_func *fn, unsigned cleanup, unsigned n, const uint32_t *args, const char *what);
#define CALL(fn, cleanup, ...) \
    call_shim((sg_func *)fn, cleanup, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__}, #fn)
#define CALL0(fn, cleanup) call_shim((sg_func *)fn, cleanup, 0, NULL, #fn)

uint32_t gstr(const char *s);   /* guest narrow (1252) copy of a UTF-8 string */
uint32_t gwstr(const char *s);  /* guest UTF-16 copy of a UTF-8 string */
uint32_t gbuf(uint32_t n);      /* zeroed guest buffer */
const char *hwstr(uint32_t a);  /* guest UTF-16 string as UTF-8 (static buffer, rotates 8) */
uint32_t last_error(void);

typedef uint32_t (*host_cb)(sg_ctx *ctx, unsigned nargs, const uint32_t *args);
uint32_t register_callback(host_cb cb);

#define CHECK(cond) do { if (cond) t_pass++; else { t_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(got, want) do { const char *g_ = (got), *w_ = (want); \
    if (g_ && !strcmp(g_, w_)) t_pass++; else { t_fail++; \
    fprintf(stderr, "FAIL %s:%d: got \"%s\" want \"%s\"\n", __FILE__, __LINE__, g_ ? g_ : "(null)", w_); } } while (0)
#define CHECK_EQ(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); \
    if (g_ == w_) t_pass++; else { t_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s = %lld (0x%llx), want %lld (0x%llx)\n", __FILE__, __LINE__, #got, g_, \
            (unsigned long long)g_, w_, (unsigned long long)w_); } } while (0)

void test_vm(void);
void test_heap(void);
void test_path(void);
void test_reg(void);
void test_file(void);
void test_crt(void);
void test_module(void);
void bench_heap(void);

#endif
