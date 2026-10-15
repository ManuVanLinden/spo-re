/* sg_com.h -- guest-visible COM objects backed by host objects (runtime-abi.md section 8).
 *
 * A host object is exposed to the guest as one 16-byte record per interface in the window,
 * {vptr, handle, SG_COM_MAGIC, 0} (spore_guest.h sg_com_guest). vptr points to a guest vtable built
 * once per interface: an array of thunk addresses handed out by sg_thunk_register, so a guest
 * `mov ecx,[eax]; push eax; call [ecx+N]` goes through sg_lookup -> sg_thunk_fn[slot] like any import.
 * Every thunk slot runs one of a fixed pool of trampolines that knows its binding index; the shared
 * dispatcher reads `this`, checks the record, finds the host object through the handle, counts and
 * logs the call, runs the handler and pops the method's stack bytes (stdcall, `this` included).
 *
 * Declaring an interface:
 *
 *     static uint32_t present(sg_com_call *c) { ... return D3D_OK; }
 *     static const sg_com_method dev_methods[] = {
 *         SG_COM_IUNKNOWN,                                    // slots 0..2, the shared IUnknown
 *         {"Present", 17, 20, present},                       // name, slot, stack bytes, handler
 *     };
 *     static sg_com_iface dev_iface = {"IDirect3DDevice9", IID, 119, dev_methods, SG_COM_N(dev_methods),
 *                                      slot_names, slot_args, D3DERR_INVALIDCALL};
 *
 * Slots not in the table run a default handler that logs once (SPORE_HLE_LOG>=1) and returns
 * iface->dflt_result; its stack bytes come from slot_args (the d3d9.h arity), so the guest stack stays
 * balanced. If slot_args is given, the declared stack bytes are checked against it when the vtable is
 * built and a mismatch aborts.
 *
 * Reference counting is in sg_com_obj. A child whose refcount belongs to a container (a texture's
 * surface levels, as in D3D9) sets `owner`; AddRef/Release then act on the owner.
 *
 * Nothing in here knows about D3D9 or DirectSound. */
#ifndef SG_COM_H
#define SG_COM_H

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "spore_guest.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- HRESULTs used by the generic code -------------------------------------------------------- */
#define SG_S_OK 0x00000000u
#define SG_S_FALSE 0x00000001u
#define SG_E_NOTIMPL 0x80004001u
#define SG_E_NOINTERFACE 0x80004002u
#define SG_E_POINTER 0x80004003u
#define SG_E_FAIL 0x80004005u
#define SG_E_OUTOFMEMORY 0x8007000eu
#define SG_E_INVALIDARG 0x80070057u

typedef struct sg_guid {
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t d4[8];
} sg_guid;
extern const sg_guid SG_IID_IUnknown;
int sg_guid_eq(const sg_guid *a, const sg_guid *b);
/* Reads a GUID from guest memory. */
sg_guid sg_guid_load(const uint8_t *mem, uint32_t a);

struct sg_com_obj;
struct sg_com_iface;
struct sg_com_method;

/* What a handler sees. Arguments are numbered after `this`: SG_COM_ARG(c, 0) is the first one. */
typedef struct sg_com_call {
    sg_ctx *ctx;
    uint8_t *mem;
    uint32_t self;               /* the guest `this` (the record) */
    struct sg_com_obj *obj;      /* the host object behind it */
    const struct sg_com_iface *iface;
    const struct sg_com_method *m;
} sg_com_call;
#define SG_COM_ARG(c, i) sg_ld32((c)->mem, (c)->ctx->esp + 8u + 4u * (uint32_t)(i))
#define SG_COM_ARGF(c, i) sg_ldf32((c)->mem, (c)->ctx->esp + 8u + 4u * (uint32_t)(i))
#define SG_COM_PTR(c, a) ((void *)((c)->mem + (uint32_t)(a))) /* after a NULL check on a */

/* A handler returns eax. */
typedef uint32_t sg_com_handler(sg_com_call *c);

typedef struct sg_com_method {
    const char *name;
    uint16_t slot;
    uint16_t stack_bytes; /* bytes the method pops, `this` included: 4 + 4 * nargs */
    sg_com_handler *fn;
} sg_com_method;
#define SG_COM_N(a) ((unsigned)(sizeof(a) / sizeof((a)[0])))

