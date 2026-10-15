// Our own shader (conformance sampling cases): output a cube-map sample from s0 in the direction of texcoord0.xyz.
ps_3_0
dcl_texcoord v0
dcl_cube s0
texld r0, v0, s0
mov oC0, r0
