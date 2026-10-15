/* w32_heap.c -- guest heaps (HeapCreate family), GlobalAlloc/LocalAlloc, the msvcr90 malloc
 * family, hle_guest_alloc, and memory-status queries.
 *
 * Allocator: every block has a 16-byte header in guest memory just below the user pointer:
 *     +0 requested size   +4 size class (0xff = large)   +8 heap handle   +12 magic
 * Small requests (<= 32 KiB) use 44 size classes (16-byte steps to 256, then four classes per
 * power of two) with one LIFO free list per class per heap, linked through the first word of
 * each free block, and are bump-carved from 1 MiB segments committed with w32_vm_alloc. Larger
 * requests get their own VirtualAlloc region. User pointers are 16-aligned (Windows gives 8).
 * One mutex per heap; HEAP_NO_SERIALIZE is ignored. Memory of small classes is reused but never
 * returned to the VM; large blocks are released on free.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "w32_int.h"

#define HDR 16u
#define MAGIC_LIVE 0x4b485053u /* 'SPHK' */
#define MAGIC_FREE 0x45455246u /* 'FREE' */
#define NCLASS 44
#define SMALL_MAX 32768u
#define SEG_SIZE 0x100000u
#define LARGE_CLASS 0xffu
#define MAX_HEAPS 128

static uint32_t cls_size[NCLASS];

typedef struct heap {
    uint32_t handle; /* guest address of its 64-byte control block */
    uint32_t flags;
    pthread_mutex_t lock;
    uint32_t free_head[NCLASS];
    uint32_t bump, bump_end;
    uint32_t *segs; unsigned nsegs, capsegs;
    uint32_t *large; unsigned nlarge, caplarge; /* open-addressing set of large user pointers */
    uint64_t live_bytes, live_blocks;
} heap;

static heap *heaps[MAX_HEAPS];
static pthread_mutex_t heaps_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t process_heap, crt_heap, runtime_heap;

static void init_classes(void) {
    int c = 0;
    for (uint32_t s = 16; s <= 256; s += 16) cls_size[c++] = s;
    for (uint32_t b = 8; b <= 14; b++)
        for (uint32_t k = 1; k <= 4; k++) cls_size[c++] = (1u << b) + k * (1u << (b - 2));
}

static int class_of(uint32_t n) {
    if (n <= 256) return n ? (int)((n + 15) / 16) - 1 : 0;
    uint32_t b = 31u - (uint32_t)__builtin_clz(n - 1); /* n in (2^b, 2^(b+1)] */
    uint32_t step = 1u << (b - 2), k = (n - (1u << b) + step - 1) / step;
    return 16 + (int)(b - 8) * 4 + (int)k - 1;
}

static heap *find_heap(uint32_t h) {
    for (int i = 0; i < MAX_HEAPS; i++)
        if (heaps[i] && heaps[i]->handle == h) return heaps[i];
    return NULL;
}

static void hdr_set(uint32_t user, uint32_t size, uint32_t cls, uint32_t hh, uint32_t magic) {
    uint8_t *m = w32_mem;
    sg_st32(m, user - 16, size);
    sg_st32(m, user - 12, cls);
    sg_st32(m, user - 8, hh);
    sg_st32(m, user - 4, magic);
}

/* ---- large-block set ------------------------------------------------------------------ */

static unsigned lhash(uint32_t p, unsigned cap) { return (unsigned)((p >> 4) * 2654435761u) & (cap - 1); }
static void large_insert(heap *h, uint32_t p);
static void large_grow(heap *h) {
    uint32_t *old = h->large;
    unsigned oc = h->caplarge;
    h->caplarge = oc ? oc * 2 : 64;
    h->large = (uint32_t *)calloc(h->caplarge, 4);
    h->nlarge = 0;
    for (unsigned i = 0; i < oc; i++)
        if (old[i] && old[i] != 1) large_insert(h, old[i]);
    free(old);
}
static void large_insert(heap *h, uint32_t p) {
    if ((h->nlarge + 1) * 2 > h->caplarge) large_grow(h);
    unsigned i = lhash(p, h->caplarge);
    while (h->large[i] && h->large[i] != 1) i = (i + 1) & (h->caplarge - 1);
    h->large[i] = p;
    h->nlarge++;
}
static int large_find(heap *h, uint32_t p, unsigned *slot) {
    if (!h->caplarge) return 0;
    unsigned i = lhash(p, h->caplarge);
    while (h->large[i]) {
        if (h->large[i] == p) { if (slot) *slot = i; return 1; }
        i = (i + 1) & (h->caplarge - 1);
    }
    return 0;
}

