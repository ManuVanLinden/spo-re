// Our own shader (pipeline test): colour = blendindices (UBYTE4, 0..255) * c1.x, position from a SHORT2 input.
// c0 = (2/W, -2/H, -1, 1), c1 = (1/255, 0, 0, 0)
vs_3_0
dcl_position v0
dcl_blendindices v1
dcl_color o1
dcl_position o0
mad r0.xy, v0.xy, c0.xy, c0.zw
mov r0.z, c1.y
mov r0.w, c0.w
mov o0, r0
mul o1, v1, c1.xxxx
