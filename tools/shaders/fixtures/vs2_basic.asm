// Our own fixture: vs_2_0 with oPos/oT/oD/oFog outputs and a0 relative addressing.
vs_2_0
def c95, 1.0, 0.0, 0.5, 3.0
dcl_position v0
dcl_texcoord v1
dcl_color v2
dcl_blendindices v3
mul r0, v3, c95.wwww
mova a0.x, r0.xxxx
dp4 oPos.x, v0, c[a0.x + 20]
dp4 oPos.y, v0, c[a0.x + 21]
dp4 oPos.z, v0, c[a0.x + 22]
dp4 oPos.w, v0, c[a0.x + 23]
mov oT0, v1
mov oT1.xy, v1
mov oD0, v2
mov oD1, c95.yyyy
mov oFog, c95.xxxx