/* ---- core ------------------------------------------------------------------------------ */

static uint32_t carve(heap *h, uint32_t bytes) {
    if (h->bump_end - h->bump < bytes) {
        uint32_t err, seg = w32_vm_alloc(0, SEG_SIZE, W32_MEM_RESERVE | W32_MEM_COMMIT, W32_PAGE_READWRITE, &err);
        if (!seg) return 0;
        if (h->nsegs == h->capsegs) {
            h->capsegs = h->capsegs ? h->capsegs * 2 : 16;
            h->segs = (uint32_t *)realloc(h->segs, h->capsegs * 4);
        }
        h->segs[h->nsegs++] = seg;
        h->bump = seg;
        h->bump_end = seg + SEG_SIZE;
    }
    uint32_t b = h->bump;
    h->bump += bytes;
    return b;
}

static uint32_t alloc_locked(heap *h, uint32_t flags, uint32_t size) {
    uint32_t user;
    if (size > 0x7ffdefffu) return 0; /* HeapAlloc's limit for a 2 GiB process */
    if (size <= SMALL_MAX) {
        int c = class_of(size);
        user = h->free_head[c];
        if (user) {
            h->free_head[c] = sg_ld32(w32_mem, user);
            if (flags & W32_HEAP_ZERO_MEMORY) memset(w32_mem + user, 0, cls_size[c]);
            else sg_st32(w32_mem, user, 0);
        } else {
            uint32_t b = carve(h, cls_size[c] + HDR);
            if (!b) return 0;
            user = b + HDR; /* fresh segment memory is zero */
        }
        hdr_set(user, size, (uint32_t)c, h->handle, MAGIC_LIVE);
    } else {
        uint32_t err, total = (size + HDR + W32_PAGE - 1) & ~(W32_PAGE - 1u);
        uint32_t b = w32_vm_alloc(0, total, W32_MEM_RESERVE | W32_MEM_COMMIT, W32_PAGE_READWRITE, &err);
        if (!b) return 0;
        user = b + HDR;
        hdr_set(user, size, LARGE_CLASS, h->handle, MAGIC_LIVE);
        large_insert(h, user);
    }
    h->live_bytes += size;
    h->live_blocks++;
    return user;
}

/* Checks a user pointer against this heap. Returns its class, or -1. */
static int check_block(heap *h, uint32_t p) {
    if (p < W32_VM_LO + HDR || p >= W32_VM_HI || (p & 15u)) return -1;
    if (!w32_vm_is_committed(p - HDR, HDR)) return -1;
    if (sg_ld32(w32_mem, p - 4) != MAGIC_LIVE || sg_ld32(w32_mem, p - 8) != h->handle) return -1;
    uint32_t c = sg_ld32(w32_mem, p - 12);
    if (c == LARGE_CLASS) return large_find(h, p, NULL) ? (int)LARGE_CLASS : -1;
    if (c >= NCLASS || sg_ld32(w32_mem, p - 16) > cls_size[c]) return -1;
    return (int)c;
}

static void free_locked(heap *h, uint32_t p, int c) {
    h->live_bytes -= sg_ld32(w32_mem, p - 16);
    h->live_blocks--;
    sg_st32(w32_mem, p - 4, MAGIC_FREE);
    if (c == (int)LARGE_CLASS) {
        unsigned slot;
        uint32_t err;
        if (large_find(h, p, &slot)) { h->large[slot] = 1; /* tombstone */ h->nlarge--; }
        w32_vm_free(p - HDR, 0, W32_MEM_RELEASE, &err);
        return;
    }
    sg_st32(w32_mem, p, h->free_head[c]);
    h->free_head[c] = p;
}

/* ---- public host API --------------------------------------------------------------------- */

uint32_t w32_heap_create(uint32_t flags, uint32_t initial, uint32_t maximum) {
    (void)initial; (void)maximum; /* growable always; `maximum` is not enforced (design doc 3.3) */
    heap *h = (heap *)calloc(1, sizeof *h);
    if (!h) return 0;
    pthread_mutex_init(&h->lock, NULL);
    h->flags = flags;
    /* control block: its own small allocation from a fresh segment of this heap */
    uint32_t cb = carve(h, 64);
    if (!cb) { free(h); return 0; }
    h->handle = cb;
    sg_st32(w32_mem, cb, 0x50414548u); /* 'HEAP' */
    sg_st32(w32_mem, cb + 4, flags);
    pthread_mutex_lock(&heaps_lock);
    for (int i = 0; i < MAX_HEAPS; i++)
        if (!heaps[i]) { heaps[i] = h; pthread_mutex_unlock(&heaps_lock); return cb; }
    pthread_mutex_unlock(&heaps_lock);
    free(h);
    return 0;
}

