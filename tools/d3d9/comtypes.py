"""D3D9 / D3DX COM interface layouts, written from the public d3d9.h / d3dx9shader.h headers.

Each interface is a list of (method name, number of stdcall arguments excluding `this`).
OUTPARAMS maps (interface, method) -> (0-based argument index, interface type written there),
for the methods that return a new interface pointer through an out parameter.
STRUCT_ARGS names the by-pointer struct arguments the boundary must convert.

Usage:   a table, imported by runtime/d3d9null/gen/gen_slots.py (which writes runtime/d3d9null/src/d3d9_slots.h)
When:    edit it only to correct a layout against the SDK headers, then run `make -C runtime/d3d9null gen`.
"""

IUNKNOWN = [("QueryInterface", 2), ("AddRef", 0), ("Release", 0)]
RESOURCE = IUNKNOWN + [("GetDevice", 1), ("SetPrivateData", 4), ("GetPrivateData", 3), ("FreePrivateData", 1),
                       ("SetPriority", 1), ("GetPriority", 0), ("PreLoad", 0), ("GetType", 0)]
BASETEX = RESOURCE + [("SetLOD", 1), ("GetLOD", 0), ("GetLevelCount", 0), ("SetAutoGenFilterType", 1),
                      ("GetAutoGenFilterType", 0), ("GenerateMipSubLevels", 0)]

