// Our own shader: SM3 features. vPos, rep loop on an integer constant, ifc/else, frc.
// R = 4 * 1/16 = 0.25; G = 1 if frac(x/16) < 0.5 else 0; B = frac(y/16); A = 1.
// c1 = (1/16, 1/16, 0, 0)
ps_3_0
defi i0, 4, 0, 0, 0
def c0, 0.0, 0.0625, 1.0, 0.5
dcl vPos.xy
dcl_texcoord v0
mov r0, c0.xxxx
rep i0
add r0.x, r0.x, c0.y
endrep
mul r1.xy, vPos.xy, c1.xy
frc r1.xy, r1.xy
ifc_lt r1.xxxx, c0.wwww
mov r0.y, c0.z
else
mov r0.y, c0.x
endif
mov r0.z, r1.y
mov r0.w, c0.z
mov oC0, r0
