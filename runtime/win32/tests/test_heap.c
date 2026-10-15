/* test_heap.c -- heaps, the malloc family, GlobalAlloc, hle_guest_alloc; a randomised check
 * against a shadow model, a multithreaded stress run and a small benchmark (--bench). */
#include "harness.h"
#include "w32_int.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

static uint32_t pick_size(void) {
    uint32_t k = rnd() % 100;
    if (k < 60) return rnd() % 128;
    if (k < 90) return rnd() % 4096;
    if (k < 98) return rnd() % 40000;
    return rnd() % 300000;
}

static void fill(uint32_t p, uint32_t n, uint8_t tag) { for (uint32_t i = 0; i < n; i++) M[p + i] = (uint8_t)(tag + i * 7); }
static int verify(uint32_t p, uint32_t n, uint8_t tag) {
    for (uint32_t i = 0; i < n; i++) if (M[p + i] != (uint8_t)(tag + i * 7)) return 0;
    return 1;
}

typedef struct { uint32_t p, n; uint8_t tag; } live_t;

static void randomized(uint32_t heap, int ops) {
    enum { N = 2000 };
    static live_t live[N];
    memset(live, 0, sizeof live);
    int bad = 0, overlaps = 0;
    for (int op = 0; op < ops; op++) {
        int i = (int)(rnd() % N);
        uint32_t action = rnd() % 3;
        if (!live[i].p) {
            uint32_t n = pick_size();
            uint32_t p = w32_heap_alloc(heap, (rnd() & 1) ? W32_HEAP_ZERO_MEMORY : 0, n);
            if (!p) { bad++; continue; }
            if (p & 15) bad++;
            if (w32_heap_size(heap, 0, p) != n) bad++;
            live[i] = (live_t){p, n, (uint8_t)rnd()};
            fill(p, n, live[i].tag);
        } else if (action == 0) {
            uint32_t n = pick_size();
            uint32_t np = w32_heap_realloc(heap, 0, live[i].p, n);
            if (!np) { bad++; continue; }
            uint32_t keep = n < live[i].n ? n : live[i].n;
            if (!verify(np, keep, live[i].tag)) bad++;
            live[i].p = np;
            live[i].n = n;
            fill(np, n, live[i].tag);
        } else {
            if (!verify(live[i].p, live[i].n, live[i].tag)) bad++;
            if (!w32_heap_free(heap, 0, live[i].p)) bad++;
            live[i].p = 0;
        }
    }
    /* no two live blocks overlap, and all still hold their pattern */
    for (int i = 0; i < N; i++) {
        if (!live[i].p) continue;
        if (!verify(live[i].p, live[i].n, live[i].tag)) bad++;
        for (int j = i + 1; j < N && j < i + 64; j++)
            if (live[j].p && live[i].p < live[j].p + live[j].n && live[j].p < live[i].p + live[i].n && live[i].n && live[j].n)
                overlaps++;
        w32_heap_free(heap, 0, live[i].p);
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(overlaps, 0);
}

typedef struct { uint32_t heap; int ops; int bad; unsigned seed; } thr_arg;
static void *stress(void *v) {
    thr_arg *a = (thr_arg *)v;
    uint32_t slots[256] = {0}, sizes[256] = {0};
    unsigned s = a->seed;
    for (int op = 0; op < a->ops; op++) {
        s = s * 1103515245u + 12345u;
        unsigned i = (s >> 8) & 255;
        if (slots[i]) {
            if (M[slots[i]] != (uint8_t)i || w32_heap_size(a->heap, 0, slots[i]) != sizes[i]) a->bad++;
            w32_heap_free(a->heap, 0, slots[i]);
            slots[i] = 0;
        } else {
            uint32_t n = 1 + ((s >> 16) % 2000);
            slots[i] = w32_heap_alloc(a->heap, 0, n);
            if (!slots[i]) { a->bad++; continue; }
            sizes[i] = n;
            M[slots[i]] = (uint8_t)i;
        }
    }
    for (int i = 0; i < 256; i++) if (slots[i]) w32_heap_free(a->heap, 0, slots[i]);
    return NULL;
}

static uint32_t cb_noop(sg_ctx *ctx, unsigned n, const uint32_t *a) { (void)ctx; (void)n; (void)a; return 0; }

void test_heap(void) {
    uint32_t ph = w32_process_heap(), crt = w32_crt_heap();
    CHECK(ph && crt && ph != crt);
    CHECK_EQ(CALL0(w32_imp_kernel32_GetProcessHeap, 0), ph);

    /* basic */
    uint32_t a = w32_heap_alloc(ph, 0, 10), b = w32_heap_alloc(ph, W32_HEAP_ZERO_MEMORY, 100);
    CHECK(a && b && a != b && !(a & 15) && !(b & 15));
    CHECK_EQ(w32_heap_size(ph, 0, a), 10);
    CHECK_EQ(w32_heap_size(ph, 0, b), 100);
    int z = 1;
    for (int i = 0; i < 100; i++) z &= M[b + i] == 0;
    CHECK(z);
    /* free + reuse of the same class (LIFO) */
    CHECK(w32_heap_free(ph, 0, a));
    CHECK_EQ(w32_heap_alloc(ph, 0, 12), a);
    /* invalid and double frees are refused, not corrupting */
    CHECK(!w32_heap_free(ph, 0, a + 16));
    CHECK(w32_heap_free(ph, 0, a));
    CHECK(!w32_heap_free(ph, 0, a));
    CHECK(w32_heap_free(ph, 0, 0)); /* HeapFree(NULL) */
    CHECK(!w32_heap_free(crt, 0, b)); /* wrong heap */
    CHECK_EQ(w32_heap_size(ph, 0, a), 0xffffffffu);

    /* realloc: grow keeps data, in-place shrink, HEAP_REALLOC_IN_PLACE_ONLY, zero fill */
    uint32_t r = w32_heap_alloc(ph, 0, 40);
    fill(r, 40, 3);
    uint32_t r2 = w32_heap_realloc(ph, W32_HEAP_ZERO_MEMORY, r, 44); /* same class (48) */
    CHECK_EQ(r2, r);
    CHECK(verify(r2, 40, 3));
    CHECK(M[r2 + 40] == 0 && M[r2 + 43] == 0);
    CHECK_EQ(w32_heap_realloc(ph, W32_HEAP_REALLOC_IN_PLACE_ONLY, r2, 5000), 0);
    uint32_t r3 = w32_heap_realloc(ph, 0, r2, 5000);
    CHECK(r3 && r3 != r2);
    CHECK(verify(r3, 40, 3));
    CHECK(!w32_heap_validate_ptr(ph, r2));

    /* large blocks get their own region and are released on free */
    uint32_t big = w32_heap_alloc(ph, 0, 1 << 20);
    CHECK(big && ((big - 16) & 0xffff) == 0);
    w32_mbi mb;
    CHECK(w32_vm_query(big, &mb) && mb.state == W32_MEM_COMMIT);
    fill(big, 1 << 20, 9);
    uint32_t big2 = w32_heap_realloc(ph, 0, big, (1 << 20) + 100); /* fits in the last page */
    CHECK_EQ(big2, big);
    uint32_t big3 = w32_heap_realloc(ph, 0, big2, 3 << 20);
    CHECK(verify(big3, 1 << 20, 9));
    CHECK(w32_vm_query(big, &mb) && mb.state == W32_MEM_FREE);
    CHECK(w32_heap_free(ph, 0, big3));

    /* private heaps and HeapDestroy */
    uint32_t h = w32_heap_create(0, 0, 0);
    CHECK(h);
    uint32_t hb = w32_heap_alloc(h, 0, 64), hl = w32_heap_alloc(h, 0, 100000);
    CHECK(hb && hl);
    CHECK(w32_heap_destroy(h));
    CHECK(w32_vm_query(hl, &mb) && mb.state == W32_MEM_FREE);
    CHECK_EQ(w32_heap_alloc(h, 0, 8), 0);
    CHECK(!w32_heap_destroy(ph));

    /* msvcr90 rules */
    uint32_t m0 = w32_malloc(0);
    CHECK(m0 != 0);
    CHECK_EQ(w32_msize(m0), 1);
    CHECK_EQ(w32_realloc(m0, 0), 0); /* frees */
    uint32_t m1 = w32_realloc(0, 33);
    CHECK_EQ(w32_msize(m1), 33);
    CHECK_EQ(w32_malloc(0xfffffff0u), 0);
    uint32_t cz = w32_calloc(10, 10);
    CHECK_EQ(w32_msize(cz), 100);
    CHECK_EQ(w32_calloc(0x10000, 0x10000), 0);
    w32_free(m1);
    w32_free(cz);

    /* shims */
    uint32_t sm = CALL(w32_imp_msvcr90_malloc, 0, 50);
    CHECK(sm);
    CHECK_EQ(CALL(w32_imp_msvcr90__msize, 0, sm), 50);
    uint32_t sr = CALL(w32_imp_msvcr90_realloc, 0, sm, 5000);
    CHECK_EQ(CALL(w32_imp_msvcr90__msize, 0, sr), 5000);
    CALL(w32_imp_msvcr90_free, 0, sr);
    CHECK_EQ(CALL(w32_imp_msvcr90_malloc, 0, 0xffffffffu), 0);
    CHECK_EQ(hle_get_errno(M), MSVC_ENOMEM);
    uint32_t ha = CALL(w32_imp_kernel32_HeapAlloc, 12, ph, W32_HEAP_ZERO_MEMORY, 24);
    CHECK(ha);
    CHECK_EQ(CALL(w32_imp_kernel32_HeapFree, 12, ph, 0, ha), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_HeapFree, 12, ph, 0, ha), 0);
    uint32_t al = CALL(w32_imp_msvcr90__aligned_malloc, 0, 100, 64);
    CHECK(al && !(al & 63));
    fill(al, 100, 5);
    uint32_t al2 = CALL(w32_imp_msvcr90__aligned_realloc, 0, al, 300, 64);
    CHECK(al2 && !(al2 & 63) && verify(al2, 100, 5));
    CALL(w32_imp_msvcr90__aligned_free, 0, al2);
    CHECK_EQ(CALL(w32_imp_msvcr90__aligned_malloc, 0, 10, 3), 0);
    uint32_t g = CALL(w32_imp_kernel32_GlobalAlloc, 8, 0x42, 16); /* GMEM_MOVEABLE | GMEM_ZEROINIT */
    CHECK(g);
    CHECK_EQ(CALL(w32_imp_kernel32_GlobalLock, 4, g), g);
    CHECK_EQ(CALL(w32_imp_kernel32_GlobalSize, 4, g), 16);
    CHECK_EQ(CALL(w32_imp_kernel32_GlobalUnlock, 4, g), 0);
    CHECK_EQ(CALL(w32_imp_kernel32_GlobalFree, 4, g), 0);
    CHECK_EQ(CALL(w32_imp_kernel32_GlobalFree, 4, g), g); /* second free fails: returns the handle */
    uint32_t la = CALL(w32_imp_kernel32_LocalAlloc, 8, 0x40, 8);
    CHECK_EQ(CALL(w32_imp_kernel32_LocalFree, 4, la), 0);

    /* hle_guest_alloc: zeroed, aligned */
    uint32_t ga = hle_guest_alloc(100, 64);
    CHECK(ga && !(ga & 63) && M[ga] == 0 && M[ga + 99] == 0);

    /* memory status */
    uint32_t ms = gbuf(64);
    sg_st32(M, ms, 64);
    CHECK_EQ(CALL(w32_imp_kernel32_GlobalMemoryStatusEx, 4, ms), 1);
    CHECK(sg_ld64(M, ms + 8) > (1ull << 30));
    CHECK_EQ(sg_ld64(M, ms + 40), 0x7ffe0000u);
    sg_st32(M, ms, 60);
    CHECK_EQ(CALL(w32_imp_kernel32_GlobalMemoryStatusEx, 4, ms), 0);
    uint32_t ms2 = gbuf(32);
    CALL(w32_imp_kernel32_GlobalMemoryStatus, 4, ms2);
    CHECK_EQ(sg_ld32(M, ms2 + 8), 0x7fffffffu); /* clamped on a >2 GiB host */

    /* class boundaries */
    static const uint32_t sizes[] = {1, 15, 16, 17, 255, 256, 257, 320, 321, 1023, 1024, 1025, 16384, 16385, 32767, 32768, 32769};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        uint32_t p = w32_heap_alloc(ph, 0, sizes[i]);
        fill(p, sizes[i], (uint8_t)i);
        uint32_t q = w32_heap_alloc(ph, 0, sizes[i]);
        fill(q, sizes[i], (uint8_t)(i + 1));
        CHECK(verify(p, sizes[i], (uint8_t)i));
        CHECK(q >= p + sizes[i] || p >= q + sizes[i]);
        CHECK_EQ(w32_heap_size(ph, 0, p), sizes[i]);
        w32_heap_free(ph, 0, p);
        w32_heap_free(ph, 0, q);
    }

    /* randomised against a shadow model: 200,000 operations */
    randomized(ph, 200000);
    uint32_t h2 = w32_heap_create(0, 0, 0);
    randomized(h2, 50000);
    CHECK(w32_heap_destroy(h2));

    /* 8 threads on the process heap */
    pthread_t th[8];
    thr_arg args[8];
    for (int i = 0; i < 8; i++) {
        args[i] = (thr_arg){ph, 100000, 0, (unsigned)i * 7919u + 1};
        pthread_create(&th[i], NULL, stress, &args[i]);
    }
    int tbad = 0;
    for (int i = 0; i < 8; i++) { pthread_join(th[i], NULL); tbad += args[i].bad; }
    CHECK_EQ(tbad, 0);
    (void)cb_noop;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