IFACES = {
    "IDirect3D9": IUNKNOWN + [
        ("RegisterSoftwareDevice", 1), ("GetAdapterCount", 0), ("GetAdapterIdentifier", 3),
        ("GetAdapterModeCount", 2), ("EnumAdapterModes", 4), ("GetAdapterDisplayMode", 2),
        ("CheckDeviceType", 5), ("CheckDeviceFormat", 6), ("CheckDeviceMultiSampleType", 6),
        ("CheckDepthStencilMatch", 5), ("CheckDeviceFormatConversion", 4), ("GetDeviceCaps", 3),
        ("GetAdapterMonitor", 1), ("CreateDevice", 6)],
    "IDirect3DDevice9": IUNKNOWN + [
        ("TestCooperativeLevel", 0), ("GetAvailableTextureMem", 0), ("EvictManagedResources", 0),
        ("GetDirect3D", 1), ("GetDeviceCaps", 1), ("GetDisplayMode", 2), ("GetCreationParameters", 1),
        ("SetCursorProperties", 3), ("SetCursorPosition", 3), ("ShowCursor", 1),
        ("CreateAdditionalSwapChain", 2), ("GetSwapChain", 2), ("GetNumberOfSwapChains", 0), ("Reset", 1),
        ("Present", 4), ("GetBackBuffer", 4), ("GetRasterStatus", 2), ("SetDialogBoxMode", 1),
        ("SetGammaRamp", 3), ("GetGammaRamp", 2), ("CreateTexture", 8), ("CreateVolumeTexture", 9),
        ("CreateCubeTexture", 7), ("CreateVertexBuffer", 6), ("CreateIndexBuffer", 6),
        ("CreateRenderTarget", 8), ("CreateDepthStencilSurface", 8), ("UpdateSurface", 4),
        ("UpdateTexture", 2), ("GetRenderTargetData", 2), ("GetFrontBufferData", 2), ("StretchRect", 5),
        ("ColorFill", 3), ("CreateOffscreenPlainSurface", 6), ("SetRenderTarget", 2), ("GetRenderTarget", 2),
        ("SetDepthStencilSurface", 1), ("GetDepthStencilSurface", 1), ("BeginScene", 0), ("EndScene", 0),
        ("Clear", 6), ("SetTransform", 2), ("GetTransform", 2), ("MultiplyTransform", 2), ("SetViewport", 1),
        ("GetViewport", 1), ("SetMaterial", 1), ("GetMaterial", 1), ("SetLight", 2), ("GetLight", 2),
        ("LightEnable", 2), ("GetLightEnable", 2), ("SetClipPlane", 2), ("GetClipPlane", 2),
        ("SetRenderState", 2), ("GetRenderState", 2), ("CreateStateBlock", 2), ("BeginStateBlock", 0),
        ("EndStateBlock", 1), ("SetClipStatus", 1), ("GetClipStatus", 1), ("GetTexture", 2), ("SetTexture", 2),
        ("GetTextureStageState", 3), ("SetTextureStageState", 3), ("GetSamplerState", 3),
        ("SetSamplerState", 3), ("ValidateDevice", 1), ("SetPaletteEntries", 2), ("GetPaletteEntries", 2),
        ("SetCurrentTexturePalette", 1), ("GetCurrentTexturePalette", 1), ("SetScissorRect", 1),
        ("GetScissorRect", 1), ("SetSoftwareVertexProcessing", 1), ("GetSoftwareVertexProcessing", 0),
        ("SetNPatchMode", 1), ("GetNPatchMode", 0), ("DrawPrimitive", 3), ("DrawIndexedPrimitive", 6),
        ("DrawPrimitiveUP", 4), ("DrawIndexedPrimitiveUP", 8), ("ProcessVertices", 6),
        ("CreateVertexDeclaration", 2), ("SetVertexDeclaration", 1), ("GetVertexDeclaration", 1), ("SetFVF", 1),
        ("GetFVF", 1), ("CreateVertexShader", 2), ("SetVertexShader", 1), ("GetVertexShader", 1),
        ("SetVertexShaderConstantF", 3), ("GetVertexShaderConstantF", 3), ("SetVertexShaderConstantI", 3),
        ("GetVertexShaderConstantI", 3), ("SetVertexShaderConstantB", 3), ("GetVertexShaderConstantB", 3),
        ("SetStreamSource", 4), ("GetStreamSource", 4), ("SetStreamSourceFreq", 2), ("GetStreamSourceFreq", 2),
        ("SetIndices", 1), ("GetIndices", 1), ("CreatePixelShader", 2), ("SetPixelShader", 1),
        ("GetPixelShader", 1), ("SetPixelShaderConstantF", 3), ("GetPixelShaderConstantF", 3),
        ("SetPixelShaderConstantI", 3), ("GetPixelShaderConstantI", 3), ("SetPixelShaderConstantB", 3),
        ("GetPixelShaderConstantB", 3), ("DrawRectPatch", 3), ("DrawTriPatch", 3), ("DeletePatch", 1),
        ("CreateQuery", 2)],
    "IDirect3DSwapChain9": IUNKNOWN + [
        ("Present", 5), ("GetFrontBufferData", 1), ("GetBackBuffer", 3), ("GetRasterStatus", 1),
        ("GetDisplayMode", 1), ("GetDevice", 1), ("GetPresentParameters", 1)],
    "IDirect3DTexture9": BASETEX + [
        ("GetLevelDesc", 2), ("GetSurfaceLevel", 2), ("LockRect", 4), ("UnlockRect", 1), ("AddDirtyRect", 1)],
    "IDirect3DCubeTexture9": BASETEX + [
        ("GetLevelDesc", 2), ("GetCubeMapSurface", 3), ("LockRect", 5), ("UnlockRect", 2), ("AddDirtyRect", 2)],
    "IDirect3DVolumeTexture9": BASETEX + [
        ("GetLevelDesc", 2), ("GetVolumeLevel", 2), ("LockBox", 4), ("UnlockBox", 1), ("AddDirtyBox", 1)],
    "IDirect3DSurface9": RESOURCE + [
        ("GetContainer", 2), ("GetDesc", 1), ("LockRect", 3), ("UnlockRect", 0), ("GetDC", 1), ("ReleaseDC", 1)],
    "IDirect3DVolume9": IUNKNOWN + [
        ("GetDevice", 1), ("SetPrivateData", 4), ("GetPrivateData", 3), ("FreePrivateData", 1),
        ("GetContainer", 2), ("GetDesc", 1), ("LockBox", 3), ("UnlockBox", 0)],
    "IDirect3DVertexBuffer9": RESOURCE + [("Lock", 4), ("Unlock", 0), ("GetDesc", 1)],
    "IDirect3DIndexBuffer9": RESOURCE + [("Lock", 4), ("Unlock", 0), ("GetDesc", 1)],
    "IDirect3DVertexDeclaration9": IUNKNOWN + [("GetDevice", 1), ("GetDeclaration", 2)],
    "IDirect3DVertexShader9": IUNKNOWN + [("GetDevice", 1), ("GetFunction", 2)],
    "IDirect3DPixelShader9": IUNKNOWN + [("GetDevice", 1), ("GetFunction", 2)],
    "IDirect3DQuery9": IUNKNOWN + [
        ("GetDevice", 1), ("GetType", 0), ("GetDataSize", 0), ("Issue", 1), ("GetData", 3)],
    "IDirect3DStateBlock9": IUNKNOWN + [("GetDevice", 1), ("Capture", 0), ("Apply", 0)],
    "ID3DXBuffer": IUNKNOWN + [("GetBufferPointer", 0), ("GetBufferSize", 0)],
    "ID3DXConstantTable": IUNKNOWN + [
        ("GetBufferPointer", 0), ("GetBufferSize", 0), ("GetDesc", 1), ("GetConstantDesc", 3),
        ("GetSamplerIndex", 1), ("GetConstant", 2), ("GetConstantByName", 2), ("GetConstantElement", 2),
        ("SetDefaults", 1), ("SetValue", 4), ("SetBool", 3), ("SetBoolArray", 4), ("SetInt", 3),
        ("SetIntArray", 4), ("SetFloat", 3), ("SetFloatArray", 4), ("SetVector", 3), ("SetVectorArray", 4),
        ("SetMatrix", 3), ("SetMatrixArray", 4), ("SetMatrixPointerArray", 4), ("SetMatrixTranspose", 3),
        ("SetMatrixTransposeArray", 4), ("SetMatrixTransposePointerArray", 4)],
}

