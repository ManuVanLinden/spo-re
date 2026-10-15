/* test_com.c -- host-only tests of the COM machinery: objects called through their guest vtables by a
 * translated-code-style caller, IUnknown, reference counts, containers, the runtime-abi entry points. */
#include "com_harness.h"

#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static const sg_guid IID_IWidget = {0x11111111, 0x2222, 0x3333, {1, 2, 3, 4, 5, 6, 7, 8}};
static const sg_guid IID_IGadget = {0x99999999, 0x2222, 0x3333, {1, 2, 3, 4, 5, 6, 7, 8}};
static const sg_guid IID_ILate = {0x55555555, 0x2222, 0x3333, {1, 2, 3, 4, 5, 6, 7, 8}};

typedef struct widget {
    sg_com_obj com;
    uint32_t value;
    int destroyed;
} widget;
static int destroyed_count;

static uint32_t w_add(sg_com_call *c) { /* Add(a, b): this->value += a + b, returns value */
    widget *w = (widget *)c->obj;
    w->value += SG_COM_ARG(c, 0) + SG_COM_ARG(c, 1);
    return w->value;
}
static uint32_t w_get(sg_com_call *c) { /* Get(out*) */
    uint32_t out = SG_COM_ARG(c, 0);
    if (!out) return SG_E_POINTER;
    sg_st32(c->mem, out, ((widget *)c->obj)->value);
    return SG_S_OK;
}
static const char *const w_names[] = {"QueryInterface", "AddRef", "Release", "Add", "Get", "Unused"};
static const uint8_t w_args[] = {2, 0, 0, 2, 1, 3};
static const sg_com_method w_methods[] = {SG_COM_IUNKNOWN, {"Add", 3, 12, w_add}, {"Get", 4, 8, w_get}};
static sg_com_iface widget_iface = {"IWidget", {0x11111111, 0x2222, 0x3333, {1, 2, 3, 4, 5, 6, 7, 8}}, 6, w_methods,
                                    SG_COM_N(w_methods), w_names, w_args, 0x80004001u, NULL, 0, 0};
static const sg_com_method g_methods[] = {SG_COM_IUNKNOWN, {"Get", 3, 8, w_get}};
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static sg_com_iface gadget_iface = {"IGadget", {0x99999999, 0x2222, 0x3333, {1, 2, 3, 4, 5, 6, 7, 8}}, 4, g_methods,
                                    SG_COM_N(g_methods), NULL, NULL, 0, &g_lock, 0, 0};
static const sg_com_method l_methods[] = {SG_COM_IUNKNOWN};
static sg_com_iface late_iface = {"ILate", {0x55555555, 0x2222, 0x3333, {1, 2, 3, 4, 5, 6, 7, 8}}, 3, l_methods,
                                  SG_COM_N(l_methods), NULL, NULL, 0, NULL, 0, 0};

static void w_destroy(sg_com_obj *o) {
    widget *w = (widget *)o;
    sg_com_obj_retire(o);
    w->destroyed = 1;
    destroyed_count++;
}
static uint32_t w_query(sg_com_obj *o, const sg_guid *iid) {
    if (!sg_guid_eq(iid, &IID_ILate)) return 0;
    uint32_t g = sg_com_obj_expose(o, &late_iface);
    if (g) sg_com_obj_addref(o);
    return g;
}
static const sg_com_class widget_class = {"widget", w_destroy, w_query};

static widget *new_widget(uint32_t v) {
    widget *w = calloc(1, sizeof *w);
    sg_com_obj_init(&w->com, &widget_class);
    w->value = v;
    return w;
}

static uint32_t put_guid(const sg_guid *g) {
    uint32_t a = gscratch(16);
    memcpy(M + a, &g->d1, 4);
    memcpy(M + a + 4, &g->d2, 2);
    memcpy(M + a + 6, &g->d3, 2);
    memcpy(M + a + 8, g->d4, 8);
    return a;
}

static uint32_t raw_called;
static void raw_method(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) { /* sg_com_host from this */
    raw_called = *(uint32_t *)sg_com_host(ctx, SG_ARG(0));
    SG_RETURN_STDCALL(77, 4);
}

static int dies(void (*fn)(void)) {
    pid_t p = fork();
    int st;
    if (!p) {
        freopen("/dev/null", "w", stderr);
        fn();
        _exit(0);
    }
    waitpid(p, &st, 0);
    return WIFSIGNALED(st);
}
static const sg_com_method bad_methods[] = {SG_COM_IUNKNOWN, {"Add", 3, 8 /* arity says 12 */, w_add}};
static sg_com_iface bad_iface = {"IBad", {0}, 6, bad_methods, SG_COM_N(bad_methods), w_names, w_args, 0, NULL, 0, 0};
static void build_bad(void) { sg_com_iface_init(&bad_iface); }
static uint32_t stale;
static void call_stale(void) { gcall(stale, 3, 2, 1, 2); }

