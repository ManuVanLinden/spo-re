// Our own fixture: vs_3_0 tangent input, absneg modifier, partial a0 write.
vs_3_0
dcl_position v0
dcl_tangent v1
dcl_position o0
dcl_texcoord o1
mova a0.x, v1.xxxx
add r0, v0, absneg(v1)
mov o0, r0
mov o1, c[a0.x + 4]
