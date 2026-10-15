/* w32_vm.c -- VirtualAlloc / VirtualFree / VirtualProtect / VirtualQuery over the guest window.
 *
 * Bookkeeping is per 4 KiB guest page (Windows x86 page size) and per 64 KiB allocation
 * granule. The host page is 16 KiB on arm64 macOS, so host protection is the union of the
 * protections of the committed guest pages it holds: a guest page decommitted next to a
 * committed one stays host-accessible (more permissive than Windows; only diagnostics differ),
 * but its contents are zeroed again when it is recommitted, which Windows guarantees.
 * A host page whose guest pages are all decommitted is replaced by a fresh PROT_NONE mapping,
 * which releases its memory and makes it read as zeros on the next commit.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "w32_int.h"

#define NPAGES (SG_USER_LIMIT / W32_PAGE)      /* 524,288 guest pages below 2 GiB */
#define NUNITS (SG_USER_LIMIT / W32_GRANULE)   /* 32,768 granules */

enum { ST_FREE = 0, ST_RESERVE = 1, ST_COMMIT = 2 };
enum { TY_NONE = 0, TY_PRIVATE = 1, TY_IMAGE = 2, TY_RUNTIME = 3 };

typedef struct vm_page { uint16_t prot; uint8_t state, type; } vm_page;

static vm_page pages[NPAGES];
static uint32_t unit_base[NUNITS];   /* allocation base of the allocation covering the unit, or 0 */
static uint32_t alloc_size[NUNITS];  /* at the base unit: reserved size in bytes (page multiple) */
static uint32_t alloc_prot[NUNITS];  /* at the base unit: AllocationProtect */
static uint8_t *host_prot;            /* per host page: current PROT_* (0xff = unknown) */
static uint32_t host_page = 16384;
static pthread_mutex_t vm_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t n_allocs;
static uint32_t scan_hint_lo = W32_VM_LO / W32_GRANULE;

static int prot_valid(uint32_t p) {
    uint32_t base = p & 0xffu;
    if (p & ~0x7ffu) return 0;
    return base == 0x01 || base == 0x02 || base == 0x04 || base == 0x08 || base == 0x10 || base == 0x20 ||
           base == 0x40 || base == 0x80;
}

/* Host protection for one guest protection value. Code is never executed from the window. */
static int host_of(uint32_t p) {
    if (p & W32_PAGE_GUARD) return PROT_NONE; /* guard pages are not emulated: first touch faults */
    switch (p & 0xffu) {
    case W32_PAGE_NOACCESS: return PROT_NONE;
    case W32_PAGE_READONLY: case W32_PAGE_EXECUTE: case W32_PAGE_EXECUTE_READ: return PROT_READ;
    default: return PROT_READ | PROT_WRITE;
    }
}

