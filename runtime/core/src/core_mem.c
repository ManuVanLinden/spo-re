/* core_mem.c -- the guest window, page protection, the stack/page allocator hook, PEB, init. */
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

#include "core_internal.h"

uint8_t *sg_core_mem;

uint8_t *sg_window_reserve(void) {
    void *p = mmap(NULL, SG_WINDOW_SIZE + SG_WINDOW_GUARD, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : (uint8_t *)p;
}

static uintptr_t host_page(void) {
    static uintptr_t ps;
    if (!ps) ps = (uintptr_t)sysconf(_SC_PAGESIZE);
    return ps;
}

int sg_window_protect(uint32_t a, uint32_t n, int prot) {
    uintptr_t ps = host_page();
    uintptr_t lo = (uintptr_t)(sg_core_mem + a) & ~(ps - 1);
    uintptr_t hi = ((uintptr_t)(sg_core_mem + a) + n + ps - 1) & ~(ps - 1);
    int p = prot == 3 ? PROT_READ | PROT_WRITE : prot == 1 ? PROT_READ : PROT_NONE;
    if (!sg_core_mem) return -1;
    return mprotect((void *)lo, hi - lo, p) ? -1 : 0;
}

/* ---- default page allocator: top-down in [STACK_REGION_LO, STACK_REGION_HI) -------------- */

#define VM_GRAN 0x10000u
#define VM_FREE_MAX 256
static pthread_mutex_t vm_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t vm_next = SG_CORE_STACK_REGION_HI;
static struct { uint32_t a, n; } vm_free_list[VM_FREE_MAX];
static unsigned vm_nfree;

static uint32_t default_vm_alloc(uint32_t size) {
    uint32_t a = 0;
    unsigned i;
    size = (size + VM_GRAN - 1) & ~(VM_GRAN - 1);
    pthread_mutex_lock(&vm_lock);
    for (i = 0; i < vm_nfree; i++)
        if (vm_free_list[i].n == size) {
            a = vm_free_list[i].a;
            vm_free_list[i] = vm_free_list[--vm_nfree];
            break;
        }
    if (!a && vm_next - SG_CORE_STACK_REGION_LO >= size) {
        vm_next -= size;
        a = vm_next;
    }
    pthread_mutex_unlock(&vm_lock);
    if (!a) return 0;
    if (sg_window_protect(a, size, 3)) return 0;
    memset(sg_core_mem + a, 0, size);
    return a;
}

static void default_vm_free(uint32_t a, uint32_t size) {
    size = (size + VM_GRAN - 1) & ~(VM_GRAN - 1);
    sg_window_protect(a, size, 0);
    /* give the pages back to the host; the reservation stays */
    madvise(sg_core_mem + a, size, MADV_FREE);
    pthread_mutex_lock(&vm_lock);
    if (vm_nfree < VM_FREE_MAX) {
        vm_free_list[vm_nfree].a = a;
        vm_free_list[vm_nfree].n = size;
        vm_nfree++;
    }
    pthread_mutex_unlock(&vm_lock);
}

sg_core_vm_alloc_fn sg_core_vm_alloc = default_vm_alloc;
sg_core_vm_free_fn sg_core_vm_free = default_vm_free;

uint32_t core_pages_alloc(uint32_t size) { return sg_core_vm_alloc(size); }
void core_pages_free(uint32_t a, uint32_t size) { sg_core_vm_free(a, size); }

/* ---- init ---------------------------------------------------------------------------------- */

static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static uint8_t *init_mem;

static void do_init(void) {
    uint8_t *mem = init_mem;
    sg_core_mem = mem;
    {
        const char *s = getenv("SPORE_CORE_LOG");
        if (s) sg_core_log_level = atoi(s);
    }
    /* thunk region: 0xCC bytes (int3), read-only (runtime-abi 7.1) */
    if (sg_window_protect(SG_THUNK_BASE, SG_THUNK_LIMIT - SG_THUNK_BASE, 3)) {
        fprintf(stderr, "sg_core_init: cannot commit the thunk region (errno %d)\n", errno);
        abort();
    }
    memset(mem + SG_THUNK_BASE, 0xcc, SG_THUNK_LIMIT - SG_THUNK_BASE);
    sg_window_protect(SG_THUNK_BASE, SG_THUNK_LIMIT - SG_THUNK_BASE, 1);
    /* TEB region + PEB: committed once (host pages are 16 KiB; TEBs are 4 KiB) */
    if (sg_window_protect(SG_TEB_REGION, SG_TEB_REGION_END - SG_TEB_REGION, 3)) {
        fprintf(stderr, "sg_core_init: cannot commit the TEB region\n");
        abort();
    }
    memset(mem + SG_PEB_ADDR, 0, 0x1000);
    sg_st8(mem, SG_PEB_ADDR + 0x02, 0);             /* BeingDebugged */
    sg_st32(mem, SG_PEB_ADDR + 0x08, SG_IMAGE_BASE); /* ImageBaseAddress */
    sg_st32(mem, SG_PEB_ADDR + 0x64, 1);             /* NumberOfProcessors (refined by the shim) */
    sg_st32(mem, SG_PEB_ADDR + 0xa4, 5);             /* OSMajorVersion: XP-era 5.1 */
    sg_st32(mem, SG_PEB_ADDR + 0xa8, 1);
    core_thunks_init();
    core_eh_register_thunks();
    core_kobj_init();
}

void sg_core_init(uint8_t *mem) {
    if (sg_core_mem && sg_core_mem != mem) {
        fprintf(stderr, "sg_core_init: already initialised with another window\n");
        abort();
    }
    init_mem = mem;
    pthread_once(&init_once, do_init);
}
