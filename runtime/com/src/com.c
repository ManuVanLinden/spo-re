/* com.c -- guest-visible COM machinery: handle table, guest records, guest vtables over the
 * thunk region, the shared method dispatcher, IUnknown, guest memory and call statistics.
 * See sg_com.h. */
#include "sg_com.h"

#include <stdlib.h>
#include <string.h>

const sg_guid SG_IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

int sg_guid_eq(const sg_guid *a, const sg_guid *b) {
    return a->d1 == b->d1 && a->d2 == b->d2 && a->d3 == b->d3 && !memcmp(a->d4, b->d4, 8);
}
sg_guid sg_guid_load(const uint8_t *mem, uint32_t a) {
    sg_guid g;
    memcpy(&g.d1, mem + a, 4);
    memcpy(&g.d2, mem + a + 4, 2);
    memcpy(&g.d3, mem + a + 6, 2);
    memcpy(g.d4, mem + a + 8, 8);
    return g;
}

/* ---- tracing --------------------------------------------------------------------------------- */
int sg_com_trace = 1;
static int stats_at_exit;
static void dump_at_exit(void) { sg_com_dump_stats(stderr); }
__attribute__((constructor)) static void com_env(void) {
    const char *s = getenv("SPORE_HLE_LOG");
    if (s && *s) sg_com_trace = atoi(s);
    s = getenv("SPORE_COM_STATS");
    stats_at_exit = (s && *s && *s != '0') || sg_com_trace >= 2;
    if (stats_at_exit) atexit(dump_at_exit);
}

/* ---- guest memory ------------------------------------------------------------------------------ */
uint8_t *sg_com_mem;
void sg_com_init(uint8_t *mem) {
    if (mem) sg_com_mem = mem;
}

/* runtime/win32's heap when it is linked; these weak defaults say "not linked". */
__attribute__((weak)) uint32_t w32_heap_create(uint32_t flags, uint32_t initial, uint32_t maximum) {
    (void)flags; (void)initial; (void)maximum;
    return 0;
}
__attribute__((weak)) uint32_t w32_heap_alloc(uint32_t heap, uint32_t flags, uint32_t size) {
    (void)heap; (void)flags; (void)size;
    return 0;
}
__attribute__((weak)) int w32_heap_free(uint32_t heap, uint32_t flags, uint32_t p) {
    (void)heap; (void)flags; (void)p;
    return 0;
}
extern uint32_t hle_guest_alloc(uint32_t size, uint32_t align);

static uint32_t com_heap;
static int com_heap_tried;
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t sg_com_galloc(uint32_t size) {
    uint32_t h;
    if (!size) size = 1;
    pthread_mutex_lock(&heap_lock);
    if (!com_heap_tried) {
        com_heap_tried = 1;
        com_heap = w32_heap_create(0, 0, 0);
    }
    h = com_heap;
    pthread_mutex_unlock(&heap_lock);
    if (h) return w32_heap_alloc(h, 0x8 /* HEAP_ZERO_MEMORY */, size); /* 16-aligned (w32_heap.c) */
    return hle_guest_alloc(size, 16);
}
void sg_com_gfree(uint32_t a) {
    if (a && com_heap) w32_heap_free(com_heap, 0, a);
}

/* ---- handle table: 256 chunks of 4096 entries, lock-free reads ------------------------------- */
typedef struct hentry {
    void *host;
    sg_com_obj *obj;
} hentry;
#define CHUNK_BITS 12
#define NCHUNKS 256
static hentry *chunks[NCHUNKS];
static uint32_t next_handle = 1, free_head; /* free list threaded through host (as an index) */
static unsigned live;
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;

static hentry *entry(uint32_t h) {
    hentry *c;
    if (h >> CHUNK_BITS >= NCHUNKS) return NULL;
    c = __atomic_load_n(&chunks[h >> CHUNK_BITS], __ATOMIC_ACQUIRE);
    return c ? &c[h & ((1u << CHUNK_BITS) - 1u)] : NULL;
}

