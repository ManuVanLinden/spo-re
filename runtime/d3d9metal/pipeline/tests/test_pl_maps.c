/* test_pl_maps.c -- unit checks of the D3D9 -> Metal mapping tables (pipeline/pl_maps.c). Host-only, no GPU.
 * The expected Metal values are spelled out as numbers from the Metal headers (pl_draw.m static-asserts them). */
#include <stdio.h>
#include <string.h>

#include "../pl_int.h"

static int pass, fail;
#define EQ(a, b) do { unsigned long long a_ = (unsigned long long)(a), b_ = (unsigned long long)(b); \
    if (a_ == b_) pass++; else { fail++; fprintf(stderr, "%s:%d: %s = %llu, expected %llu\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

int main(void) {
    /* blend factors: D3DBLEND 1..15 */
    static const unsigned bf[16] = {1, 0, 1, 2, 3, 4, 5, 8, 9, 6, 7, 10, 4, 5, 11, 12};
    for (unsigned b = 1; b <= 15; b++) EQ(pl_blend_factor(b, 0), bf[b]);
    EQ(pl_blend_factor(7, 1), 1);            /* DESTALPHA on an X8 target reads 1 -> One */
    EQ(pl_blend_factor(8, 1), 0);            /* INVDESTALPHA -> Zero */
    EQ(pl_blend_factor(9, 1), 6);            /* DESTCOLOR unaffected */
    uint32_t s, d;
    EQ(pl_blend_both(12, &s, &d), 1); EQ(s, 5); EQ(d, 6);
    EQ(pl_blend_both(13, &s, &d), 1); EQ(s, 6); EQ(d, 5);
    EQ(pl_blend_both(5, &s, &d), 0);
    for (unsigned o = 1; o <= 5; o++) EQ(pl_blend_op(o), o - 1);
    /* colour write mask: D3D RGBA 1/2/4/8 -> Metal 8/4/2/1 */
    EQ(pl_color_write_mask(0xf), 0xf);
    EQ(pl_color_write_mask(1), 8); EQ(pl_color_write_mask(2), 4); EQ(pl_color_write_mask(4), 2); EQ(pl_color_write_mask(8), 1);
    EQ(pl_color_write_mask(0x7), 0xe);
    /* compare and stencil */
    for (unsigned f = 1; f <= 8; f++) EQ(pl_compare(f), f - 1);
    EQ(pl_compare(0), 7);
    for (unsigned o = 1; o <= 8; o++) EQ(pl_stencil_op(o), o - 1);
    /* cull with clockwise front faces */
    EQ(pl_cull(1), 0); EQ(pl_cull(2), 1); EQ(pl_cull(3), 2);
    EQ(pl_fill(3), 0); EQ(pl_fill(2), 1); EQ(pl_fill(1), 0);
    /* address modes */
    EQ(pl_address(1), 2); EQ(pl_address(2), 3); EQ(pl_address(3), 0); EQ(pl_address(4), 5); EQ(pl_address(5), 1);
    EQ(pl_minmag(0), 0); EQ(pl_minmag(1), 0); EQ(pl_minmag(2), 1); EQ(pl_minmag(3), 1);
    EQ(pl_mip(0), 0); EQ(pl_mip(1), 1); EQ(pl_mip(2), 2);
    int ex;
    EQ(pl_border(0, &ex), 0); EQ(ex, 1);
    EQ(pl_border(0xff000000u, &ex), 1); EQ(ex, 1);
    EQ(pl_border(0xffffffffu, &ex), 2); EQ(ex, 1);
    EQ(pl_border(0xff808080u, &ex), 2); EQ(ex, 0);
    EQ(pl_border(0x40ffffffu, &ex), 0); EQ(ex, 0);
    /* vertex formats for every D3DDECLTYPE (0..16) */
    static const unsigned vf[17] = {28, 29, 30, 31, 42, 3, 16, 18, 9, 22, 24, 19, 21, 0, 40, 25, 27};
    static const unsigned vs[17] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8};
    for (unsigned t = 0; t <= 16; t++) { uint32_t sz; EQ(pl_vertex_format(t, &sz), vf[t]); EQ(sz, vs[t]); }
    EQ(pl_vertex_format(17, NULL), 0);
    /* topologies */
    uint32_t prim, cnt; int fan;
    EQ(pl_topology(1, 7, &prim, &cnt, &fan), 1); EQ(prim, 0); EQ(cnt, 7);
    EQ(pl_topology(2, 7, &prim, &cnt, &fan), 1); EQ(prim, 1); EQ(cnt, 14);
    EQ(pl_topology(3, 7, &prim, &cnt, &fan), 1); EQ(prim, 2); EQ(cnt, 8);
    EQ(pl_topology(4, 7, &prim, &cnt, &fan), 1); EQ(prim, 3); EQ(cnt, 21);
    EQ(pl_topology(5, 7, &prim, &cnt, &fan), 1); EQ(prim, 4); EQ(cnt, 9);
    EQ(pl_topology(6, 7, &prim, &cnt, &fan), 1); EQ(prim, 3); EQ(cnt, 21); EQ(fan, 1);
    EQ(pl_topology(9, 7, &prim, &cnt, &fan), 0);
    /* fans */
    uint32_t out[9];
    pl_fan_to_list(3, NULL, 4, 10, out);
    static const uint32_t f0[9] = {10, 11, 12, 10, 12, 13, 10, 13, 14};
    EQ(memcmp(out, f0, sizeof f0), 0);
    uint16_t i16[5] = {7, 3, 0xffff, 9, 1};
    pl_fan_to_list(3, i16, 2, 0, out);
    static const uint32_t f1[9] = {7, 3, 0xffff, 7, 0xffff, 9, 7, 9, 1};
    EQ(memcmp(out, f1, sizeof f1), 0);
    uint32_t i32[4] = {5, 6, 7, 8};
    pl_fan_to_list(2, i32, 4, 0, out);
    EQ(out[0], 5); EQ(out[4], 7); EQ(out[5], 8);
    /* primitive restart values */
    uint16_t r16[20] = {0};
    EQ(pl_has_restart(r16, 2, 20), 0);
    r16[17] = 0xffff;
    EQ(pl_has_restart(r16, 2, 20), 1);
    EQ(pl_has_restart(r16, 2, 17), 0);
    uint32_t r32[3] = {0xffff, 1, 2};
    EQ(pl_has_restart(r32, 4, 3), 0);
    r32[2] = 0xffffffffu;
    EQ(pl_has_restart(r32, 4, 3), 1);
    float c[4];
    pl_argb_to_float(0x80ff0033u, c);
    EQ((int)(c[0] * 255 + 0.5f), 255); EQ((int)(c[1] * 255 + 0.5f), 0); EQ((int)(c[2] * 255 + 0.5f), 0x33); EQ((int)(c[3] * 255 + 0.5f), 0x80);
    printf("test_pl_maps: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
