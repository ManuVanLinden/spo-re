/* test_memorder.c -- the Dac hand-off of SporeApp.exe as a message-passing test
 * (docs/design/memory-ordering.md). No game code: two
 * host threads use the guest window exactly as translated code does.
 *
 * Writer (the main thread's pause/resume/stop functions, 0x113d780...): prepares a record with
 * plain stores (sg_st32, as the translator emits for an unlisted heap store), then stores the
 * state word (0x16e7b9c). Reader (the Dac loop, 0x113dc70): polls the state word, then reads the
 * record. Every field and the state word carry the same increasing sequence number, so a field
 * read after the state word that is smaller than the state seen is stale: the reader saw the new
 * state before the data that goes with it.
 *
 *   ordered  today's emission: the state store is sg_st32_rel_u (stlr), the poll sg_ld32_acq_u
 *            (ldapr); fields stay plain. Must never see a stale field.
 *   relaxed  the emission before the ordering policy: every access plain (sg_st32/sg_ld32). A compiler barrier
 *            sits between all accesses (the game's loop has opaque calls there), so whatever goes
 *            wrong is the CPU's reordering, not clang's. arm64 may reorder both the writer's
 *            stores and the reader's loads, and does on Apple Silicon.
 *
 * `core_tests memorder` runs the ordered case (asserted) and measures the relaxed one (reported,
 * not asserted, so the suite stays deterministic); `core_tests memorder-relaxed` asserts the
 * relaxed case too and is expected to FAIL on arm64: it reproduces the bug the ordering policy fixes. */
#include <time.h>

#include "core_internal.h"
#include "fixture.h"

#define MO_FIELDS 6
#define MO_LINE 128u /* Apple cores' cache line: keep the state word and every field apart */

typedef struct {
    uint32_t state, rec;    /* guest addresses */
    int ordered;
    volatile int stop;
    uint32_t rounds;        /* writer's last sequence number */
    unsigned long reads, stale, first_stale_seen, first_stale_field;
} mo_test;

#define CB() __asm__ volatile("" ::: "memory")

static uint32_t mo_field(const mo_test *t, int i) { return t->rec + MO_LINE * (uint32_t)i; }

static void *mo_writer(void *arg) {
    mo_test *t = arg;
    uint32_t n;
    for (n = 1; n <= t->rounds && !t->stop; n++) {
        int i;
        if (t->ordered) {
            for (i = 0; i < MO_FIELDS; i++) sg_st32(M, mo_field(t, i), n);   /* plain record */
            sg_st32_rel_u(M, t->state, n);                                    /* release state store */
        } else {
            for (i = 0; i < MO_FIELDS; i++) { sg_st32(M, mo_field(t, i), n); CB(); }
            sg_st32(M, t->state, n);
            CB();
        }
    }
    t->stop = 1;
    return NULL;
}

static void *mo_reader(void *arg) {
    mo_test *t = arg;
    while (!t->stop) {
        uint32_t s, d[MO_FIELDS];
        int i;
        if (t->ordered) {
            s = sg_ld32_acq_u(M, t->state);                                   /* acquire poll */
            for (i = 0; i < MO_FIELDS; i++) d[i] = sg_ld32(M, mo_field(t, i));
        } else {
            s = sg_ld32(M, t->state);
            CB();
            for (i = 0; i < MO_FIELDS; i++) { d[i] = sg_ld32(M, mo_field(t, i)); CB(); }
        }
        t->reads++;
        for (i = 0; i < MO_FIELDS; i++)
            if (d[i] < s) {
                if (!t->stale) { t->first_stale_seen = s; t->first_stale_field = d[i]; }
                t->stale++;
                break;
            }
    }
    return NULL;
}

/* One bounded run: `rounds` hand-offs (about 0.1-0.5 s on an M-series core). */
static mo_test mo_run(int ordered, uint32_t rounds) {
    mo_test t;
    pthread_t w, r;
    memset(&t, 0, sizeof t);
    t.state = (fx_alloc(MO_LINE * (MO_FIELDS + 2)) + MO_LINE - 1u) & ~(MO_LINE - 1u);
    t.rec = t.state + MO_LINE;
    t.ordered = ordered;
    t.rounds = rounds;
    pthread_create(&r, NULL, mo_reader, &t);
    pthread_create(&w, NULL, mo_writer, &t);
    pthread_join(w, NULL);
    pthread_join(r, NULL);
    return t;
}

static double mo_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

void test_memorder(int assert_relaxed) {
    const uint32_t rounds = 2000000u;
    double t0;
    mo_test o, r;
    t_name = "memorder";

    /* today's emission: release state store, acquire poll */
    t0 = mo_now();
    o = mo_run(1, rounds);
    fprintf(stderr, "  memorder: ordered emission: %lu reads, %lu stale (%.2f s)\n", o.reads, o.stale, mo_now() - t0);
    CHECK(o.reads > 0, "the reader never ran");
    CHECK(o.stale == 0, "ordered emission: %lu of %lu reads saw state %lu before its data (field %lu)", o.stale,
          o.reads, o.first_stale_seen, o.first_stale_field);

    /* the emission before the ordering policy: all plain */
    t0 = mo_now();
    r = mo_run(0, rounds);
    fprintf(stderr, "  memorder: relaxed emission: %lu reads, %lu stale (%.2f s)%s\n", r.reads, r.stale, mo_now() - t0,
            r.stale ? "" : " -- this CPU did not reorder in this run");
    if (assert_relaxed)
        CHECK(r.stale == 0, "relaxed emission: %lu of %lu reads saw state %lu before its data (field %lu)", r.stale,
              r.reads, r.first_stale_seen, r.first_stale_field);
}