static uint32_t handle_new(void *host, sg_com_obj *obj) {
    uint32_t h;
    hentry *e;
    pthread_mutex_lock(&table_lock);
    if (free_head) {
        h = free_head;
        free_head = (uint32_t)(uintptr_t)entry(h)->host;
    } else {
        h = next_handle;
        if (h >> CHUNK_BITS >= NCHUNKS) {
            pthread_mutex_unlock(&table_lock);
            return 0;
        }
        if (!chunks[h >> CHUNK_BITS]) {
            hentry *c = calloc(1u << CHUNK_BITS, sizeof *c);
            if (!c) {
                pthread_mutex_unlock(&table_lock);
                return 0;
            }
            __atomic_store_n(&chunks[h >> CHUNK_BITS], c, __ATOMIC_RELEASE);
        }
        next_handle++;
    }
    e = entry(h);
    e->obj = obj;
    __atomic_store_n(&e->host, host, __ATOMIC_RELEASE);
    live++;
    pthread_mutex_unlock(&table_lock);
    return h;
}
static void handle_free(uint32_t h) {
    hentry *e = entry(h);
    if (!e || !h) return;
    pthread_mutex_lock(&table_lock);
    e->obj = NULL;
    e->host = (void *)(uintptr_t)free_head;
    free_head = h;
    live--;
    pthread_mutex_unlock(&table_lock);
}
unsigned sg_com_live_objects(void) { return live; }

/* ---- records ----------------------------------------------------------------------------------- */
/* Retired records stay invalid (magic 0) in a FIFO before their memory is reused, so a stale
 * guest pointer is caught by the magic check for a while. */
#define QUARANTINE 1024
static uint32_t quarantine[QUARANTINE];
static unsigned q_head;

static uint32_t record_new(uint32_t vtbl, uint32_t handle) {
    uint32_t g = sg_com_galloc(16);
    uint8_t *mem = sg_com_mem;
    if (!g) return 0;
    sg_st32(mem, g + 4, handle);
    sg_st32(mem, g + 8, SG_COM_MAGIC);
    sg_st32(mem, g + 12, 0);
    __atomic_store_n((uint32_t *)(void *)(mem + g), vtbl, __ATOMIC_RELEASE);
    return g;
}
static void record_retire(uint32_t g) {
    uint32_t old;
    uint8_t *mem = sg_com_mem;
    sg_st32(mem, g + 8, 0);
    sg_st32(mem, g, 0);
    sg_st32(mem, g + 4, 0);
    pthread_mutex_lock(&table_lock);
    old = quarantine[q_head];
    quarantine[q_head] = g;
    q_head = (q_head + 1) % QUARANTINE;
    pthread_mutex_unlock(&table_lock);
    if (old) sg_com_gfree(old);
}

static hentry *lookup_entry(const uint8_t *mem, uint32_t self) {
    uint32_t h;
    if (!self || (self & 15u) || self >= SG_THUNK_BASE) return NULL;
    if (sg_ld32(mem, self + 8) != SG_COM_MAGIC) return NULL;
    h = sg_ld32(mem, self + 4);
    return h ? entry(h) : NULL;
}
sg_com_obj *sg_com_lookup(const uint8_t *mem, uint32_t self) {
    hentry *e = lookup_entry(mem, self);
    return e ? e->obj : NULL;
}
void sg_com_put(uint8_t *mem, uint32_t out, uint32_t guest) {
    if (out) sg_st32(mem, out, guest);
}

/* ---- objects ----------------------------------------------------------------------------------- */
int sg_com_obj_init(sg_com_obj *o, const sg_com_class *cls) {
    o->cls = cls;
    o->refs = 1;
    o->owner = NULL;
    o->nrec = 0;
    o->handle = handle_new(o, o);
    return o->handle ? 0 : -1;
}
uint32_t sg_com_obj_expose(sg_com_obj *o, sg_com_iface *iface) {
    uint32_t vt, g;
    for (unsigned i = 0; i < o->nrec; i++)
        if (o->rec[i].iface == iface) return o->rec[i].guest;
    if (o->nrec >= SG_COM_MAX_REC) return 0;
    vt = sg_com_iface_init(iface);
    if (!vt) return 0;
    g = record_new(vt, o->handle);
    if (!g) return 0;
    o->rec[o->nrec].iface = iface;
    o->rec[o->nrec].guest = g;
    o->nrec++;
    return g;
}
uint32_t sg_com_obj_guest(const sg_com_obj *o, const sg_com_iface *iface) {
    if (!o || !o->nrec) return 0;
    if (!iface) return o->rec[0].guest;
    for (unsigned i = 0; i < o->nrec; i++)
        if (o->rec[i].iface == iface) return o->rec[i].guest;
    return 0;
}
uint32_t sg_com_obj_addref(sg_com_obj *o) {
    while (o->owner) o = o->owner;
    return __atomic_add_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
}
uint32_t sg_com_obj_release(sg_com_obj *o) {
    uint32_t r;
    while (o->owner) o = o->owner;
    r = __atomic_load_n(&o->refs, __ATOMIC_ACQUIRE);
    if (!r) { /* over-release: D3D9 would crash or corrupt; report and keep the object */
        if (sg_com_trace >= 1) fprintf(stderr, "com: %s: Release with refcount 0 (ignored)\n", o->cls ? o->cls->name : "?");
        return 0;
    }
    r = __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
    if (!r && o->cls && o->cls->destroy) o->cls->destroy(o);
    return r;
}
void sg_com_obj_retire(sg_com_obj *o) {
    for (unsigned i = 0; i < o->nrec; i++) record_retire(o->rec[i].guest);
    o->nrec = 0;
    handle_free(o->handle);
    o->handle = 0;
}

