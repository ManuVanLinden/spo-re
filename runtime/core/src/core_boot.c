/* core_boot.c -- the start-up hooks the image linker (runtime/image/src/main.c) calls:
 *   sg_core_startup(mem)  after the thunk region exists and before the IAT is installed
 *   sg_boot(mem, entry, argc, argv)  for --run
 * runtime/image defines WEAK defaults of both; these strong definitions replace them when
 * runtime/core is linked. Optional neighbours (runtime/win32) are found by name with dlsym, so
 * this file links with or without them. */
#include <dlfcn.h>

#include "core_internal.h"

/* Guest pages for thread stacks through runtime/win32's VirtualAlloc bookkeeping, so the guest
 * heap never hands out a stack and VirtualQuery sees it (w32.h: w32_vm_alloc/w32_vm_free). */
typedef uint32_t (*w32_vm_alloc_fn)(uint32_t addr, uint32_t size, uint32_t type, uint32_t protect, uint32_t *err);
typedef int (*w32_vm_free_fn)(uint32_t addr, uint32_t size, uint32_t type, uint32_t *err);
static w32_vm_alloc_fn w32_alloc;
static w32_vm_free_fn w32_free;
static uint32_t stack_alloc_w32(uint32_t size) {
    uint32_t a = w32_alloc(0, size, 0x3000u /* MEM_COMMIT|MEM_RESERVE */, 0x04u /* PAGE_READWRITE */, NULL);
    if (a) memset(sg_core_mem + a, 0, size);
    return a;
}
static void stack_free_w32(uint32_t a, uint32_t size) {
    (void)size;
    w32_free(a, 0, 0x8000u /* MEM_RELEASE */, NULL);
}

void sg_boot_diag_install(void); /* core_boot_diag.c */
extern const int sg_core_overrides_linked; /* core_overrides.c: referenced so the member is linked */

void sg_core_startup(uint8_t *mem) {
    unsigned bad = 0, n;
    void *p;
    sg_core_log(2, "sg_core_startup: sg_imp_* overrides %s", sg_core_overrides_linked ? "compiled in" : "off");
    if (!sg_core_mem) sg_core_init(mem);
    n = sg_core_register_hle_shims(&bad);
    sg_core_log(2, "sg_core_startup: %u core shims registered (%u refused)", n, bad);
    /* runtime/win32's CloseHandle forwards handles it does not own to w32_close_foreign
     * (runtime/win32/include/w32.h): point it at the core's handle table. */
    p = dlsym(RTLD_DEFAULT, "w32_close_foreign");
    if (p) *(int (**)(sg_ctx *, uint32_t))p = sg_core_close_foreign;
    *(void **)&w32_alloc = dlsym(RTLD_DEFAULT, "w32_vm_alloc");
    *(void **)&w32_free = dlsym(RTLD_DEFAULT, "w32_vm_free");
    if (w32_alloc && w32_free) {
        sg_core_vm_alloc = stack_alloc_w32;
        sg_core_vm_free = stack_free_w32;
        sg_core_log(2, "sg_core_startup: thread stacks come from runtime/win32's VirtualAlloc");
    }
}

/* Runs the PE entry on a guest main thread (1 MiB stack, the image's SizeOfStackReserve).
 * The command line is runtime/win32's business (GetCommandLineA, __getmainargs). Returns the
 * entry's eax if it ever returns; normally the CRT calls exit() first. */
int sg_boot(uint8_t *mem, uint32_t entry, int argc, char **argv) {
    sg_thread *t;
    (void)argc;
    (void)argv;
    if (!sg_core_mem) sg_core_init(mem);
    t = sg_self ? sg_self : sg_core_attach_main(0x100000u);
    if (!t) {
        fprintf(stderr, "sg_boot: cannot create the guest main thread\n");
        return 1;
    }
    core_set_host_thread_name("main");
    sg_boot_diag_install();
    sg_core_log(2, "sg_boot: calling entry 0x%08x (tid %u, stack 0x%08x-0x%08x)", entry, t->tid, t->stack_lo,
                t->stack_hi);
    return (int)sg_call_guest(&t->ctx, entry, SG_CC_CDECL, 0, NULL);
}
