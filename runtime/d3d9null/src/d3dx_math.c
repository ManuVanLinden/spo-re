/* d3dx_math.c -- the D3DX 9 (d3dx9_27) math helpers the exe imports, and D3DXCompileShader as an
 * honest failure. Written from the documented D3DX
 * contracts (Microsoft Learn): row-major 4x4 float matrices, row vectors, every function returns
 * its output pointer and accepts output == input.
 *
 * Precision: D3DX computes in single precision; each element here is the float sum of four float
 * products in index order (k = 0..3). D3DX's SSE paths may round some sums differently by one ulp
 * [inf]; nothing in the null device depends on that. */
#include <stdio.h>
#include <string.h>

#include "d3d9_int.h"

static void ldm(const uint8_t *mem, uint32_t a, float m[16]) { memcpy(m, mem + a, 64); }
static void stm(uint8_t *mem, uint32_t a, const float m[16]) { memcpy(mem + a, m, 64); }
static void mul(const float *a, const float *b, float *o) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = a[i * 4 + 0] * b[0 * 4 + j];
            s += a[i * 4 + 1] * b[1 * 4 + j];
            s += a[i * 4 + 2] * b[2 * 4 + j];
            s += a[i * 4 + 3] * b[3 * 4 + j];
            o[i * 4 + j] = s;
        }
}
static void transpose(const float *a, float *o) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) o[j * 4 + i] = a[i * 4 + j];
}

/* D3DXMATRIX *D3DXMatrixMultiply(D3DXMATRIX *pOut, const D3DXMATRIX *pM1, const D3DXMATRIX *pM2) */
SG_IMPORT(d3dx9_27, D3DXMatrixMultiply) {
    float a[16], b[16], o[16];
    ldm(mem, SG_ARG(1), a), ldm(mem, SG_ARG(2), b);
    mul(a, b, o);
    stm(mem, SG_ARG(0), o);
    SG_RETURN_STDCALL(SG_ARG(0), 12);
}
/* D3DXMatrixMultiplyTranspose: transpose(pM1 * pM2) */
SG_IMPORT(d3dx9_27, D3DXMatrixMultiplyTranspose) {
    float a[16], b[16], o[16], t[16];
    ldm(mem, SG_ARG(1), a), ldm(mem, SG_ARG(2), b);
    mul(a, b, o);
    transpose(o, t);
    stm(mem, SG_ARG(0), t);
    SG_RETURN_STDCALL(SG_ARG(0), 12);
}
/* D3DXMATRIX *D3DXMatrixTranspose(D3DXMATRIX *pOut, const D3DXMATRIX *pM) */
SG_IMPORT(d3dx9_27, D3DXMatrixTranspose) {
    float a[16], t[16];
    ldm(mem, SG_ARG(1), a);
    transpose(a, t);
    stm(mem, SG_ARG(0), t);
    SG_RETURN_STDCALL(SG_ARG(0), 8);
}
/* D3DXVECTOR4 *D3DXVec3Transform(D3DXVECTOR4 *pOut, const D3DXVECTOR3 *pV, const D3DXMATRIX *pM): (x,y,z,1) * M */
SG_IMPORT(d3dx9_27, D3DXVec3Transform) {
    float v[3], m[16], o[4];
    memcpy(v, mem + SG_ARG(1), 12);
    ldm(mem, SG_ARG(2), m);
    for (int j = 0; j < 4; j++) {
        float s = v[0] * m[0 * 4 + j];
        s += v[1] * m[1 * 4 + j];
        s += v[2] * m[2 * 4 + j];
        s += m[3 * 4 + j];
        o[j] = s;
    }
    memcpy(mem + SG_ARG(0), o, 16);
    SG_RETURN_STDCALL(SG_ARG(0), 12);
}
/* D3DXVECTOR3 *D3DXVec3TransformNormal(D3DXVECTOR3 *pOut, const D3DXVECTOR3 *pV, const D3DXMATRIX *pM): (x,y,z,0) * M */
SG_IMPORT(d3dx9_27, D3DXVec3TransformNormal) {
    float v[3], m[16], o[3];
    memcpy(v, mem + SG_ARG(1), 12);
    ldm(mem, SG_ARG(2), m);
    for (int j = 0; j < 3; j++) {
        float s = v[0] * m[0 * 4 + j];
        s += v[1] * m[1 * 4 + j];
        s += v[2] * m[2 * 4 + j];
        o[j] = s;
    }
    memcpy(mem + SG_ARG(0), o, 12);
    SG_RETURN_STDCALL(SG_ARG(0), 12);
}

/* HRESULT D3DXCompileShader(LPCSTR pSrcData, UINT srcDataLen, const D3DXMACRO *pDefines, LPD3DXINCLUDE
 *     pInclude, LPCSTR pFunctionName, LPCSTR pProfile, DWORD Flags, LPD3DXBUFFER *ppShader,
 *     LPD3DXBUFFER *ppErrorMsgs, LPD3DXCONSTANTTABLE *ppConstantTable)
 * There is no HLSL compiler in the null device: fail with E_FAIL and an error-message buffer, as
 * D3DX does for source it cannot compile. Count the calls for the run report. */
uint64_t d3dx_compile_calls;
SG_IMPORT(d3dx9_27, D3DXCompileShader) {
    static const char msg[] = "spore-native: no HLSL compiler in the null Direct3D 9 device";
    char prof[32] = "", fn[64] = "";
    uint32_t data = 0, buf;
    if (SG_ARG(5)) snprintf(prof, sizeof prof, "%s", (const char *)mem + SG_ARG(5));
    if (SG_ARG(4)) snprintf(fn, sizeof fn, "%s", (const char *)mem + SG_ARG(4));
    if (__atomic_fetch_add(&d3dx_compile_calls, 1, __ATOMIC_RELAXED) < 8 || sg_com_trace >= 1)
        fprintf(stderr, "d3d9: D3DXCompileShader(%u bytes, entry \"%s\", profile \"%s\", flags 0x%x) = E_FAIL (no compiler)\n",
                SG_ARG(1), fn, prof, SG_ARG(6));
    if (SG_ARG(7)) sg_st32(mem, SG_ARG(7), 0);
    if (SG_ARG(9)) sg_st32(mem, SG_ARG(9), 0);
    if (SG_ARG(8)) {
        sg_com_init(mem);
        buf = d3d9null_d3dx_buffer(sizeof msg, &data);
        if (buf && data) memcpy(mem + data, msg, sizeof msg);
        sg_st32(mem, SG_ARG(8), buf);
    }
    SG_RETURN_STDCALL(0x80004005u, 40);
}