int main(void) {
    harness_init();

    /* one object, called through the guest vtable */
    widget *w = new_widget(5);
    uint32_t g = sg_com_obj_expose(&w->com, &widget_iface);
    CHECK(g != 0);
    CHECK_EQ(g & 15, 0);
    CHECK_EQ(gld(g + 8), SG_COM_MAGIC);
    CHECK_EQ(gld(g + 4), w->com.handle);
    uint32_t vt = gld(g);
    CHECK_EQ(vt, widget_iface.vtbl);
    for (unsigned s = 0; s < 6; s++) {
        uint32_t t = gld(vt + 4 * s);
        CHECK(t >= SG_THUNK_BASE && t < SG_THUNK_LIMIT && !(t & 15));
        CHECK(sg_thunk_fn[(t - SG_THUNK_BASE) / 16] != NULL);
    }
    CHECK(sg_com_lookup(M, g) == &w->com);
    CHECK_EQ(gcall(g, 3, 2, 10, 20), 35);           /* Add */
    uint32_t out = gscratch(4);
    CHECK_EQ(gcall(g, 4, 1, out), 0);               /* Get */
    CHECK_EQ(gld(out), 35);
    CHECK_EQ(gcall(g, 4, 1, 0), SG_E_POINTER);
    CHECK_EQ(gcall(g, 5, 3, 1, 2, 3), 0x80004001u);  /* unlisted slot: default result, 16 bytes popped */
    CHECK_EQ(sg_com_count(&widget_iface, 3), 1);
    CHECK_EQ(sg_com_count(&widget_iface, 4), 2);
    CHECK_EQ(sg_com_count(&widget_iface, 5), 1);

    /* IUnknown */
    CHECK_EQ(gcall(g, 1, 0), 2);                    /* AddRef */
    CHECK_EQ(gcall(g, 2, 0), 1);                    /* Release */
    uint32_t ppv = gscratch(4);
    CHECK_EQ(gcall(g, 0, 2, put_guid(&IID_IWidget), ppv), 0);
    CHECK_EQ(gld(ppv), g);
    CHECK_EQ(w->com.refs, 2);
    CHECK_EQ(gcall(g, 0, 2, put_guid(&SG_IID_IUnknown), ppv), 0);
    CHECK_EQ(gld(ppv), g);
    CHECK_EQ(gcall(g, 0, 2, put_guid(&IID_IGadget), ppv), SG_E_NOINTERFACE);
    CHECK_EQ(gld(ppv), 0);
    CHECK_EQ(gcall(g, 0, 2, put_guid(&IID_IGadget), 0), SG_E_POINTER);
    /* a second interface on the same object shares the handle and the count */
    uint32_t g2 = sg_com_obj_expose(&w->com, &gadget_iface);
    CHECK(g2 && g2 != g);
    CHECK_EQ(gld(g2 + 4), w->com.handle);
    CHECK_EQ(gcall(g, 0, 2, put_guid(&IID_IGadget), ppv), 0);
    CHECK_EQ(gld(ppv), g2);
    CHECK_EQ(gcall(g2, 3, 1, out), 0);              /* IGadget::Get (with the interface lock) */
    CHECK_EQ(gld(out), 35);
    CHECK_EQ(gcall(g2, 0, 2, put_guid(&IID_IWidget), ppv), 0);
    CHECK_EQ(gld(ppv), g);
    /* an interface created on demand by the class's query hook */
    CHECK_EQ(gcall(g, 0, 2, put_guid(&IID_ILate), ppv), 0);
    uint32_t g3 = gld(ppv);
    CHECK(g3 && g3 != g && g3 != g2);
    CHECK_EQ(w->com.refs, 6);
    for (int i = 0; i < 5; i++) gcall(g3, 2, 0);
    CHECK_EQ(w->com.refs, 1);
    CHECK_EQ(w->destroyed, 0);
    CHECK_EQ(gcall(g, 2, 0), 0);                     /* final Release destroys */
    CHECK_EQ(w->destroyed, 1);
    CHECK_EQ(gld(g + 8), 0);                         /* records invalidated */
    CHECK_EQ(gld(g2 + 8), 0);
    CHECK(sg_com_lookup(M, g) == NULL);
    stale = g;
    CHECK(dies(call_stale));                         /* a call through a stale pointer is reported */

    /* container: a child whose count belongs to its owner (D3D9 surface levels) */
    widget *parent = new_widget(1), *child = new_widget(2);
    uint32_t gp = sg_com_obj_expose(&parent->com, &widget_iface);
    uint32_t gc = sg_com_obj_expose(&child->com, &widget_iface);
    child->com.owner = &parent->com;
    CHECK_EQ(gcall(gc, 1, 0), 2);                    /* AddRef on the child counts on the parent */
    CHECK_EQ(parent->com.refs, 2);
    CHECK_EQ(gcall(gc, 2, 0), 1);
    CHECK_EQ(gcall(gp, 3, 2, 1, 1), 3);
    CHECK_EQ(gcall(gp, 2, 0), 0);
    CHECK_EQ(parent->destroyed, 1);

    /* handles and records are reused */
    unsigned live0 = sg_com_live_objects();
    for (int i = 0; i < 3000; i++) {
        widget *x = new_widget((uint32_t)i);
        uint32_t gx = sg_com_obj_expose(&x->com, &widget_iface);
        if (gcall(gx, 3, 2, 1, 0) != (uint32_t)i + 1) { CHECK(0); break; }
        gcall(gx, 2, 0);
        free(x);
    }
    CHECK_EQ(sg_com_live_objects(), live0);

    /* runtime-abi 8.1 entry points: sg_com_vtable / sg_com_wrap / sg_com_host */
    static uint32_t host_value = 0xabcd;
    sg_func *const raw[1] = {raw_method};
    uint32_t rvt = sg_com_vtable(&T, raw, 1, "IRaw");
    uint32_t rg = sg_com_wrap(&T, &host_value, rvt);
    CHECK(rvt && rg);
    CHECK(sg_com_host(&T, rg) == &host_value);
    CHECK_EQ(gcall(rg, 0, 0), 77);
    CHECK_EQ(raw_called, 0xabcd);
    CHECK(sg_com_host(&T, rg + 16) == NULL);

    /* declaration errors abort when the vtable is built */
    CHECK(dies(build_bad));

    CHECK(sg_com_total_calls() > 0);
    CHECK_EQ(sg_com_bindings_used(), 6 + 4 + 3);
    return harness_report("runtime/com");
}