void bench_heap(void) {
    uint32_t ph = w32_process_heap();
    enum { N = 4096 };
    static uint32_t slot[N];
    int iters = 4000000;
    double t0 = now();
    for (int i = 0; i < iters; i++) {
        unsigned k = rnd() & (N - 1);
        if (slot[k]) { w32_heap_free(ph, 0, slot[k]); slot[k] = 0; }
        else slot[k] = w32_heap_alloc(ph, 0, 8 + (rnd() & 255));
    }
    double t1 = now();
    printf("heap bench: %d alloc/free ops (8..263 bytes, 4096 live slots) in %.3f s = %.1f ns/op\n", iters, t1 - t0,
           (t1 - t0) * 1e9 / iters);
    for (int i = 0; i < 1000000; i++) {
        uint32_t p = w32_malloc(64);
        w32_free(p);
    }
    double t2 = now();
    printf("malloc(64)+free pairs: %.1f ns/pair\n", (t2 - t1) * 1e9 / 1000000);
    for (int i = 0; i < 1000; i++) {
        uint32_t p = w32_heap_alloc(ph, 0, 100000);
        w32_heap_free(ph, 0, p);
    }
    double t3 = now();
    printf("large (100 KB) alloc+free: %.1f us/pair\n", (t3 - t2) * 1e6 / 1000);
}
