// Our own fixture: ps_3_0 partial colour write and abs op.
ps_3_0
dcl_texcoord v0
abs r0, v0
mov oC0.xyz, r0
mov oC0.w, r0.xxxx
