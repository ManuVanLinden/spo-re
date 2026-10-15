// Our own shader (pipeline test): pixel-space position -> clip space, texcoord and colour passed through.
// c0 = (2/W, -2/H, -1, 1)
vs_3_0
dcl_position v0
dcl_texcoord v1
dcl_color v2
dcl_position o0
dcl_texcoord o1
dcl_color o2
mad r0.xy, v0.xy, c0.xy, c0.zw
mov r0.zw, v0
mov o0, r0
mov o1, v1
mov o2, v2
