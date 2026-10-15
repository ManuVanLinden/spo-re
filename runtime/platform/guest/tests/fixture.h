/* fixture.h -- host-only harness for the guest window layer tests (see fixture.c). */
#ifndef SGP_FIXTURE_H
#define SGP_FIXTURE_H

#include <stdio.h>
#include <string.h>

#include "sgp_int.h"

extern int t_pass, t_fail;
#define CHECK(c, ...) do { if (c) t_pass++; else { t_fail++; fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define CHECK_EQ(a, b) do { unsigned long long a_ = (unsigned long long)(a), b_ = (unsigned long long)(b); \
        CHECK(a_ == b_, "%s = 0x%llx, expected 0x%llx", #a, a_, b_); } while (0)

extern uint8_t *M;
extern sg_ctx *C;
#define FX_DATA 0x01800000u
#define FX_FN(k) (0x00410000u + 0x100u * (uint32_t)(k))
int fx_init(void);
void fx_register(uint32_t guest, sg_func *fn);
/* Calls an import as translated code does (SG_CALL with arguments pushed): returns eax and
 * checks the stdcall clean-up and the return address. */
uint32_t gcall(sg_ctx *ctx, sg_func *fn, unsigned n, const uint32_t *args);
#define GSTD(fn, ...) gcall(C, fn, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__})
#define GSTD0(fn) gcall(C, fn, 0, NULL)

#endif
