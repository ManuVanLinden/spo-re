// Our own shader: vertex texture fetch. Color = texldl(s0, texcoord, lod 0).x replicated.
// c0 = (2/W, -2/H, -1, 1), c1 = (0, 1, 0.5, 0)
vs_3_0
dcl_position v0
dcl_texcoord v1
dcl_position o0
dcl_texcoord o1
dcl_color o2
dcl_2d s0
mad r0.xy, v0.xy, c0.xy, c0.zw
mov r0.zw, v0
mov o0, r0
mov r2, v1
mov r2.zw, c1.xxxx
texldl r1, r2, s0
mov o1, v1
mov o2, r1.xxxx
