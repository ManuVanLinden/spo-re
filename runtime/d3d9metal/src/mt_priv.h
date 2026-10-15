/* mt_priv.h -- the device layer's private declarations shared by src/mt_*.mm (not for pipeline/ or shader/). */
#ifndef MT_PRIV_H
#define MT_PRIV_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
extern "C" {
#include "../../d3d9null/src/d3d9_int.h"
}
#pragma clang diagnostic pop
#include "d3d9metal_int.h"
#include "d3d9metal.h"

#define ARG(i) SG_COM_ARG(c, i)
#define MEM c->mem
#define O ((mt_obj *)c->obj)
#define M(S, Name, fn) {#Name, S_##S##_##Name, 4 + 4 * A_##S##_##Name, fn}
#define D3DLOCK_NO_DIRTY_UPDATE 0x8000u
#define D3DLOCK_NOSYSLOCK 0x800u

extern "C" {

extern d3d9metal_stats g_mst;
extern sg_com_iface mt_iface[MT_K_NKIND];
extern const sg_com_class mt_class;
extern int64_t g_live_kind[MT_K_NKIND];
extern int g_mt_trace;

static inline uint64_t mt_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static inline d3d9null_stats *nst(void) { return d3d9null_stats_ptr(); }
static inline uint32_t lvl_dim(uint32_t v, uint32_t l) { return v >> l ? v >> l : 1; }

/* objects (mt_d3d9.mm) */
mt_obj *mt_new(int kind, mt_dev *dev, mt_obj *owner);
void mt_obj_free(mt_obj *o);
void mt_kid_add(mt_obj *p, mt_obj *k);
uint32_t mt_put_new(sg_com_call *c, uint32_t out, mt_obj *o, uint32_t err);

/* resources (mt_resources.mm) */
const mt_format *mt_format_of(uint32_t d3dfmt);
uint32_t mt_texture_new(mt_dev *d, int kind, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels, uint32_t usage,
                        uint32_t fmt, uint32_t pool, uint32_t ms, uint32_t msq, mt_obj **out);
uint32_t mt_buffer_new(mt_dev *d, int kind, uint32_t len, uint32_t usage, uint32_t fmt_or_fvf, uint32_t pool, mt_obj **out);
void mt_res_release_metal(mt_obj *o);
mt_obj *mt_sub(mt_obj *t, uint32_t face, uint32_t level); /* level surface / volume, created on demand */
mt_obj *mt_top(mt_obj *s, uint32_t *face, uint32_t *level);  /* container (or itself) and where */
void mt_retire(mt_dev *d, void *metal_obj);               /* release once the current command buffer completes */
void mt_reap(mt_dev *d);
int mt_upload_region(mt_dev *d, mt_obj *t, uint32_t face, uint32_t level, const uint32_t box[6]);
int mt_upload_all(mt_dev *d, mt_obj *t);
int mt_readback_all(mt_dev *d, mt_obj *t);                /* GPU contents -> guest shadow (waits) */
uint32_t mt_shadow_of(mt_obj *t);
void mt_gpu_wrote(mt_obj *t);
extern const sg_com_method mt_texture_methods[], mt_cube_methods[], mt_volumetex_methods[], mt_surface_methods[],
    mt_volume_methods[], mt_vb_methods[], mt_ib_methods[], mt_query_methods[];
extern const unsigned mt_n_texture_methods, mt_n_cube_methods, mt_n_volumetex_methods, mt_n_surface_methods,
    mt_n_volume_methods, mt_n_vb_methods, mt_n_ib_methods, mt_n_query_methods;
uint32_t mt_dv_update_surface(sg_com_call *c);
uint32_t mt_dv_update_texture(sg_com_call *c);
uint32_t mt_dv_rt_data(sg_com_call *c);
uint32_t mt_dv_front_data(sg_com_call *c);
uint32_t mt_dv_stretch(sg_com_call *c);
uint32_t mt_dv_color_fill(sg_com_call *c);
uint32_t mt_dv_create_query(sg_com_call *c);
uint32_t mt_r_get_device(sg_com_call *c);
void mt_occlusion_pass_begun(mt_dev *d);

/* command buffers and passes (mt_resources.mm) */
void mt_device_metal_init(mt_dev *d);
void mt_device_metal_free(mt_dev *d);

/* present (mt_present.mm) */
uint32_t mt_present(sg_com_call *c, mt_dev *d, mt_obj *sc, uint32_t hwnd);
int mt_util_draw(mt_dev *d, int kind, mt_obj *src, const uint32_t srect[4], MT_ID(MTLTexture) dst, uint32_t dslice,
                 uint32_t dlevel, uint32_t dfmt, const uint32_t drect[4], uint32_t dw, uint32_t dh, int linear, const float rgba[4]);
enum { MT_UTIL_COPY = 0, MT_UTIL_FILL = 1, MT_UTIL_COPY_OPAQUE = 2, MT_UTIL_PRESENT = 3 };
void mt_device_lost(mt_dev *d, const char *what);
void mt_stall_beat(void);
void mt_private_free_all(mt_obj *o);
void mt_watch_tick(int force);
}

#endif