SHORT = {"IDirect3D9": "D3D9", "IDirect3DDevice9": "Device", "IDirect3DSwapChain9": "SwapChain",
         "IDirect3DTexture9": "Texture", "IDirect3DCubeTexture9": "CubeTexture",
         "IDirect3DVolumeTexture9": "VolumeTexture", "IDirect3DSurface9": "Surface",
         "IDirect3DVolume9": "Volume", "IDirect3DVertexBuffer9": "VertexBuffer",
         "IDirect3DIndexBuffer9": "IndexBuffer", "IDirect3DVertexDeclaration9": "VertexDeclaration",
         "IDirect3DVertexShader9": "VertexShader", "IDirect3DPixelShader9": "PixelShader",
         "IDirect3DQuery9": "Query", "IDirect3DStateBlock9": "StateBlock", "ID3DXBuffer": "D3DXBuffer",
         "ID3DXConstantTable": "D3DXConstantTable"}

OUTPARAMS = {
    ("IDirect3D9", "CreateDevice"): (5, "IDirect3DDevice9"),
    ("IDirect3DDevice9", "GetDirect3D"): (0, "IDirect3D9"),
    ("IDirect3DDevice9", "CreateAdditionalSwapChain"): (1, "IDirect3DSwapChain9"),
    ("IDirect3DDevice9", "GetSwapChain"): (1, "IDirect3DSwapChain9"),
    ("IDirect3DDevice9", "GetBackBuffer"): (3, "IDirect3DSurface9"),
    ("IDirect3DDevice9", "CreateTexture"): (6, "IDirect3DTexture9"),
    ("IDirect3DDevice9", "CreateVolumeTexture"): (7, "IDirect3DVolumeTexture9"),
    ("IDirect3DDevice9", "CreateCubeTexture"): (5, "IDirect3DCubeTexture9"),
    ("IDirect3DDevice9", "CreateVertexBuffer"): (4, "IDirect3DVertexBuffer9"),
    ("IDirect3DDevice9", "CreateIndexBuffer"): (4, "IDirect3DIndexBuffer9"),
    ("IDirect3DDevice9", "CreateRenderTarget"): (6, "IDirect3DSurface9"),
    ("IDirect3DDevice9", "CreateDepthStencilSurface"): (6, "IDirect3DSurface9"),
    ("IDirect3DDevice9", "CreateOffscreenPlainSurface"): (4, "IDirect3DSurface9"),
    ("IDirect3DDevice9", "GetRenderTarget"): (1, "IDirect3DSurface9"),
    ("IDirect3DDevice9", "GetDepthStencilSurface"): (0, "IDirect3DSurface9"),
    ("IDirect3DDevice9", "CreateStateBlock"): (1, "IDirect3DStateBlock9"),
    ("IDirect3DDevice9", "EndStateBlock"): (0, "IDirect3DStateBlock9"),
    ("IDirect3DDevice9", "CreateVertexDeclaration"): (1, "IDirect3DVertexDeclaration9"),
    ("IDirect3DDevice9", "CreateVertexShader"): (1, "IDirect3DVertexShader9"),
    ("IDirect3DDevice9", "CreatePixelShader"): (1, "IDirect3DPixelShader9"),
    ("IDirect3DDevice9", "CreateQuery"): (1, "IDirect3DQuery9"),
    ("IDirect3DDevice9", "GetTexture"): (1, "IDirect3DBaseTexture9"),
    ("IDirect3DDevice9", "GetVertexShader"): (0, "IDirect3DVertexShader9"),
    ("IDirect3DDevice9", "GetPixelShader"): (0, "IDirect3DPixelShader9"),
    ("IDirect3DDevice9", "GetIndices"): (0, "IDirect3DIndexBuffer9"),
    ("IDirect3DDevice9", "GetVertexDeclaration"): (0, "IDirect3DVertexDeclaration9"),
    ("IDirect3DSwapChain9", "GetBackBuffer"): (2, "IDirect3DSurface9"),
    ("IDirect3DTexture9", "GetSurfaceLevel"): (1, "IDirect3DSurface9"),
    ("IDirect3DCubeTexture9", "GetCubeMapSurface"): (2, "IDirect3DSurface9"),
    ("IDirect3DVolumeTexture9", "GetVolumeLevel"): (1, "IDirect3DVolume9"),
}
# IDirect3DBaseTexture9 is the common prefix of the three texture interfaces.
IFACES["IDirect3DBaseTexture9"] = BASETEX
SHORT["IDirect3DBaseTexture9"] = "BaseTexture"