/* ---- IUnknown ---------------------------------------------------------------------------------- */
uint32_t sg_com_qi(sg_com_call *c) {
    uint32_t riid = SG_COM_ARG(c, 0), ppv = SG_COM_ARG(c, 1), g = 0;
    sg_com_obj *o = c->obj;
    sg_guid iid;
    if (!ppv) return SG_E_POINTER;
    if (!riid) {
        sg_st32(c->mem, ppv, 0);
        return SG_E_INVALIDARG;
    }
    iid = sg_guid_load(c->mem, riid);
    if (sg_guid_eq(&iid, &SG_IID_IUnknown)) g = o->rec[0].guest;
    for (unsigned i = 0; !g && i < o->nrec; i++)
        if (sg_guid_eq(&iid, &o->rec[i].iface->iid)) g = o->rec[i].guest;
    if (g) {
        sg_com_obj_addref(o);
    } else if (o->cls && o->cls->query) {
        g = o->cls->query(o, &iid);
    }
    sg_st32(c->mem, ppv, g);
    if (!g && sg_com_trace >= 1)
        fprintf(stderr, "com: %s::QueryInterface({%08x-%04x-%04x-...}): E_NOINTERFACE\n", c->iface->name, iid.d1, iid.d2, iid.d3);
    return g ? SG_S_OK : SG_E_NOINTERFACE;
}
uint32_t sg_com_addref(sg_com_call *c) { return sg_com_obj_addref(c->obj); }
uint32_t sg_com_release(sg_com_call *c) { return sg_com_obj_release(c->obj); }

/* ---- bindings and trampolines ------------------------------------------------------------------ */
#define MAX_BIND 2048
typedef struct binding {
    sg_com_iface *iface;
    const sg_com_method *m;
    const char *name;
    sg_com_handler *fn;
    uint16_t slot, bytes;
    uint8_t warned;
    uint64_t count;
} binding;
static binding binds[MAX_BIND];
static unsigned nbinds;
static pthread_mutex_t bind_lock = PTHREAD_MUTEX_INITIALIZER;
unsigned sg_com_bindings_used(void) { return nbinds; }

static SG_NORETURN SG_COLD void bad_this(sg_ctx *ctx, uint8_t *mem, const binding *b, uint32_t self) {
    fprintf(stderr, "com: %s::%s called with a `this` that is not a live object: 0x%08x (vptr 0x%08x magic 0x%08x), "
                    "from guest 0x%08x\n", b->iface->name, b->name, self,
            self && self < SG_THUNK_BASE ? sg_ld32(mem, self) : 0u, self && self < SG_THUNK_BASE ? sg_ld32(mem, self + 8) : 0u,
            sg_ld32(mem, ctx->esp));
    fflush(NULL);
    abort();
}

static uint32_t unlisted(sg_com_call *c) { return c->iface->dflt_result; }

static void trace_call(sg_ctx *ctx, uint8_t *mem, const binding *b, uint32_t self, uint32_t eax) {
    char buf[256];
    int n = snprintf(buf, sizeof buf, "com: %s::%s(this=0x%08x", b->iface->name, b->name, self);
    for (unsigned i = 1; i < b->bytes / 4u && n < (int)sizeof buf - 16; i++)
        n += snprintf(buf + n, sizeof buf - (size_t)n, ", 0x%x", sg_ld32(mem, ctx->esp + 4u + 4u * i));
    fprintf(stderr, "%s) = 0x%x [from 0x%08x]%s\n", buf, eax, sg_ld32(mem, ctx->esp), b->m ? "" : " (unlisted slot)");
}

