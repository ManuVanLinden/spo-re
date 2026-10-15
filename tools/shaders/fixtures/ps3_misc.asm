// Our own fixture: ps_3_0 vPos, vFace, derivatives, MRT, oDepth.
ps_3_0
def c31, 0.0, 0.5, 1.0, 2.0
dcl vPos.xy
dcl vFace
dcl_texcoord v0
dcl_2d s0
dsx r0, v0
dsy r1, v0
add r2, r0, r1
mul r3, vPos.xyxy, c0
cmp r4, vFace, c31.zzzz, c31.xxxx
texld r5, v0, s0
mad oC0, r5, r4, r2
mov oC1, r3
mov oDepth, c31.yyyy
