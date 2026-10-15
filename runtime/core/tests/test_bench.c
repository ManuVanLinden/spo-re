/* test_bench.c -- rough per-operation costs of the hot runtime paths (run: core_tests bench). */
#include <time.h>

#include "core_internal.h"
#include "fixture.h"

SGC_SHIM(kernel32, TlsGetValue);
SGC_SHIM(kernel32, EnterCriticalSection);
SGC_SHIM(kernel32, LeaveCriticalSection);
SGC_SHIM(kernel32, InitializeCriticalSection);
SGC_SHIM(kernel32, SetEvent);
SGC_SHIM(kernel32, CreateEventA);
SGC_SHIM(kernel32, WaitForSingleObject);
extern void test_eh_prepare_catcher(uint32_t callee);

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}
static void noop(sg_ctx *ctx, uint8_t *mem) { SG_RET(0); }
static void landing_owner(sg_ctx *ctx, uint8_t *mem) {
    SG_LANDING_BEGIN(FX_FN(400));
    SG_LANDING_RET(0);
SG_RESUME_DISPATCH:
    sg_bad_resume(ctx);
}

void test_bench(void) {
    const int N = 2000000;
    double t0, dt;
    int i;
    uint32_t idx = 3, cs = fx_alloc(24), ev;
    sg_ctx *ctx = C;
    uint8_t *mem = M;
    t_name = "bench";
    fx_register(FX_FN(401), noop);
    t0 = now_ns();
    for (i = 0; i < N; i++) { SG_PUSH32(idx); SG_CALL(sgc_kernel32_TlsGetValue, 0x00ff0000u); }
    dt = now_ns() - t0;
    printf("  TlsGetValue shim (direct SG_CALL, push+call+ret check): %.2f ns\n", dt / N);
    GSTD(sgc_kernel32_InitializeCriticalSection, cs);
    t0 = now_ns();
    for (i = 0; i < N; i++) {
        SG_PUSH32(cs); SG_CALL(sgc_kernel32_EnterCriticalSection, 0x00ff0004u);
        SG_PUSH32(cs); SG_CALL(sgc_kernel32_LeaveCriticalSection, 0x00ff0008u);
    }
    dt = now_ns() - t0;
    printf("  Enter+LeaveCriticalSection, uncontended: %.2f ns\n", dt / N);
    ev = GSTD(sgc_kernel32_CreateEventA, 0u, 0u, 0u, 0u);
    t0 = now_ns();
    for (i = 0; i < N / 10; i++) {
        SG_PUSH32(ev); SG_CALL(sgc_kernel32_SetEvent, 0x00ff000cu);
        SG_PUSH32(0); SG_PUSH32(ev); SG_CALL(sgc_kernel32_WaitForSingleObject, 0x00ff0010u);
    }
    dt = now_ns() - t0;
    printf("  SetEvent+WaitForSingleObject (signalled, same thread): %.2f ns\n", dt / (N / 10));
    t0 = now_ns();
    for (i = 0; i < N; i++) sg_call_guest(C, FX_FN(401), SG_CC_CDECL, 0, NULL);
    dt = now_ns() - t0;
    printf("  sg_call_guest to an empty function (dispatch lookup + checks): %.2f ns\n", dt / N);
    fx_register(FX_FN(400), landing_owner);
    t0 = now_ns();
    for (i = 0; i < N; i++) { SG_CALL(landing_owner, 0x00ff0014u); }
    dt = now_ns() - t0;
    printf("  entry+exit of a landing owner (host _setjmp): %.2f ns\n", dt / N);
    test_eh_prepare_catcher(FX_FN(3));
    t0 = now_ns();
    for (i = 0; i < 20000; i++) sg_call_guest(C, FX_FN(20), SG_CC_CDECL, 0, NULL);
    dt = now_ns() - t0;
    printf("  C++ throw + catch + resume (catcher -> thrower): %.0f ns\n", dt / 20000);
    t_pass++;
}
