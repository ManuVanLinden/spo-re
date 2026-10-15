// Our own fixture: ps_2_0 with t#/v# inputs, cube sampling, texkill, partial precision.
ps_2_0
def c0, 1.0, 0.5, 0.0, 2.0
dcl t0
dcl t1.xyz
dcl_pp v0
dcl_2d s0
dcl_cube s1
texld_pp r0, t0, s0
texld r1, t1, s1
texldp r2, t0, s0
mul_pp r0, r0, v0
mad_sat_pp r0, r1, c0.yyyy, r0
add r0, r0, r2
texkill t0
mov oC0, r0
