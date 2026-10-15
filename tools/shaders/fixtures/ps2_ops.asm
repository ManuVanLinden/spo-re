// Our own fixture: ps_2_0 arithmetic coverage with partial writes.
ps_2_0
def c5, 0.0, 1.0, 2.0, 0.5
dcl t0
dcl v0
dcl_2d s0
texld r0, t0, s0
lrp r1.xyz, c0, r0, v0
dp3 r2.x, r1, c1
dp4 r2.y, r0, c2
abs r3, neg(r0)
nrm r4.xyz, r3
pow r2.z, r2.xxxx, c5.zzzz
cmp r5, r1, r0, c5.xxxx
rsq r2.w, r2.xxxx
frc r6, t0
rcp r6.x, r2.yyyy
min r7, r6, c5.yyyy
max r7, r7, c5.xxxx
dp2add r8.x, t0, c3, c5.wwww
exp r8.y, r2.xxxx
log r8.z, r2.yyyy
add r0.xyz, r0, r8
mov oC0, r0