/* Recomputes and applies host protection for host pages covering guest pages [p0, p1). */
static void sync_host(uint32_t p0, uint32_t p1, int fresh_if_empty) {
    uint32_t per = host_page / W32_PAGE;
    uint32_t h0 = p0 / per, h1 = (p1 + per - 1) / per;
    for (uint32_t h = h0; h < h1; h++) {
        int want = PROT_NONE, any_commit = 0, image = 0;
        for (uint32_t q = h * per; q < (h + 1) * per && q < NPAGES; q++) {
            if (pages[q].type == TY_IMAGE) image = 1;
            if (pages[q].state == ST_COMMIT) { any_commit = 1; want |= host_of(pages[q].prot); }
        }
        if (image && host_prot[h] == 0xff) continue; /* loader-owned and never touched by us */
        if (!any_commit && fresh_if_empty && !image) {
            void *r = mmap(w32_mem + (uint64_t)h * host_page, host_page, PROT_NONE,
                           MAP_FIXED | MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
            if (r == MAP_FAILED) w32_fatal("vm: mmap(MAP_FIXED) failed: %s", strerror(errno));
            host_prot[h] = PROT_NONE;
            continue;
        }
        if (host_prot[h] != (uint8_t)want) {
            if (mprotect(w32_mem + (uint64_t)h * host_page, host_page, want))
                w32_fatal("vm: mprotect(0x%08x) failed: %s", h * host_page, strerror(errno));
            host_prot[h] = (uint8_t)want;
        }
    }
}

/* Zeroes guest pages that become committed while their host page was already accessible
 * (stale contents from an earlier decommit). Fresh host pages read as zero already. */
static void zero_new_commits(uint32_t p0, uint32_t p1, const uint8_t *was_committed) {
    uint32_t per = host_page / W32_PAGE;
    for (uint32_t q = p0; q < p1; q++) {
        if (was_committed[q - p0]) continue;
        if (host_prot[q / per] == PROT_NONE || host_prot[q / per] == 0xff) continue;
        if (!(host_prot[q / per] & PROT_WRITE)) {
            /* make it writable for the memset; sync_host fixes the protection afterwards */
            mprotect(w32_mem + (uint64_t)(q / per) * host_page, host_page, PROT_READ | PROT_WRITE);
            host_prot[q / per] = PROT_READ | PROT_WRITE;
        }
        memset(w32_mem + (uint64_t)q * W32_PAGE, 0, W32_PAGE);
    }
}

void w32_vm_init(void) {
    long hp = sysconf(_SC_PAGESIZE);
    host_page = hp > 0 ? (uint32_t)hp : 16384u;
    if (host_page < W32_PAGE || host_page % W32_PAGE) w32_fatal("vm: unsupported host page size %u", host_page);
    host_prot = (uint8_t *)malloc(SG_USER_LIMIT / host_page);
    if (!host_prot) w32_fatal("vm: out of host memory");
    memset(host_prot, PROT_NONE, SG_USER_LIMIT / host_page);
    memset(pages, 0, sizeof pages);
    memset(unit_base, 0, sizeof unit_base);
    n_allocs = 0;
    /* The image: registered so VirtualAlloc never hands it out; the loader maps it. */
    w32_vm_mark_image(SG_IMAGE_BASE, SG_IMAGE_SIZE, W32_PAGE_READWRITE);
    for (uint32_t h = SG_IMAGE_BASE / host_page; h < (SG_IMAGE_BASE + SG_IMAGE_SIZE + host_page - 1) / host_page; h++)
        host_prot[h] = 0xff;
    /* Runtime regions (thunks, TEBs, PEB): reserved, handed out only by w32_vm_alloc_runtime. */
    for (uint32_t p = SG_THUNK_BASE / W32_PAGE; p < NPAGES; p++) {
        pages[p].state = ST_RESERVE;
        pages[p].type = TY_RUNTIME;
    }
    for (uint32_t u = SG_THUNK_BASE / W32_GRANULE; u < NUNITS; u++) unit_base[u] = SG_THUNK_BASE;
    alloc_size[SG_THUNK_BASE / W32_GRANULE] = SG_USER_LIMIT - SG_THUNK_BASE;
    alloc_prot[SG_THUNK_BASE / W32_GRANULE] = W32_PAGE_READWRITE;
}

void w32_vm_mark_image(uint32_t addr, uint32_t size, uint32_t protect) {
    uint32_t p0 = addr / W32_PAGE, p1 = (uint32_t)(((uint64_t)addr + size + W32_PAGE - 1) / W32_PAGE);
    pthread_mutex_lock(&vm_lock);
    for (uint32_t p = p0; p < p1 && p < NPAGES; p++) {
        pages[p].state = ST_COMMIT;
        pages[p].type = TY_IMAGE;
        pages[p].prot = (uint16_t)protect;
    }
    for (uint32_t u = SG_IMAGE_BASE / W32_GRANULE; u <= (SG_IMAGE_BASE + SG_IMAGE_SIZE - 1) / W32_GRANULE; u++)
        unit_base[u] = SG_IMAGE_BASE;
    alloc_size[SG_IMAGE_BASE / W32_GRANULE] = SG_IMAGE_SIZE;
    alloc_prot[SG_IMAGE_BASE / W32_GRANULE] = W32_PAGE_EXECUTE_WRITECOPY;
    pthread_mutex_unlock(&vm_lock);
}

/* First fit over granules. Returns the unit index or -1. */
static int64_t find_free_units(uint32_t nunits, int top_down) {
    uint32_t lo = W32_VM_LO / W32_GRANULE, hi = W32_VM_HI / W32_GRANULE;
    if (!top_down) {
        uint32_t run = 0;
        for (uint32_t u = scan_hint_lo > lo ? scan_hint_lo : lo; u < hi; u++) {
            run = unit_base[u] ? 0 : run + 1;
            if (run == nunits) return (int64_t)(u + 1 - nunits);
        }
        if (scan_hint_lo > lo) { /* the hint can skip holes freed later: rescan from the bottom */
            run = 0;
            for (uint32_t u = lo; u < hi; u++) {
                run = unit_base[u] ? 0 : run + 1;
                if (run == nunits) return (int64_t)(u + 1 - nunits);
            }
        }
    } else {
        uint32_t run = 0;
        for (uint32_t u = hi; u-- > lo;) {
            run = unit_base[u] ? 0 : run + 1;
            if (run == nunits) return (int64_t)u;
        }
    }
    return -1;
}

static void set_err(uint32_t *err, uint32_t e) { if (err) *err = e; }

static uint32_t do_reserve(uint32_t addr, uint32_t size, uint32_t type, uint32_t protect, uint32_t *err, int runtime) {
    uint32_t base, end, nunits;
    if (addr) {
        base = addr & ~(W32_GRANULE - 1u);
        uint64_t e = ((uint64_t)addr + size + W32_PAGE - 1) & ~(uint64_t)(W32_PAGE - 1);
        uint32_t hi = runtime ? SG_USER_LIMIT : W32_VM_HI;
        if (base < W32_VM_LO || e > hi) { set_err(err, W32_ERROR_INVALID_ADDRESS); return 0; }
        end = (uint32_t)e;
        if (runtime && unit_base[base / W32_GRANULE] == SG_THUNK_BASE) {
            /* inside the runtime region: the requested pages must not be committed yet */
            for (uint32_t p = addr / W32_PAGE; p < end / W32_PAGE; p++)
                if (pages[p].state == ST_COMMIT) { set_err(err, W32_ERROR_INVALID_ADDRESS); return 0; }
        } else {
            for (uint32_t u = base / W32_GRANULE; u <= (end - 1) / W32_GRANULE; u++)
                if (unit_base[u]) { set_err(err, W32_ERROR_INVALID_ADDRESS); return 0; }
        }
    } else {
        nunits = (uint32_t)(((uint64_t)size + W32_GRANULE - 1) / W32_GRANULE);
        int64_t u = find_free_units(nunits, (type & W32_MEM_TOP_DOWN) != 0);
        if (u < 0) { set_err(err, W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
        base = (uint32_t)u * W32_GRANULE;
        end = base + (uint32_t)(((uint64_t)size + W32_PAGE - 1) & ~(uint64_t)(W32_PAGE - 1));
        if (!(type & W32_MEM_TOP_DOWN) && (uint32_t)u == scan_hint_lo) scan_hint_lo = (uint32_t)u + nunits;
    }
    int in_runtime = runtime && unit_base[base / W32_GRANULE] == SG_THUNK_BASE;
    if (!in_runtime) {
        for (uint32_t u = base / W32_GRANULE; u <= (end - 1) / W32_GRANULE; u++) unit_base[u] = base;
        alloc_size[base / W32_GRANULE] = end - base;
        alloc_prot[base / W32_GRANULE] = protect;
        for (uint32_t p = base / W32_PAGE; p < end / W32_PAGE; p++) {
            pages[p].state = ST_RESERVE;
            pages[p].type = TY_PRIVATE;
            pages[p].prot = 0;
        }
        n_allocs++;
    }
    return base;
}

static int commit_range(uint32_t p0, uint32_t p1, uint32_t protect) {
    uint8_t stackbuf[256], *was = p1 - p0 <= sizeof stackbuf ? stackbuf : (uint8_t *)malloc(p1 - p0);
    if (!was) return 0;
    for (uint32_t p = p0; p < p1; p++) was[p - p0] = pages[p].state == ST_COMMIT;
    zero_new_commits(p0, p1, was);
    for (uint32_t p = p0; p < p1; p++) { pages[p].state = ST_COMMIT; pages[p].prot = (uint16_t)protect; }
    if (was != stackbuf) free(was);
    sync_host(p0, p1, 0);
    return 1;
}

uint32_t w32_vm_alloc(uint32_t addr, uint32_t size, uint32_t type, uint32_t protect, uint32_t *err) {
    uint32_t r = 0;
    set_err(err, 0);
    if (!size || !(type & (W32_MEM_COMMIT | W32_MEM_RESERVE | W32_MEM_RESET))) {
        set_err(err, W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (type & W32_MEM_RESET) return addr & ~(W32_PAGE - 1u); /* contents may be discarded: keep them */
    if (!prot_valid(protect)) { set_err(err, W32_ERROR_INVALID_PARAMETER); return 0; }
    if ((uint64_t)addr + size > SG_USER_LIMIT) { set_err(err, W32_ERROR_INVALID_ADDRESS); return 0; }
    pthread_mutex_lock(&vm_lock);
    if ((type & W32_MEM_RESERVE) || !addr) {
        r = do_reserve(addr, size, type, protect, err, 0);
        if (r && (type & W32_MEM_COMMIT)) {
            uint32_t start = addr ? addr & ~(W32_PAGE - 1u) : r;
            uint32_t end = (uint32_t)(((uint64_t)(addr ? addr : r) + size + W32_PAGE - 1) & ~(uint64_t)(W32_PAGE - 1));
            commit_range(start / W32_PAGE, end / W32_PAGE, protect);
            if (addr) r = addr & ~(W32_GRANULE - 1u);
        }
    } else {
        /* commit inside an existing reservation */
        uint32_t start = addr & ~(W32_PAGE - 1u);
        uint32_t end = (uint32_t)(((uint64_t)addr + size + W32_PAGE - 1) & ~(uint64_t)(W32_PAGE - 1));
        uint32_t base = unit_base[start / W32_GRANULE];
        if (!base || base == SG_IMAGE_BASE || base == SG_THUNK_BASE || end > base + alloc_size[base / W32_GRANULE]) {
            set_err(err, W32_ERROR_INVALID_ADDRESS);
        } else {
            commit_range(start / W32_PAGE, end / W32_PAGE, protect);
            r = start;
        }
    }
    pthread_mutex_unlock(&vm_lock);
    return r;
}

uint32_t w32_vm_alloc_runtime(uint32_t addr, uint32_t size, uint32_t protect) {
    uint32_t err = 0, r, end;
    if (!size || !prot_valid(protect) || (uint64_t)addr + size > SG_USER_LIMIT) return 0;
    pthread_mutex_lock(&vm_lock);
    r = do_reserve(addr, size, W32_MEM_RESERVE, protect, &err, 1);
    if (r) {
        uint32_t start = addr ? addr & ~(W32_PAGE - 1u) : r;
        end = (uint32_t)(((uint64_t)(addr ? addr : r) + size + W32_PAGE - 1) & ~(uint64_t)(W32_PAGE - 1));
        commit_range(start / W32_PAGE, end / W32_PAGE, protect);
        r = start;
    }
    pthread_mutex_unlock(&vm_lock);
    return r;
}

int w32_vm_free(uint32_t addr, uint32_t size, uint32_t type, uint32_t *err) {
    int ok = 0;
    set_err(err, 0);
    if (type != W32_MEM_RELEASE && type != W32_MEM_DECOMMIT) { set_err(err, W32_ERROR_INVALID_PARAMETER); return 0; }
    if (addr >= W32_VM_HI) { set_err(err, W32_ERROR_INVALID_ADDRESS); return 0; }
    pthread_mutex_lock(&vm_lock);
    uint32_t base = unit_base[addr / W32_GRANULE];
    if (!base || base == SG_IMAGE_BASE) {
        set_err(err, W32_ERROR_INVALID_ADDRESS);
    } else if (type == W32_MEM_RELEASE) {
        if (size || addr != base) {
            set_err(err, W32_ERROR_INVALID_PARAMETER);
        } else {
            uint32_t end = base + alloc_size[base / W32_GRANULE];
            for (uint32_t p = base / W32_PAGE; p < end / W32_PAGE; p++) pages[p] = (vm_page){0, ST_FREE, TY_NONE};
            for (uint32_t u = base / W32_GRANULE; u <= (end - 1) / W32_GRANULE; u++) unit_base[u] = 0;
            if (base / W32_GRANULE < scan_hint_lo) scan_hint_lo = base / W32_GRANULE;
            n_allocs--;
            sync_host(base / W32_PAGE, end / W32_PAGE, 1);
            ok = 1;
        }
    } else {
        uint32_t start = addr & ~(W32_PAGE - 1u), end;
        if (!size) {
            if (addr != base) { set_err(err, W32_ERROR_INVALID_PARAMETER); goto out; }
            end = base + alloc_size[base / W32_GRANULE];
        } else {
            end = (uint32_t)(((uint64_t)addr + size + W32_PAGE - 1) & ~(uint64_t)(W32_PAGE - 1));
        }
        if (end > base + alloc_size[base / W32_GRANULE]) { set_err(err, W32_ERROR_INVALID_ADDRESS); goto out; }
        for (uint32_t p = start / W32_PAGE; p < end / W32_PAGE; p++) { pages[p].state = ST_RESERVE; pages[p].prot = 0; }
        sync_host(start / W32_PAGE, end / W32_PAGE, 1);
        ok = 1;
    }
out:
    pthread_mutex_unlock(&vm_lock);
    return ok;
}

int w32_vm_protect(uint32_t addr, uint32_t size, uint32_t protect, uint32_t *old, uint32_t *err) {
    int ok = 0;
    set_err(err, 0);
    if (!size || !prot_valid(protect)) { set_err(err, W32_ERROR_INVALID_PARAMETER); return 0; }
    if ((uint64_t)addr + size > SG_USER_LIMIT) { set_err(err, W32_ERROR_INVALID_ADDRESS); return 0; }
    pthread_mutex_lock(&vm_lock);
    uint32_t start = addr & ~(W32_PAGE - 1u);
    uint32_t end = (uint32_t)(((uint64_t)addr + size + W32_PAGE - 1) & ~(uint64_t)(W32_PAGE - 1));
    uint32_t base = unit_base[start / W32_GRANULE];
    for (uint32_t p = start / W32_PAGE; p < end / W32_PAGE; p++)
        if (pages[p].state != ST_COMMIT || unit_base[p / (W32_GRANULE / W32_PAGE)] != base) {
            set_err(err, W32_ERROR_INVALID_ADDRESS);
            goto out;
        }
    if (old) *old = pages[start / W32_PAGE].prot;
    uint32_t per = host_page / W32_PAGE;
    for (uint32_t p = start / W32_PAGE; p < end / W32_PAGE; p++) {
        pages[p].prot = (uint16_t)protect;
        if (pages[p].type == TY_IMAGE && host_prot[p / per] == 0xff) host_prot[p / per] = 0xfe; /* now ours */
    }
    sync_host(start / W32_PAGE, end / W32_PAGE, 0);
    ok = 1;
out:
    pthread_mutex_unlock(&vm_lock);
    return ok;
}

static uint32_t type_bits(uint8_t t) {
    return t == TY_IMAGE ? W32_MEM_IMAGE : t ? W32_MEM_PRIVATE : 0;
}

int w32_vm_query(uint32_t addr, w32_mbi *o) {
    if (addr >= SG_USER_LIMIT - W32_GRANULE) return 0; /* above the highest user address */
    pthread_mutex_lock(&vm_lock);
    uint32_t p = addr / W32_PAGE, q = p;
    uint32_t base = unit_base[addr / W32_GRANULE];
    vm_page pg = pages[p];
    memset(o, 0, sizeof *o);
    o->base = p * W32_PAGE;
    if (pg.state == ST_FREE) {
        while (q < NPAGES && pages[q].state == ST_FREE && !unit_base[q / (W32_GRANULE / W32_PAGE)]) q++;
        if (q == p) q = p + 1; /* the tail of a granule after a short reservation: free but unusable */
        o->state = W32_MEM_FREE;
        o->protect = W32_PAGE_NOACCESS;
    } else {
        uint32_t end = base + alloc_size[base / W32_GRANULE];
        while (q < end / W32_PAGE && pages[q].state == pg.state && pages[q].prot == pg.prot && pages[q].type == pg.type) q++;
        o->alloc_base = base;
        o->alloc_protect = alloc_prot[base / W32_GRANULE];
        o->state = pg.state == ST_COMMIT ? W32_MEM_COMMIT : W32_MEM_RESERVE;
        o->protect = pg.state == ST_COMMIT ? pg.prot : 0;
        o->type = type_bits(pg.type);
    }
    o->region_size = (q - p) * W32_PAGE;
    pthread_mutex_unlock(&vm_lock);
    return 1;
}

/* Lock-free test used on hot paths (heap frees): racy reads of single bytes are benign here. */
int w32_vm_is_committed(uint32_t addr, uint32_t len) {
    if ((uint64_t)addr + len > SG_USER_LIMIT || !len) return 0;
    for (uint32_t p = addr / W32_PAGE; p <= (addr + len - 1) / W32_PAGE; p++)
        if (__atomic_load_n(&pages[p].state, __ATOMIC_RELAXED) != ST_COMMIT) return 0;
    return 1;
}

void w32_vm_get_stats(w32_vm_stats *s) {
    uint32_t r = 0, c = 0;
    pthread_mutex_lock(&vm_lock);
    for (uint32_t p = W32_VM_LO / W32_PAGE; p < W32_VM_HI / W32_PAGE; p++) {
        if (pages[p].type != TY_PRIVATE) continue;
        if (pages[p].state == ST_RESERVE) r++;
        else if (pages[p].state == ST_COMMIT) c++;
    }
    s->reserved_bytes = (r + c) * W32_PAGE;
    s->committed_bytes = c * W32_PAGE;
    s->allocations = n_allocs;
    pthread_mutex_unlock(&vm_lock);
}

/* ---- shims ------------------------------------------------------------------------------- */

W32_IMPORT(kernel32, VirtualAlloc) {
    uint32_t err = 0, r = w32_vm_alloc(SG_ARG(0), SG_ARG(1), SG_ARG(2), SG_ARG(3), &err);
    if (!r) hle_set_last_error(ctx, mem, err);
    SG_RETURN_STDCALL(r, 16);
}

W32_IMPORT(kernel32, VirtualFree) {
    uint32_t err = 0;
    int ok = w32_vm_free(SG_ARG(0), SG_ARG(1), SG_ARG(2), &err);
    if (!ok) hle_set_last_error(ctx, mem, err);
    SG_RETURN_STDCALL(ok, 12);
}

W32_IMPORT(kernel32, VirtualQuery) {
    uint32_t a = SG_ARG(0), out = SG_ARG(1), len = SG_ARG(2);
    w32_mbi m;
    if (len < sizeof m) { hle_set_last_error(ctx, mem, W32_ERROR_BAD_LENGTH); SG_RETURN_STDCALL(0, 12); }
    if (!w32_vm_query(a, &m)) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_PARAMETER); SG_RETURN_STDCALL(0, 12); }
    memcpy(mem + out, &m, sizeof m);
    SG_RETURN_STDCALL(sizeof m, 12);
}

W32_IMPORT(kernel32, VirtualProtect) {
    uint32_t err = 0, old = 0, oldp = SG_ARG(3);
    int ok;
    if (!oldp) { hle_set_last_error(ctx, mem, W32_ERROR_NOACCESS); SG_RETURN_STDCALL(0, 16); }
    ok = w32_vm_protect(SG_ARG(0), SG_ARG(1), SG_ARG(2), &old, &err);
    if (ok) sg_st32(mem, oldp, old);
    else hle_set_last_error(ctx, mem, err);
    SG_RETURN_STDCALL(ok, 16);
}