int w32_heap_destroy(uint32_t hh) {
    heap *h;
    uint32_t err;
    if (hh == process_heap || hh == crt_heap || hh == runtime_heap) return 0; /* as Windows: not the process heap */
    pthread_mutex_lock(&heaps_lock);
    h = find_heap(hh);
    if (h)
        for (int i = 0; i < MAX_HEAPS; i++)
            if (heaps[i] == h) heaps[i] = NULL;
    pthread_mutex_unlock(&heaps_lock);
    if (!h) return 0;
    for (unsigned i = 0; i < h->caplarge; i++)
        if (h->large[i] > 1) w32_vm_free(h->large[i] - HDR, 0, W32_MEM_RELEASE, &err);
    for (unsigned i = 0; i < h->nsegs; i++) w32_vm_free(h->segs[i], 0, W32_MEM_RELEASE, &err);
    free(h->large);
    free(h->segs);
    pthread_mutex_destroy(&h->lock);
    free(h);
    return 1;
}

uint32_t w32_heap_alloc(uint32_t hh, uint32_t flags, uint32_t size) {
    heap *h = find_heap(hh);
    uint32_t r;
    if (!h) return 0;
    pthread_mutex_lock(&h->lock);
    r = alloc_locked(h, flags | (h->flags & W32_HEAP_ZERO_MEMORY), size);
    pthread_mutex_unlock(&h->lock);
    if (!r) hle_log(HLE_LOG_WARN, "HeapAlloc(0x%08x, %u bytes) failed", hh, size);
    return r;
}

int w32_heap_free(uint32_t hh, uint32_t flags, uint32_t p) {
    heap *h = find_heap(hh);
    int c;
    (void)flags;
    if (!p) return 1; /* HeapFree(NULL) succeeds */
    if (!h) return 0;
    pthread_mutex_lock(&h->lock);
    c = check_block(h, p);
    if (c >= 0) free_locked(h, p, c);
    pthread_mutex_unlock(&h->lock);
    if (c < 0) hle_log(HLE_LOG_WARN, "HeapFree(0x%08x, 0x%08x): not a live block of this heap", hh, p);
    return c >= 0;
}

uint32_t w32_heap_size(uint32_t hh, uint32_t flags, uint32_t p) {
    heap *h = find_heap(hh);
    uint32_t r = 0xffffffffu;
    (void)flags;
    if (!h) return r;
    pthread_mutex_lock(&h->lock);
    if (check_block(h, p) >= 0) r = sg_ld32(w32_mem, p - 16);
    pthread_mutex_unlock(&h->lock);
    return r;
}

int w32_heap_validate_ptr(uint32_t hh, uint32_t p) {
    heap *h = find_heap(hh);
    int c;
    if (!h) return 0;
    pthread_mutex_lock(&h->lock);
    c = check_block(h, p);
    pthread_mutex_unlock(&h->lock);
    return c >= 0;
}

uint32_t w32_heap_realloc(uint32_t hh, uint32_t flags, uint32_t p, uint32_t size) {
    heap *h = find_heap(hh);
    uint32_t r = 0, old;
    int c;
    if (!h || !p) return 0;
    pthread_mutex_lock(&h->lock);
    c = check_block(h, p);
    if (c < 0) goto out;
    old = sg_ld32(w32_mem, p - 16);
    uint32_t cap = c == (int)LARGE_CLASS ? 0 : cls_size[c];
    if (c == (int)LARGE_CLASS) {
        w32_mbi mb;
        w32_vm_query(p - HDR, &mb);
        cap = mb.region_size - HDR; /* committed bytes of the region */
    }
    /* stay in place when the block's capacity suffices and a small block does not get much smaller */
    if (size <= cap && (c == (int)LARGE_CLASS ? size > SMALL_MAX : class_of(size) == c || size > cap / 2)) {
        if ((flags | h->flags) & W32_HEAP_ZERO_MEMORY && size > old) memset(w32_mem + p + old, 0, size - old);
        sg_st32(w32_mem, p - 16, size);
        h->live_bytes += (uint64_t)size - old;
        r = p;
        goto out;
    }
    if (flags & W32_HEAP_REALLOC_IN_PLACE_ONLY) goto out;
    r = alloc_locked(h, flags & W32_HEAP_ZERO_MEMORY, size);
    if (r) {
        memcpy(w32_mem + r, w32_mem + p, old < size ? old : size);
        if ((flags | h->flags) & W32_HEAP_ZERO_MEMORY && size > old) memset(w32_mem + r + old, 0, size - old);
        free_locked(h, p, check_block(h, p));
    }
out:
    pthread_mutex_unlock(&h->lock);
    return r;
}