# Imports / plain functions that produce interfaces through out parameters: name -> {arg index: iface}
IMPORT_OUT = {
    "d3dx9_27.dll!D3DXCompileShader": {7: "ID3DXBuffer", 8: "ID3DXBuffer", 9: "ID3DXConstantTable"},
}
# stdcall argument bytes of the imports the driver region calls (None = cdecl, caller cleans)
IMPORT_ARGS = {
    "d3dx9_27.dll!D3DXCompileShader": 10, "d3dx9_27.dll!D3DXGetPixelShaderProfile": 1,
    "d3dx9_27.dll!D3DXGetVertexShaderProfile": 1, "d3dx9_27.dll!D3DXMatrixMultiplyTranspose": 3,
    "d3dx9_27.dll!D3DXMatrixTranspose": 2, "d3dx9_27.dll!D3DXMatrixMultiply": 3,
    "d3dx9_27.dll!D3DXCheckVersion": 2, "d3dx9_27.dll!D3DXVec3Transform": 3,
    "d3dx9_27.dll!D3DXVec3TransformNormal": 3, "d3d9.dll!Direct3DCreate9": 1,
    "user32.dll!GetClientRect": 2, "kernel32.dll!QueryPerformanceCounter": 1,
    "kernel32.dll!QueryPerformanceFrequency": 1, "kernel32.dll!GetVersionExA": 1,
    "kernel32.dll!VirtualAlloc": 4, "kernel32.dll!VirtualFree": 3,
}


def slot_name(iface, off):
    t = IFACES.get(iface)
    if t is None or off % 4 or off // 4 >= len(t):
        return None
    return t[off // 4][0]


def nargs(iface, off):
    t = IFACES.get(iface)
    if t is None or off % 4 or off // 4 >= len(t):
        return None
    return t[off // 4][1]


def check():
    assert len(IFACES["IDirect3DDevice9"]) == 119, len(IFACES["IDirect3DDevice9"])
    assert len(IFACES["IDirect3D9"]) == 17
    assert len(IFACES["IDirect3DTexture9"]) == 22
    assert len(IFACES["IDirect3DSurface9"]) == 17
    assert len(IFACES["IDirect3DVertexBuffer9"]) == 14
    assert IFACES["IDirect3DDevice9"][0x1d8 // 4][0] == "CreateQuery"
    assert IFACES["IDirect3DDevice9"][0x178 // 4][0] == "SetVertexShaderConstantF"
    assert IFACES["IDirect3DTexture9"][0x4c // 4][0] == "LockRect"
    assert IFACES["IDirect3DSurface9"][0x34 // 4][0] == "LockRect"
    assert IFACES["IDirect3DQuery9"][0x18 // 4][0] == "Issue"
    import sys, os
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "graphics"))
    from d3d9names import DEVICE, D3D9
    assert [n for n, _ in IFACES["IDirect3DDevice9"]] == DEVICE
    assert [n for n, _ in IFACES["IDirect3D9"]] == D3D9


if __name__ == "__main__":
    check()
    print("ok", {k: len(v) for k, v in IFACES.items()})