static void dispatch(sg_ctx *SG_RESTRICT ctx, uint8_t *mem, unsigned k) {
    binding *b = &binds[k];
    uint32_t self = sg_ld32(mem, ctx->esp + 4u), eax;
    hentry *e = lookup_entry(mem, self);
    sg_com_method fake;
    sg_com_call c;
    if (SG_UNLIKELY(!e || !e->obj)) bad_this(ctx, mem, b, self);
    __atomic_add_fetch(&b->count, 1, __ATOMIC_RELAXED);
    if (SG_UNLIKELY(!sg_com_mem)) sg_com_mem = mem;
    c.ctx = ctx;
    c.mem = mem;
    c.self = self;
    c.obj = e->obj;
    c.iface = b->iface;
    c.m = b->m;
    if (!b->m) {
        fake.name = b->name;
        fake.slot = b->slot;
        fake.stack_bytes = b->bytes;
        fake.fn = unlisted;
        c.m = &fake;
        if (!b->warned && sg_com_trace >= 1) {
            b->warned = 1;
            fprintf(stderr, "com: %s::%s (slot %u) has no handler; returning 0x%x\n", b->iface->name, b->name, b->slot,
                    b->iface->dflt_result);
        }
    }
    if (b->iface->lock) pthread_mutex_lock(b->iface->lock);
    eax = b->fn(&c);
    if (b->iface->lock) pthread_mutex_unlock(b->iface->lock);
    if (SG_UNLIKELY(sg_com_trace >= 2)) trace_call(ctx, mem, b, self, eax);
    ctx->eax = eax;
    SG_RET(b->bytes);
}