uint32_t w32_process_heap(void) { return process_heap; }
uint32_t w32_crt_heap(void) { return crt_heap; }

/* msvcr90 malloc family over _crtheap (heapinit.c creates it with HeapCreate(0, 4096, 0)). */
uint32_t w32_malloc(uint32_t n) {
    if (n > 0xffffffe0u) return 0; /* _HEAP_MAXREQ */
    return w32_heap_alloc(crt_heap, 0, n ? n : 1);
}
void w32_free(uint32_t p) { if (p) w32_heap_free(crt_heap, 0, p); }
uint32_t w32_realloc(uint32_t p, uint32_t n) {
    if (!p) return w32_malloc(n);
    if (!n) { w32_free(p); return 0; }
    if (n > 0xffffffe0u) return 0;
    return w32_heap_realloc(crt_heap, 0, p, n);
}
uint32_t w32_calloc(uint32_t n, uint32_t sz) {
    uint64_t t = (uint64_t)n * sz;
    if (t > 0xffffffe0u) return 0;
    return w32_heap_alloc(crt_heap, W32_HEAP_ZERO_MEMORY, t ? (uint32_t)t : 1);
}
uint32_t w32_msize(uint32_t p) { return w32_heap_size(crt_heap, 0, p); }

/* Runtime-owned guest data (hle.h contract): zero-filled, never freed. */
uint32_t hle_guest_alloc(uint32_t size, uint32_t align) {
    if (!runtime_heap) w32_fatal("hle_guest_alloc before w32_init");
    if (align <= 16) return w32_heap_alloc(runtime_heap, W32_HEAP_ZERO_MEMORY, size);
    uint32_t p = w32_heap_alloc(runtime_heap, W32_HEAP_ZERO_MEMORY, size + align);
    return p ? (p + align - 1) & ~(align - 1) : 0;
}

void w32_heap_init(void) {
    for (int i = 0; i < MAX_HEAPS; i++) heaps[i] = NULL; /* tests re-initialise; old heaps leak */
    init_classes();
    runtime_heap = w32_heap_create(0, 0, 0);
    process_heap = w32_heap_create(0, 0, 0);
    crt_heap = w32_heap_create(0, 4096, 0);
    if (!runtime_heap || !process_heap || !crt_heap) w32_fatal("heap: cannot create the initial heaps");
}

/* ---- shims: KERNEL32 heap ------------------------------------------------------------- */

static void set_heap_err(sg_ctx *ctx, uint8_t *mem, uint32_t ok) {
    if (!ok) hle_set_last_error(ctx, mem, W32_ERROR_NOT_ENOUGH_MEMORY);
}

W32_IMPORT(kernel32, GetProcessHeap) { SG_RETURN_STDCALL(process_heap, 0); }
W32_IMPORT(kernel32, HeapCreate) { SG_RETURN_STDCALL(w32_heap_create(SG_ARG(0), SG_ARG(1), SG_ARG(2)), 12); }
W32_IMPORT(kernel32, HeapDestroy) { SG_RETURN_STDCALL(w32_heap_destroy(SG_ARG(0)), 4); }
W32_IMPORT(kernel32, HeapAlloc) {
    uint32_t r = w32_heap_alloc(SG_ARG(0), SG_ARG(1), SG_ARG(2));
    set_heap_err(ctx, mem, r);
    SG_RETURN_STDCALL(r, 12);
}
W32_IMPORT(kernel32, HeapFree) {
    int ok = w32_heap_free(SG_ARG(0), SG_ARG(1), SG_ARG(2));
    if (!ok) hle_set_last_error(ctx, mem, W32_ERROR_INVALID_PARAMETER);
    SG_RETURN_STDCALL(ok, 12);
}
W32_IMPORT(kernel32, HeapReAlloc) {
    uint32_t r = w32_heap_realloc(SG_ARG(0), SG_ARG(1), SG_ARG(2), SG_ARG(3));
    set_heap_err(ctx, mem, r);
    SG_RETURN_STDCALL(r, 16);
}
W32_IMPORT(kernel32, HeapSize) { SG_RETURN_STDCALL(w32_heap_size(SG_ARG(0), SG_ARG(1), SG_ARG(2)), 12); }

