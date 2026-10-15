// Our own shader: SetStreamSourceFreq instancing. Stream 0 = unit-square corner (v0.xy),
// stream 1 = per-instance (offset.xy, size, 0) in v1 and a D3DCOLOR in v2.
// c0 = (2/W, -2/H, -1, 1), c1 = (0, 1, 0.5, 0)
vs_3_0
dcl_position v0
dcl_texcoord v1
dcl_color v2
dcl_position o0
dcl_texcoord o1
dcl_color o2
mad r0.xy, v0.xy, v1.zz, v1.xy
add r0.xy, r0.xy, neg(c1.zzzz)
mad r0.xy, r0.xy, c0.xy, c0.zw
mov r0.z, c1.z
mov r0.w, c1.y
mov o0, r0
mov o1, c1.xxxx
mov o2, v2
