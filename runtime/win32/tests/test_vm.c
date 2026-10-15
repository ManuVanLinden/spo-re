/* test_vm.c -- VirtualAlloc / VirtualFree / VirtualQuery / VirtualProtect semantics. */
#include "harness.h"
#include "w32_int.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* Runs `fn(addr)` in a child; returns the signal that ended it (0 if it exited normally). */
static int child_signal(void (*fn)(uint32_t), uint32_t addr) {
    fflush(NULL);
    pid_t pid = fork();
    if (!pid) {
        signal(SIGBUS, SIG_DFL);
        signal(SIGSEGV, SIG_DFL);
        fn(addr);
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFSIGNALED(st) ? WTERMSIG(st) : 0;
}
static void touch_write(uint32_t a) { *(volatile uint8_t *)(M + a) = 1; }
static void touch_read(uint32_t a) { (void)*(volatile uint8_t *)(M + a); }

void test_vm(void) {
    uint32_t err = 0, old = 0;
    w32_mbi mb;

    /* reserve, then commit part of it, as EA's allocator does (0x926eed, 0x92747f) */
    uint32_t r = w32_vm_alloc(0, 0x30000, W32_MEM_RESERVE, W32_PAGE_READWRITE, &err);
    CHECK(r != 0);
    CHECK_EQ(r & 0xffff, 0);
    CHECK(r >= W32_VM_LO && r < W32_VM_HI);
    CHECK(w32_vm_query(r, &mb));
    CHECK_EQ(mb.state, W32_MEM_RESERVE);
    CHECK_EQ(mb.alloc_base, r);
    CHECK_EQ(mb.region_size, 0x30000);
    CHECK_EQ(mb.type, W32_MEM_PRIVATE);
    CHECK_EQ(mb.protect, 0);
    CHECK_EQ(mb.alloc_protect, W32_PAGE_READWRITE);

    uint32_t c = w32_vm_alloc(r + 0x1234, 0x2000, W32_MEM_COMMIT, W32_PAGE_READWRITE, &err);
    CHECK_EQ(c, r + 0x1000); /* rounded down to the page */
    CHECK(w32_vm_query(r, &mb));
    CHECK_EQ(mb.state, W32_MEM_RESERVE);
    CHECK_EQ(mb.region_size, 0x1000);
    CHECK(w32_vm_query(r + 0x1000, &mb));
    CHECK_EQ(mb.state, W32_MEM_COMMIT);
    CHECK_EQ(mb.region_size, 0x3000); /* 0x1234 + 0x2000 spans three pages */
    CHECK_EQ(mb.protect, W32_PAGE_READWRITE);
    CHECK_EQ(mb.base, r + 0x1000);
    /* committed memory reads zero and is writable */
    CHECK_EQ(sg_ld32(M, r + 0x1000), 0);
    sg_st32(M, r + 0x3ffc, 0xdeadbeef);
    CHECK_EQ(sg_ld32(M, r + 0x3ffc), 0xdeadbeef);
    /* a commit across the reservation's end fails */
    CHECK_EQ(w32_vm_alloc(r + 0x2f000, 0x2000, W32_MEM_COMMIT, W32_PAGE_READWRITE, &err), 0);
    CHECK_EQ(err, W32_ERROR_INVALID_ADDRESS);
    /* commit outside any reservation fails */
    CHECK_EQ(w32_vm_alloc(0x7f000000u, 0x1000, W32_MEM_COMMIT, W32_PAGE_READWRITE, &err), 0);
    CHECK_EQ(err, W32_ERROR_INVALID_ADDRESS);

    /* decommit, recommit: the page reads zero again (Windows guarantees it) */
    CHECK(w32_vm_free(r + 0x3000, 0x1000, W32_MEM_DECOMMIT, &err));
    CHECK(w32_vm_query(r + 0x3000, &mb));
    CHECK_EQ(mb.state, W32_MEM_RESERVE);
    CHECK_EQ(w32_vm_alloc(r + 0x3000, 0x1000, W32_MEM_COMMIT, W32_PAGE_READWRITE, &err), r + 0x3000);
    CHECK_EQ(sg_ld32(M, r + 0x3ffc), 0);

    /* protection changes are reported per page and the old value is returned */
    CHECK(w32_vm_protect(r + 0x1000, 0x1000, W32_PAGE_READONLY, &old, &err));
    CHECK_EQ(old, W32_PAGE_READWRITE);
    CHECK(w32_vm_query(r + 0x1000, &mb));
    CHECK_EQ(mb.protect, W32_PAGE_READONLY);
    CHECK_EQ(mb.region_size, 0x1000);
    CHECK(!w32_vm_protect(r + 0x10000, 0x1000, W32_PAGE_READONLY, &old, &err)); /* not committed */
    CHECK_EQ(err, W32_ERROR_INVALID_ADDRESS);

    /* release needs the base address and size 0 */
    CHECK(!w32_vm_free(r + 0x1000, 0, W32_MEM_RELEASE, &err));
    CHECK_EQ(err, W32_ERROR_INVALID_PARAMETER);
    CHECK(!w32_vm_free(r, 0x1000, W32_MEM_RELEASE, &err));
    CHECK(w32_vm_free(r, 0, W32_MEM_RELEASE, &err));
    CHECK(w32_vm_query(r, &mb));
    CHECK_EQ(mb.state, W32_MEM_FREE);
    CHECK_EQ(mb.alloc_base, 0);

    /* reserve+commit in one call, and at a fixed address */
    uint32_t a = w32_vm_alloc(0, 5000, W32_MEM_RESERVE | W32_MEM_COMMIT, W32_PAGE_READWRITE, &err);
    CHECK(a && !(a & 0xffff));
    CHECK(w32_vm_query(a, &mb));
    CHECK_EQ(mb.region_size, 0x2000);
    CHECK_EQ(mb.state, W32_MEM_COMMIT);
    uint32_t fixed = 0x30000000u;
    CHECK_EQ(w32_vm_alloc(fixed + 0x1234, 0x10000, W32_MEM_RESERVE | W32_MEM_COMMIT, W32_PAGE_READWRITE, &err), fixed);
    CHECK_EQ(w32_vm_alloc(fixed, 0x1000, W32_MEM_RESERVE, W32_PAGE_READWRITE, &err), 0); /* overlap */
    CHECK_EQ(err, W32_ERROR_INVALID_ADDRESS);
    /* MEM_COMMIT alone with a NULL address reserves too */
    uint32_t b = w32_vm_alloc(0, 0x1000, W32_MEM_COMMIT, W32_PAGE_READWRITE, &err);
    CHECK(b != 0);
    /* top-down allocations come from the top of the user range, below the runtime regions */
    uint32_t td = w32_vm_alloc(0, 0x10000, W32_MEM_RESERVE | W32_MEM_TOP_DOWN, W32_PAGE_READWRITE, &err);
    CHECK(td >= 0x70000000u && td + 0x10000 <= W32_VM_HI);
    /* the image, the thunk region and the TEBs are never handed out */
    CHECK_EQ(w32_vm_alloc(SG_IMAGE_BASE, 0x1000, W32_MEM_RESERVE, W32_PAGE_READWRITE, &err), 0);
    CHECK_EQ(w32_vm_alloc(SG_THUNK_BASE, 0x1000, W32_MEM_RESERVE, W32_PAGE_READWRITE, &err), 0);
    CHECK(w32_vm_query(0x401000, &mb));
    CHECK_EQ(mb.type, W32_MEM_IMAGE);
    CHECK_EQ(mb.alloc_base, SG_IMAGE_BASE);
    CHECK(!w32_vm_free(SG_IMAGE_BASE, 0, W32_MEM_RELEASE, &err));
    /* the null page and the top of the address space */
    CHECK(w32_vm_query(0, &mb));
    CHECK_EQ(mb.state, W32_MEM_FREE);
    CHECK(!w32_vm_query(0x7fff0000u, &mb));
    /* bad parameters */
    CHECK_EQ(w32_vm_alloc(0, 0, W32_MEM_RESERVE, W32_PAGE_READWRITE, &err), 0);
    CHECK_EQ(err, W32_ERROR_INVALID_PARAMETER);
    CHECK_EQ(w32_vm_alloc(0, 0x1000, W32_MEM_RESERVE, 0x3, &err), 0);
    CHECK_EQ(err, W32_ERROR_INVALID_PARAMETER);

    /* host protection really follows: decommitted / no-access pages fault, read-only rejects writes */
    uint32_t p = w32_vm_alloc(0, 0x20000, W32_MEM_RESERVE | W32_MEM_COMMIT, W32_PAGE_READWRITE, &err);
    CHECK_EQ(child_signal(touch_write, p + 0x8000), 0);
    CHECK(w32_vm_free(p + 0x10000, 0x10000, W32_MEM_DECOMMIT, &err));
    int sig = child_signal(touch_read, p + 0x10000);
    CHECK(sig == SIGBUS || sig == SIGSEGV);
    CHECK(w32_vm_protect(p, 0x4000, W32_PAGE_READONLY, &old, &err)); /* one whole host page */
    sig = child_signal(touch_write, p);
    CHECK(sig == SIGBUS || sig == SIGSEGV);
    CHECK_EQ(child_signal(touch_read, p), 0);
    CHECK(w32_vm_protect(p, 0x4000, W32_PAGE_NOACCESS, &old, &err));
    sig = child_signal(touch_read, p);
    CHECK(sig == SIGBUS || sig == SIGSEGV);

    /* runtime allocations in the reserved top regions */
    uint32_t teb2 = w32_vm_alloc_runtime(SG_TEB_REGION + 0x4000, 0x1000, W32_PAGE_READWRITE);
    CHECK_EQ(teb2, SG_TEB_REGION + 0x4000);
    CHECK_EQ(w32_vm_alloc_runtime(SG_TEB_REGION + 0x4000, 0x1000, W32_PAGE_READWRITE), 0); /* already committed */
    sg_st32(M, teb2, 7);
    CHECK_EQ(sg_ld32(M, teb2), 7);

    /* shims: VirtualAlloc / VirtualQuery / VirtualFree through the calling convention */
    uint32_t va = CALL(w32_imp_kernel32_VirtualAlloc, 16, 0, 0x10000, W32_MEM_RESERVE, W32_PAGE_READWRITE);
    CHECK(va != 0);
    uint32_t mbi = gbuf(28);
    CHECK_EQ(CALL(w32_imp_kernel32_VirtualQuery, 12, va + 0x10, mbi, 28), 28);
    CHECK_EQ(sg_ld32(M, mbi + 0), va);
    CHECK_EQ(sg_ld32(M, mbi + 16), W32_MEM_RESERVE);
    CHECK_EQ(CALL(w32_imp_kernel32_VirtualQuery, 12, va, mbi, 20), 0);
    CHECK_EQ(last_error(), W32_ERROR_BAD_LENGTH);
    CHECK_EQ(CALL(w32_imp_kernel32_VirtualAlloc, 16, va + 0x2000, 0x1000, W32_MEM_COMMIT, W32_PAGE_READWRITE), va + 0x2000);
    CHECK_EQ(CALL(w32_imp_kernel32_VirtualFree, 12, va, 0, W32_MEM_RELEASE), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_VirtualFree, 12, va, 0, W32_MEM_RELEASE), 0);
    CHECK_EQ(last_error(), W32_ERROR_INVALID_ADDRESS);
    uint32_t oldp = gbuf(4);
    uint32_t v2 = CALL(w32_imp_kernel32_VirtualAlloc, 16, 0, 0x1000, W32_MEM_COMMIT, W32_PAGE_READWRITE);
    CHECK_EQ(CALL(w32_imp_kernel32_VirtualProtect, 16, v2, 0x1000, W32_PAGE_READONLY, oldp), 1);
    CHECK_EQ(sg_ld32(M, oldp), W32_PAGE_READWRITE);
    CHECK_EQ(CALL(w32_imp_kernel32_VirtualProtect, 16, v2, 0x1000, W32_PAGE_READWRITE, 0), 0);
    CHECK_EQ(last_error(), W32_ERROR_NOACCESS);

    /* many reservations: first fit reuses holes */
    uint32_t hole[64];
    for (int i = 0; i < 64; i++) hole[i] = w32_vm_alloc(0, 0x10000, W32_MEM_RESERVE, W32_PAGE_READWRITE, &err);
    CHECK(w32_vm_free(hole[10], 0, W32_MEM_RELEASE, &err));
    CHECK_EQ(w32_vm_alloc(0, 0x10000, W32_MEM_RESERVE, W32_PAGE_READWRITE, &err), hole[10]);
    for (int i = 0; i < 64; i++) w32_vm_free(hole[i], 0, W32_MEM_RELEASE, &err);
    w32_vm_stats st;
    w32_vm_get_stats(&st);
    CHECK(st.allocations > 0 && st.committed_bytes <= st.reserved_bytes);
}