/* GlobalAlloc/LocalAlloc: always fixed memory from the process heap; a GMEM_MOVEABLE "handle"
 * is the pointer itself, which GlobalLock returns unchanged. */
enum { GMEM_ZEROINIT = 0x40 };
static uint32_t gl_alloc(uint32_t flags, uint32_t n) {
    return w32_heap_alloc(process_heap, (flags & GMEM_ZEROINIT) ? W32_HEAP_ZERO_MEMORY : 0, n ? n : 1);
}
W32_IMPORT(kernel32, GlobalAlloc) {
    uint32_t r = gl_alloc(SG_ARG(0), SG_ARG(1));
    set_heap_err(ctx, mem, r);
    SG_RETURN_STDCALL(r, 8);
}
W32_IMPORT(kernel32, LocalAlloc) {
    uint32_t r = gl_alloc(SG_ARG(0), SG_ARG(1));
    set_heap_err(ctx, mem, r);
    SG_RETURN_STDCALL(r, 8);
}
W32_IMPORT(kernel32, GlobalFree) {
    uint32_t p = SG_ARG(0);
    if (!p || w32_heap_free(process_heap, 0, p)) SG_RETURN_STDCALL(0, 4);
    hle_set_last_error(ctx, mem, W32_ERROR_INVALID_HANDLE);
    SG_RETURN_STDCALL(p, 4);
}
W32_IMPORT(kernel32, LocalFree) {
    uint32_t p = SG_ARG(0);
    if (!p || w32_heap_free(process_heap, 0, p)) SG_RETURN_STDCALL(0, 4);
    hle_set_last_error(ctx, mem, W32_ERROR_INVALID_HANDLE);
    SG_RETURN_STDCALL(p, 4);
}
W32_IMPORT(kernel32, GlobalLock) {
    uint32_t p = SG_ARG(0);
    if (!w32_heap_validate_ptr(process_heap, p)) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_HANDLE); p = 0; }
    SG_RETURN_STDCALL(p, 4);
}
W32_IMPORT(kernel32, GlobalUnlock) {
    hle_set_last_error(ctx, mem, 0); /* "unlocked": returns 0 with NO_ERROR */
    SG_RETURN_STDCALL(0, 4);
}
W32_IMPORT(kernel32, GlobalSize) {
    uint32_t s = w32_heap_size(process_heap, 0, SG_ARG(0));
    if (s == 0xffffffffu) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_HANDLE); s = 0; }
    SG_RETURN_STDCALL(s, 4);
}

/* ---- memory status ---------------------------------------------------------------------- */

static uint64_t host_memsize(void) {
    uint64_t v = 0;
    size_t n = sizeof v;
    if (sysctlbyname("hw.memsize", &v, &n, NULL, 0)) v = 8ull << 30;
    return v;
}
static uint32_t vm_free_bytes(void) {
    w32_vm_stats s;
    w32_vm_get_stats(&s);
    return (W32_VM_HI - W32_VM_LO) - s.reserved_bytes;
}

W32_IMPORT(kernel32, GlobalMemoryStatusEx) {
    uint32_t p = SG_ARG(0);
    uint64_t phys = host_memsize();
    if (!p || sg_ld32(mem, p) != 64) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_PARAMETER); SG_RETURN_STDCALL(0, 4); }
    sg_st32(mem, p + 4, 25);                 /* dwMemoryLoad, percent */
    sg_st64(mem, p + 8, phys);               /* ullTotalPhys */
    sg_st64(mem, p + 16, phys / 4 * 3);      /* ullAvailPhys */
    sg_st64(mem, p + 24, phys * 2);          /* ullTotalPageFile */
    sg_st64(mem, p + 32, phys * 2 - phys / 4);
    sg_st64(mem, p + 40, 0x7ffe0000u);       /* ullTotalVirtual: 2 GiB minus the reserved tops */
    sg_st64(mem, p + 48, vm_free_bytes());   /* ullAvailVirtual */
    sg_st64(mem, p + 56, 0);
    SG_RETURN_STDCALL(1, 4);
}

