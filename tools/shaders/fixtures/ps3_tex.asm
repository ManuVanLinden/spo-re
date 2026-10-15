// Our own fixture: ps_3_0 sampling forms, cube and volume samplers, modifiers, texkill, cmp, dp2add.
ps_3_0
def c30, 0.0, 0.5, 1.0, -1.0
dcl_texcoord v0
dcl_texcoord1 v1.xyz
dcl_color v2
dcl_texcoord2_centroid v3
dcl_2d s0
dcl_cube s1
dcl_volume s2
dcl_2d s3
texld r0, v0, s0
texldp r1, v0, s0
texldb r2, v0, s3
texldl r3, v0, s3
texldd r4, v0, s3, c10, c11
texld_pp r5, v1, s1
texld r6, v3, s2.wzyx
mul r0, r0, v2
mad_sat r0, r1, c0, r0
lrp r7, c1.xxxx, r2, r3
cmp r8, neg(r4), r5, r6
dp2add r9.x, v0, c2, c30.yyyy
dp3_sat r9.y, v1, c3
dp4 r9.z, r7, c4
mul_pp r9.w, abs(r8.xxxx), absneg(r7.yyyy)
add r0, r0, r9
texkill v3
frc r10, r0
min r10, r10, c30.zzzz
max r10, r10, c30.xxxx
log r11.x, r10.xxxx
exp r11.y, r10.yyyy
rcp r11.z, r10.zzzz
rsq r11.w, r10.wwww
nrm r12.xyz, r11
pow r12.w, r11.xxxx, c30.yyyy
ifc_ne r12.wwww, c30.xxxx
mov r0.w, c30.zzzz
else
mov r0.w, c30.yyyy
endif
mov oC0, r0
