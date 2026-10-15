// Our own fixture: vs_3_0 loop/aL, rep, if/ifc/else, break, defi, lit, m4x4, crs, sgn, dst, expp.
vs_3_0
defi i0, 4, 20, 2, 0
defi i1, 3, 0, 0, 0
def c210, 0.0, 1.0, 2.0, 0.25
dcl_position v0
dcl_normal v1
dcl_position o0
dcl_texcoord o1
dcl_texcoord2 o2
dcl_fog o3.x
dcl_psize o4.x
mov r0, c210.xxxx
loop aL, i0
add r0, r0, c[aL.x + 0]
breakc_gt r0.xxxx, c210.zzzz
endloop
rep i1
mad r0, r0, c210.wwww, v1
endrep
if b0
mov r1, c9
else
mov r1, neg(c9)
endif
ifc_lt r0.xxxx, c210.yyyy
add r1, r1, c210.yyyy
endif
m4x4 o0, v0, c0
lit r2, r1
crs r3.xyz, v1, r1
sgn r4, r1, r5, r6
dst r5, r2, r3
expp r6.x, r1.xxxx
add o1, r2, r0
add o2, r3, r4
mov o3.x, r6.xxxx
mov o4.x, c210.yyyy
