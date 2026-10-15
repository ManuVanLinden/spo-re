// Our own fixture: vs_3_0 with matrix-palette skinning via a0 relative addressing,
// the scalar ops, comparison ops and several outputs.
vs_3_0
def c200, 3.0, 0.5, 0.0, 1.0
dcl_position v0
dcl_normal v1
dcl_blendindices v2
dcl_blendweight v3
dcl_texcoord v4
dcl_color v5
dcl_position o0
dcl_texcoord o1
dcl_texcoord1 o2.xyz
dcl_color o3
mul r0, v2, c200.xxxx
mova a0, r0
mul r1, v3.xxxx, c[a0.x + 10]
mad r1, v3.yyyy, c[a0.y + 10], r1
dp4 r2.x, v0, r1
dp4 r2.y, v0, c[a0.x + 11]
dp4 r2.z, v0, c[a0.x + 12]
mov r2.w, c200.w
dp4 o0.x, r2, c0
dp4 o0.y, r2, c1
dp4 o0.z, r2, c2
dp4 o0.w, r2, c3
dp3 r3.x, v1, c4
rsq r3.y, r3.xxxx
rcp r3.z, r3.yyyy
exp r3.w, r3.zzzz
nrm r4, v1
slt r5, r4, c5
sge r6, r4, c5
frc r7, v4
max r7, r7, neg(r5)
min r7, r7, abs(r6)
lrp r8, c6, r7, r4
sincos r9.xy, r3.xxxx
pow r9.z, r3.xxxx, r3.yyyy
abs r9.w, r9.xxxx
mov o1, r8
add_sat o2.xyz, r9, c7
mul o3, v5, c8
