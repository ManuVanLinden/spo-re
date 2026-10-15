// Our own shader: hardware shadow-map lookup (D3D9 depth-texture compare) with the reference depth from c2.x.
ps_3_0
dcl_texcoord v0
dcl_2d s0
mov r1, v0
mov r1.z, c2.xxxx
texld r0, r1, s0
mov oC0, r0
