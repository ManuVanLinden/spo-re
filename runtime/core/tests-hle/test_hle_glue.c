/* test_hle_glue.c -- links the runtime core with runtime/hle and checks the registration
 * glue: sg_core_register_hle_shims() replaces the table entries, hle_install_imports() then puts
 * thunks for the core shims into the IAT, and a call through the IAT reaches the core.
 * Uses a scratch window with no game bytes (the IAT range is just committed memory here). */
#include <stdio.h>
#include <string.h>

#include "hle.h"
#ifdef HLE_PATCHED
#include "hle_imports.h"
#endif
#include "sg_core.h"

SGC_SHIM(kernel32, TlsGetValue);
SGC_SHIM(kernel32, TlsSetValue);
SGC_SHIM(kernel32, TlsAlloc);

/* image-linker symbols and the guest heap, minimal stand-ins */
const uint32_t sg_dispatch_bucket[SG_DISPATCH_BUCKETS + 1];
const sg_dispatch_entry sg_dispatch_entries[1];
const sg_resume_point sg_resume_points[1];
const uint32_t sg_resume_point_count = 0;
static sg_func *miss(sg_ctx *ctx, uint8_t *mem, uint32_t a) { (void)mem; sg_core_fatal(ctx, "miss 0x%08x", a); }
sg_miss_hook_fn sg_miss_hook = miss;
static uint32_t heap = 0x02000000u;
uint32_t hle_guest_alloc(uint32_t size, uint32_t align) {
    uint32_t a;
    if (!align) align = 1;
    heap = (heap + align - 1) & ~(align - 1);
    a = heap;
    heap += size;
    return a;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    uint8_t *mem = sg_window_reserve();
    sg_thread *t;
    unsigned n, i, found = 0;
    sg_core_init(mem);
    sg_window_protect(0x01300000u, 0x00200000u, 3); /* IAT page */
    sg_window_protect(0x02000000u, 0x00100000u, 3); /* heap */
    t = sg_core_attach_main(0);
    {
        unsigned bad = 0;
        n = sg_core_register_hle_shims(&bad);
        printf("core shims registered in the HLE table: %u of %u (%u clean-up mismatches)\n", n, sg_core_shim_count, bad);
        CHECK(n >= 50, "only %u registered", n);
        CHECK(bad == 0, "%u shims disagree with the HLE table's clean-up bytes", bad);
    }
    hle_install_imports(mem);
    for (i = 0; i < hle_import_count; i++) {
        if (!strcmp(hle_import_info[i].symbol, "sg_imp_kernel32_TlsGetValue")) {
            uint32_t thunk = sg_ld32(mem, hle_imports[i].iat), v, arg[1];
            found = 1;
            CHECK(sg_thunk_fn[(thunk - SG_THUNK_BASE) / SG_THUNK_SLOT] == sgc_kernel32_TlsGetValue,
                  "IAT slot does not reach the core shim");
            /* a guest call through the IAT value (runtime-abi 7.2 path 2: mov reg,[IAT]; call reg) */
            sg_st32(mem, t->ctx.fs_base + SG_TEB_TLS_SLOTS + 4 * 5, 0x1234);
            arg[0] = 5;
            v = sg_call_guest(&t->ctx, thunk, SG_CC_STDCALL, 1, arg);
            CHECK(v == 0x1234, "slot 5 reads 0x%x through the IAT", v);
        }
    }
    CHECK(found, "TlsGetValue not in the import table");
#ifdef HLE_PATCHED
    /* runtime-abi 7.2 path 1: translated code calls the stub symbol directly (SG_CALL); with the
     * forwarding-stub patch it must reach the core shim, not the "unimplemented" abort */
    {
        sg_ctx *ctx = &t->ctx;
        uint32_t esp0 = ctx->esp;
        sg_st32(mem, ctx->fs_base + SG_TEB_TLS_SLOTS + 4 * 6, 0x5678);
        ctx->esp -= 4; sg_st32(mem, ctx->esp, 6);
        ctx->esp -= 4; sg_st32(mem, ctx->esp, 0x00abcdefu);
        sg_imp_kernel32_TlsGetValue(ctx, mem);
        CHECK(ctx->eip == 0x00abcdefu && ctx->esp == esp0, "direct call: bad return (eip %08x)", ctx->eip);
        CHECK(ctx->eax == 0x5678, "direct call to the stub symbol returned 0x%x", ctx->eax);
        printf("direct SG_CALL(sg_imp_kernel32_TlsGetValue) forwarded to the core shim\n");
    }
#endif
    /* every core shim whose symbol is an HLE import now sits in the table */
    for (i = 0; i < hle_import_count; i++) {
        sg_func *f = sg_core_shim_lookup(hle_import_info[i].symbol);
        if (f) CHECK(hle_imports[i].fn == f, "%s not replaced", hle_import_info[i].symbol);
    }
    printf("hle glue: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