uint32_t sg_com_qi(sg_com_call *c);
uint32_t sg_com_addref(sg_com_call *c);
uint32_t sg_com_release(sg_com_call *c);
#define SG_COM_IUNKNOWN {"QueryInterface", 0, 12, sg_com_qi}, {"AddRef", 1, 4, sg_com_addref}, {"Release", 2, 4, sg_com_release}

typedef struct sg_com_iface {
    const char *name;
    sg_guid iid;
    unsigned nslots;
    const sg_com_method *methods;
    unsigned nmethods;
    const char *const *slot_names; /* optional, nslots entries: names for unlisted slots */
    const uint8_t *slot_args;      /* optional, nslots entries: stdcall args excluding `this` */
    uint32_t dflt_result;          /* eax of an unlisted slot */
    pthread_mutex_t *lock;         /* optional: held around every call (the D3D9 device mutex) */
    /* filled in by sg_com_iface_init */
    uint32_t vtbl;                 /* guest vtable */
    unsigned bind0;                /* binding index of slot 0 */
} sg_com_iface;

/* Builds the guest vtable (once; thread-safe). Returns the vtable's guest address, 0 on failure. */
uint32_t sg_com_iface_init(sg_com_iface *iface);

/* ---- host objects ------------------------------------------------------------------------------ */
typedef struct sg_com_class {
    const char *name;
    void (*destroy)(struct sg_com_obj *o);  /* called once when the count reaches 0; must call sg_com_obj_retire */
    /* optional: an interface the object does not expose yet (e.g. created on demand). Return the
     * guest record (with a reference added) or 0. */
    uint32_t (*query)(struct sg_com_obj *o, const sg_guid *iid);
} sg_com_class;

#define SG_COM_MAX_REC 4
typedef struct sg_com_obj {
    const sg_com_class *cls;
    uint32_t refs;               /* atomic */
    struct sg_com_obj *owner;    /* refcount delegate (container), or NULL */
    uint32_t handle;
    unsigned nrec;
    struct { const sg_com_iface *iface; uint32_t guest; } rec[SG_COM_MAX_REC];
} sg_com_obj;

/* Initialises the header with one reference and a handle. Returns 0 on success. */
int sg_com_obj_init(sg_com_obj *o, const sg_com_class *cls);
/* Adds a guest record for `iface` (builds the vtable if needed). Returns the guest address. */
uint32_t sg_com_obj_expose(sg_com_obj *o, sg_com_iface *iface);
/* The guest record for `iface`, or the first one if iface is NULL. 0 if not exposed. */
uint32_t sg_com_obj_guest(const sg_com_obj *o, const sg_com_iface *iface);
uint32_t sg_com_obj_addref(sg_com_obj *o);
uint32_t sg_com_obj_release(sg_com_obj *o);  /* runs cls->destroy at 0 */
/* Invalidates the records (magic and vptr zeroed, quarantined before reuse) and frees the handle.
 * Called by destroy; does not free the host memory. */
void sg_com_obj_retire(sg_com_obj *o);
/* Host object behind a guest pointer, or NULL if it is not a live record of ours. */
sg_com_obj *sg_com_lookup(const uint8_t *mem, uint32_t guest_this);
/* Writes `guest` to the out pointer `out` (if non-zero). */
void sg_com_put(uint8_t *mem, uint32_t out, uint32_t guest);

/* ---- guest memory owned by the COM layer (Lock buffers, records, vtables) --------------------- */
/* 16-aligned, zero-filled. Uses a private win32 heap when runtime/win32 is linked, otherwise
 * hle_guest_alloc (never freed). Returns 0 on exhaustion. */
uint32_t sg_com_galloc(uint32_t size);
void sg_com_gfree(uint32_t a);
extern uint8_t *sg_com_mem; /* the window base; set by sg_com_init or the first use */
void sg_com_init(uint8_t *mem);

/* ---- statistics and tracing -------------------------------------------------------------------- */
/* Trace level: SPORE_HLE_LOG (2 logs every COM call). SPORE_COM_STATS=1 prints the per-method
 * counts at exit (also at level 2). */
extern int sg_com_trace;
uint64_t sg_com_count(const sg_com_iface *iface, unsigned slot);
uint64_t sg_com_total_calls(void);
void sg_com_dump_stats(FILE *f);
unsigned sg_com_bindings_used(void);
unsigned sg_com_live_objects(void);

#ifdef __cplusplus
}
#endif
#endif