/* 2048 trampolines: tr_0x000 .. tr_0x7ff, each one dispatching its own binding index. */
#define TR(n) static void tr_##n(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) { dispatch(ctx, mem, n); }
#define TR16(p) TR(p##0) TR(p##1) TR(p##2) TR(p##3) TR(p##4) TR(p##5) TR(p##6) TR(p##7) \
    TR(p##8) TR(p##9) TR(p##a) TR(p##b) TR(p##c) TR(p##d) TR(p##e) TR(p##f)
#define TR256(p) TR16(p##0) TR16(p##1) TR16(p##2) TR16(p##3) TR16(p##4) TR16(p##5) TR16(p##6) TR16(p##7) \
    TR16(p##8) TR16(p##9) TR16(p##a) TR16(p##b) TR16(p##c) TR16(p##d) TR16(p##e) TR16(p##f)
TR256(0x0) TR256(0x1) TR256(0x2) TR256(0x3) TR256(0x4) TR256(0x5) TR256(0x6) TR256(0x7)
#define TP(n) tr_##n,
#define TP16(p) TP(p##0) TP(p##1) TP(p##2) TP(p##3) TP(p##4) TP(p##5) TP(p##6) TP(p##7) \
    TP(p##8) TP(p##9) TP(p##a) TP(p##b) TP(p##c) TP(p##d) TP(p##e) TP(p##f)
#define TP256(p) TP16(p##0) TP16(p##1) TP16(p##2) TP16(p##3) TP16(p##4) TP16(p##5) TP16(p##6) TP16(p##7) \
    TP16(p##8) TP16(p##9) TP16(p##a) TP16(p##b) TP16(p##c) TP16(p##d) TP16(p##e) TP16(p##f)
static sg_func *const trampolines[MAX_BIND] = {TP256(0x0) TP256(0x1) TP256(0x2) TP256(0x3) TP256(0x4) TP256(0x5)
                                                   TP256(0x6) TP256(0x7)};

static SG_NORETURN SG_COLD void decl_error(const sg_com_iface *i, const char *what, unsigned slot) {
    fprintf(stderr, "com: interface %s: %s (slot %u)\n", i->name, what, slot);
    abort();
}

uint32_t sg_com_iface_init(sg_com_iface *iface) {
    uint32_t vt;
    unsigned base;
    char **names;
    if (__atomic_load_n(&iface->vtbl, __ATOMIC_ACQUIRE)) return iface->vtbl;
    pthread_mutex_lock(&bind_lock);
    if (iface->vtbl) {
        pthread_mutex_unlock(&bind_lock);
        return iface->vtbl;
    }
    if (nbinds + iface->nslots > MAX_BIND) {
        pthread_mutex_unlock(&bind_lock);
        fprintf(stderr, "com: out of trampolines (%u + %u > %u)\n", nbinds, iface->nslots, MAX_BIND);
        abort();
    }
    vt = sg_com_galloc(4u * iface->nslots);
    if (!vt) {
        pthread_mutex_unlock(&bind_lock);
        return 0;
    }
    base = nbinds;
    names = calloc(iface->nslots, sizeof *names);
    for (unsigned s = 0; s < iface->nslots; s++) {
        binding *b = &binds[base + s];
        b->iface = iface;
        b->slot = (uint16_t)s;
        b->name = iface->slot_names ? iface->slot_names[s] : NULL;
        b->bytes = iface->slot_args ? (uint16_t)(4u + 4u * iface->slot_args[s]) : 0xffff;
        b->fn = unlisted;
    }
    for (unsigned i = 0; i < iface->nmethods; i++) {
        const sg_com_method *m = &iface->methods[i];
        binding *b;
        if (m->slot >= iface->nslots) decl_error(iface, "method slot out of range", m->slot);
        b = &binds[base + m->slot];
        if (b->m) decl_error(iface, "two methods declared for one slot", m->slot);
        if (iface->slot_args && b->bytes != m->stack_bytes) decl_error(iface, "stack bytes differ from the slot's arity", m->slot);
        if (iface->slot_names && strcmp(iface->slot_names[m->slot], m->name)) decl_error(iface, "name differs from the slot's", m->slot);
        b->m = m;
        b->name = m->name;
        b->bytes = m->stack_bytes;
        b->fn = m->fn;
    }
    for (unsigned s = 0; s < iface->nslots; s++) {
        binding *b = &binds[base + s];
        char nm[160];
        if (b->bytes == 0xffff) decl_error(iface, "slot has neither a method nor a known arity", s);
        if (!b->name) {
            snprintf(nm, sizeof nm, "slot%u", s);
            b->name = strdup(nm);
        }
        snprintf(nm, sizeof nm, "%s::%s", iface->name, b->name);
        names[s] = strdup(nm);
        sg_st32(sg_com_mem, vt + 4u * s, sg_thunk_register(trampolines[base + s], names[s]));
    }
    nbinds += iface->nslots;
    iface->bind0 = base;
    __atomic_store_n(&iface->vtbl, vt, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&bind_lock);
    return vt;
}

/* ---- statistics -------------------------------------------------------------------------------- */
uint64_t sg_com_count(const sg_com_iface *iface, unsigned slot) {
    if (!iface->vtbl || slot >= iface->nslots) return 0;
    return __atomic_load_n(&binds[iface->bind0 + slot].count, __ATOMIC_RELAXED);
}
uint64_t sg_com_total_calls(void) {
    uint64_t t = 0;
    for (unsigned k = 0; k < nbinds; k++) t += __atomic_load_n(&binds[k].count, __ATOMIC_RELAXED);
    return t;
}
void sg_com_dump_stats(FILE *f) {
    uint64_t t = sg_com_total_calls();
    if (!t) return;
    fprintf(f, "com: %llu calls to host COM methods, %u live objects\n", (unsigned long long)t, live);
    for (unsigned k = 0; k < nbinds; k++)
        if (binds[k].count)
            fprintf(f, "com:   %10llu  %s::%s%s\n", (unsigned long long)binds[k].count, binds[k].iface->name, binds[k].name,
                    binds[k].m ? "" : " (unlisted)");
}

/* ---- the runtime-abi section 8 entry points (spore_guest.h) ----------------------------------- */
uint32_t sg_com_vtable(sg_ctx *ctx, sg_func *const *methods, unsigned n, const char *iface) {
    uint32_t vt;
    if (ctx && ctx->mem) sg_com_init(ctx->mem);
    vt = sg_com_galloc(4u * n);
    if (!vt) return 0;
    for (unsigned i = 0; i < n; i++) sg_st32(sg_com_mem, vt + 4u * i, sg_thunk_register(methods[i], iface));
    return vt;
}
uint32_t sg_com_wrap(sg_ctx *ctx, void *host, uint32_t vtbl_guest) {
    uint32_t h, g;
    if (ctx && ctx->mem) sg_com_init(ctx->mem);
    h = handle_new(host, NULL);
    if (!h) return 0;
    g = record_new(vtbl_guest, h);
    if (!g) handle_free(h);
    return g;
}
void *sg_com_host(sg_ctx *ctx, uint32_t guest_this) {
    hentry *e = lookup_entry(ctx ? ctx->mem : sg_com_mem, guest_this);
    return e ? __atomic_load_n(&e->host, __ATOMIC_ACQUIRE) : NULL;
}
