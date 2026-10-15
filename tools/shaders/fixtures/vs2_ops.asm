// Our own fixture: vs_2_0 arithmetic coverage, rep/defi, full a0 write, full oPos write.
vs_2_0
defi i0, 2, 0, 0, 0
def c90, 0.0, 1.0, 2.0, 0.5
dcl_position v0
dcl_normal v1
dcl_blendweight v2
dcl_tangent v3
mova a0, v2
mad r0, v0, c0, c1
add r1.xyz, r0, neg(c2)
dp3 r2.x, v1, v3
nrm r3, v1
rcp r2.y, r2.xxxx
rsq r2.z, r2.yyyy
max r4, r3, c90.xxxx
min r4, r4, c90.yyyy
slt r5, r4, c90.wwww
sge r6, r4, c90.wwww
frc r7, v0
abs r8, neg(r7)
exp r2.w, r2.xxxx
expp r9.x, r2.yyyy
lrp r10, r5, r6, r7
sincos r11.xy, r2.xxxx, c91, c92
lit r12, r4
pow r12.w, r2.xxxx, r2.yyyy
rep i0
add r1, r1, c[a0.x + 30]
endrep
mov oPos, r1
mov oD0.xyz, r12
mov oT0, r10
mov oT1, r11
mov oT2, r8