W32_IMPORT(kernel32, GlobalMemoryStatus) {
    uint32_t p = SG_ARG(0);
    uint64_t phys = host_memsize();
    uint32_t cap = 0x7fffffffu; /* values above 2 GiB are clamped for non-LARGE_ADDRESS_AWARE programs */
#define CL(v) ((v) > cap ? cap : (uint32_t)(v))
    sg_st32(mem, p, 32);
    sg_st32(mem, p + 4, 25);
    sg_st32(mem, p + 8, CL(phys));
    sg_st32(mem, p + 12, CL(phys / 4 * 3));
    sg_st32(mem, p + 16, CL(phys * 2));
    sg_st32(mem, p + 20, CL(phys * 2 - phys / 4));
    sg_st32(mem, p + 24, 0x7ffe0000u);
    sg_st32(mem, p + 28, vm_free_bytes());
#undef CL
    SG_RET(4);
}

/* ---- shims: msvcr90 ------------------------------------------------------------------- */

static void set_enomem(uint8_t *mem, uint32_t r) { if (!r) hle_set_errno(mem, MSVC_ENOMEM); }

W32_IMPORT(msvcr90, malloc) { uint32_t r = w32_malloc(SG_ARG(0)); set_enomem(mem, r); SG_RETURN_CDECL(r); }
W32_IMPORT(msvcr90, free) { w32_free(SG_ARG(0)); SG_RET(0); }
W32_IMPORT(msvcr90, realloc) {
    uint32_t n = SG_ARG(1), r = w32_realloc(SG_ARG(0), n);
    if (n) set_enomem(mem, r);
    SG_RETURN_CDECL(r);
}
W32_IMPORT(msvcr90, calloc) { uint32_t r = w32_calloc(SG_ARG(0), SG_ARG(1)); set_enomem(mem, r); SG_RETURN_CDECL(r); }
W32_IMPORT(msvcr90, _msize) { SG_RETURN_CDECL(w32_msize(SG_ARG(0))); }
/* operator new / delete (not imported by this exe, which links its own; kept for completeness) */
W32_IMPORT(msvcr90, new) { uint32_t r = w32_malloc(SG_ARG(0)); SG_RETURN_CDECL(r); }
W32_IMPORT(msvcr90, delete) { w32_free(SG_ARG(0)); SG_RET(0); }

/* _aligned_malloc: msvcr90 stores the original pointer just below the aligned block. */
static uint32_t aligned_alloc32(uint32_t size, uint32_t align, uint32_t offset) {
    if (align & (align - 1)) return 0;
    if (align < 4) align = 4;
    uint32_t raw = w32_malloc(size + align + 4 + offset);
    if (!raw) return 0;
    uint32_t a = ((raw + 4 + offset + align - 1) & ~(align - 1)) - offset;
    sg_st32(w32_mem, (a & ~3u) - 4, raw);
    return a;
}
W32_IMPORT(msvcr90, _aligned_malloc) {
    uint32_t align = SG_ARG(1);
    if (align & (align - 1)) { hle_set_errno(mem, MSVC_EINVAL); SG_RETURN_CDECL(0); }
    uint32_t r = aligned_alloc32(SG_ARG(0), align, 0);
    set_enomem(mem, r);
    SG_RETURN_CDECL(r);
}
W32_IMPORT(msvcr90, _aligned_free) {
    uint32_t p = SG_ARG(0);
    if (p) w32_free(sg_ld32(mem, (p & ~3u) - 4));
    SG_RET(0);
}
W32_IMPORT(msvcr90, _aligned_realloc) {
    uint32_t p = SG_ARG(0), size = SG_ARG(1), align = SG_ARG(2);
    if (!p) { uint32_t r = aligned_alloc32(size, align, 0); set_enomem(mem, r); SG_RETURN_CDECL(r); }
    uint32_t raw = sg_ld32(mem, (p & ~3u) - 4);
    if (!size) { w32_free(raw); SG_RETURN_CDECL(0); }
    uint32_t old = w32_msize(raw) - (p - raw);
    uint32_t r = aligned_alloc32(size, align, 0);
    if (!r) { hle_set_errno(mem, MSVC_ENOMEM); SG_RETURN_CDECL(0); }
    memcpy(mem + r, mem + p, old < size ? old : size);
    w32_free(raw);
    SG_RETURN_CDECL(r);
}
