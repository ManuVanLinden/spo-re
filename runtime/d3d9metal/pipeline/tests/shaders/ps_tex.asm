// Our own shader (pipeline test): texture s0 at texcoord0.
ps_3_0
dcl_texcoord v0
dcl_2d s0
texld r0, v0, s0
mov oC0, r0
