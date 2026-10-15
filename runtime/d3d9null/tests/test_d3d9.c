/* test_d3d9.c -- host-only tests of the null Direct3D 9 through its guest vtables. */
#include <stdlib.h>

#include "com_harness.h"
#include "../src/d3d9_int.h"

void hle_stop_check(sg_ctx *ctx, uint8_t *mem, const char *import) { (void)ctx; (void)mem; (void)import; }
extern void sg_imp_d3d9_Direct3DCreate9(sg_ctx *ctx, uint8_t *mem);
extern void sg_imp_d3dx9_27_D3DXCheckVersion(sg_ctx *ctx, uint8_t *mem);
extern void sg_imp_d3dx9_27_D3DXGetPixelShaderProfile(sg_ctx *ctx, uint8_t *mem);
static uint32_t imp2(void (*fn)(sg_ctx *, uint8_t *), uint32_t a0, uint32_t a1, unsigned n) {
    sg_ctx *ctx = &T;
    uint8_t *mem = M;
    uint32_t esp0 = T.esp;
    if (n > 1) SG_PUSH32(a1);
    SG_PUSH32(a0);
    SG_CALL(fn, 0x011f8507u);
    CHECK_EQ(T.esp, esp0);
    return T.eax;
}

static uint32_t create9(uint32_t sdk) { /* push 32; call [IAT Direct3DCreate9] */
    sg_ctx *ctx = &T;
    uint8_t *mem = M;
    uint32_t esp0 = T.esp;
    SG_PUSH32(sdk);
    SG_CALL(sg_imp_d3d9_Direct3DCreate9, 0x011f84f3u);
    CHECK_EQ(T.esp, esp0);
    return T.eax;
}
static uint32_t used_handled(void) { /* every (interface, method) the game calls has its own handler */
    sg_com_iface *ifs[] = {&d3d9_iface_d3d9, &d3d9_iface_device, &d3d9_iface_swapchain, &d3d9_iface_texture,
                           &d3d9_iface_cube, &d3d9_iface_volumetex, &d3d9_iface_surface, &d3d9_iface_vb, &d3d9_iface_ib,
                           &d3d9_iface_vdecl, &d3d9_iface_vs, &d3d9_iface_ps, &d3d9_iface_query, &d3d9_iface_d3dxbuf};
    unsigned ok = 0;
    for (unsigned u = 0; u < D3D9_USED_METHODS; u++) {
        int found = 0;
        for (unsigned i = 0; i < sizeof ifs / sizeof ifs[0]; i++) {
            if (strcmp(ifs[i]->name, d3d9_used[u].iface)) continue;
            for (unsigned k = 0; k < ifs[i]->nmethods; k++)
                if (ifs[i]->methods[k].slot == d3d9_used[u].slot && !strcmp(ifs[i]->methods[k].name, d3d9_used[u].method)) found = 1;
        }
        if (found) ok++;
        else fprintf(stderr, "no handler: %s::%s\n", d3d9_used[u].iface, d3d9_used[u].method);
    }
    return ok;
}

int main(void) {
    harness_init();
    unsigned live0 = sg_com_live_objects();
    CHECK_EQ(used_handled(), 103);
    CHECK_EQ(D3D9_USED_METHODS, 103);

    CHECK_EQ(imp2(sg_imp_d3dx9_27_D3DXCheckVersion, 32, 27, 2), 1);
    CHECK_EQ(imp2(sg_imp_d3dx9_27_D3DXCheckVersion, 32, 26, 2), 0);
    uint32_t prof = imp2(sg_imp_d3dx9_27_D3DXGetPixelShaderProfile, 0x1000, 0, 1);
    CHECK(prof && !strcmp((char *)M + prof, "ps_3_0"));
    /* ---- IDirect3D9 ---- */
    uint32_t d3d = create9(32);
    CHECK(d3d != 0);
    CHECK_EQ(gcall(d3d, S_D3D9_GetAdapterCount, 0), 1);
    uint32_t id = gscratch(D3DADAPTER_IDENTIFIER9_SIZE);
    CHECK_EQ(gcall(d3d, S_D3D9_GetAdapterIdentifier, 3, 0, 0, id), D3D_OK);
    CHECK_EQ(gld(id + 1064), 0x10de);
    CHECK_EQ(gld(id + 1068), 0x0191);
    uint32_t lo = gld(id + 1056);
    CHECK_EQ((lo >> 16) * 10000 + (lo & 0xffff), 117741); /* driverBuild as the config filler 0x7cb640 computes it */
    CHECK(!strcmp((char *)M + id + 512, "NVIDIA GeForce 8800 GTX"));
    CHECK_EQ(gcall(d3d, S_D3D9_GetAdapterIdentifier, 3, 1, 0, id), D3DERR_INVALIDCALL);
    uint32_t caps = gscratch(D3DCAPS9_SIZE);
    CHECK_EQ(gcall(d3d, S_D3D9_GetDeviceCaps, 3, 0, D3DDEVTYPE_HAL, caps), D3D_OK);
    CHECK_EQ(gld(caps + 0x1c) & 0x00100000, 0);         /* DevCaps: no PUREDEVICE */
    CHECK(gld(caps + 0x1c) & 0x00010000);                /* HWTRANSFORMANDLIGHT */
    CHECK_EQ(gld(caps + 0xcc) & 0xffff, 0x0300);         /* PixelShaderVersion 3.0 */
    CHECK_EQ(gld(caps + 0xc4) & 0xffff, 0x0300);
    CHECK_EQ(gld(caps + 0xc8), 256);                     /* MaxVertexShaderConst */
    CHECK(gld(caps + 0xc) & 0x40000000);                 /* Caps2 CANAUTOGENMIPMAP */
    CHECK(gld(caps + 0xd4) & 1);                         /* DevCaps2 STREAMOFFSET */
    CHECK_EQ(gcall(d3d, S_D3D9_GetDeviceCaps, 3, 0, D3DDEVTYPE_REF, caps), D3DERR_NOTAVAILABLE);
    uint32_t n = gcall(d3d, S_D3D9_GetAdapterModeCount, 2, 0, D3DFMT_X8R8G8B8);
    CHECK(n >= 5);
    CHECK_EQ(gcall(d3d, S_D3D9_GetAdapterModeCount, 2, 0, D3DFMT_R5G6B5), 0);
    uint32_t mode = gscratch(16), cur = gscratch(16);
    CHECK_EQ(gcall(d3d, S_D3D9_GetAdapterDisplayMode, 2, 0, cur), D3D_OK);
    CHECK_EQ(gld(cur + 12), D3DFMT_X8R8G8B8);
    int saw_desktop = 0, sorted = 1;
    uint32_t pw = 0, ph = 0;
    for (uint32_t i = 0; i < n; i++) {
        CHECK_EQ(gcall(d3d, S_D3D9_EnumAdapterModes, 4, 0, D3DFMT_X8R8G8B8, i, mode), D3D_OK);
        if (gld(mode) == gld(cur) && gld(mode + 4) == gld(cur + 4)) saw_desktop = 1;
        if (gld(mode) < pw || (gld(mode) == pw && gld(mode + 4) <= ph)) sorted = 0;
        pw = gld(mode), ph = gld(mode + 4);
    }
    CHECK(saw_desktop);
    CHECK(sorted);
    CHECK_EQ(gcall(d3d, S_D3D9_EnumAdapterModes, 4, 0, D3DFMT_X8R8G8B8, n, mode), D3DERR_INVALIDCALL);
    /* formats: the INST probe at 0x75dd74 must fail; the common ones succeed */
    CHECK_EQ(gcall(d3d, S_D3D9_CheckDeviceFormat, 6, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_SURFACE, D3DFMT_INST), D3DERR_NOTAVAILABLE);
    CHECK_EQ(gcall(d3d, S_D3D9_CheckDeviceFormat, 6, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_DXT5), D3D_OK);
    CHECK_EQ(gcall(d3d, S_D3D9_CheckDeviceFormat, 6, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F), D3D_OK);
    CHECK_EQ(gcall(d3d, S_D3D9_CheckDeviceFormat, 6, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, D3DFMT_D24S8), D3D_OK);
    CHECK_EQ(gcall(d3d, S_D3D9_CheckDeviceFormat, 6, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_P8), D3DERR_NOTAVAILABLE);
    CHECK_EQ(gcall(d3d, S_D3D9_CheckDeviceFormat, 6, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_AUTOGENMIPMAP, D3DRTYPE_TEXTURE, D3DFMT_DXT1), D3DOK_NOAUTOGEN);
    CHECK_EQ(gcall(d3d, S_D3D9_CheckDepthStencilMatch, 5, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_D24S8), D3D_OK);
    CHECK_EQ(gcall(d3d, S_D3D9_GetAdapterMonitor, 1, 0), D3D9NULL_HMONITOR);
    CHECK_EQ(gcall(d3d, S_D3D9_RegisterSoftwareDevice, 1, 0), D3DERR_NOTAVAILABLE);

    /* ---- CreateDevice with the game's present parameters (0x11f7d30), windowed 1024x768 ---- */
    uint32_t pp = gscratch(56), ppdev = gscratch(4);
    uint32_t ppv[14] = {1024, 768, D3DFMT_A8R8G8B8, 1, 0, 0, 3 /* COPY */, 0x00c0ffee, 1, 1, D3DFMT_D24S8, 0, 0, 0x80000000u};
    for (int i = 0; i < 14; i++) gst(pp + 4 * i, ppv[i]);
    /* vertex-processing flags: none, or two of SOFTWARE 0x20 / HARDWARE 0x40 / MIXED 0x80, are invalid */
    CHECK_EQ(gcall(d3d, S_D3D9_CreateDevice, 6, 0, D3DDEVTYPE_HAL, 0x00c0ffee, 0x0, pp, ppdev), D3DERR_INVALIDCALL);
    CHECK_EQ(gcall(d3d, S_D3D9_CreateDevice, 6, 0, D3DDEVTYPE_HAL, 0x00c0ffee, 0xc0, pp, ppdev), D3DERR_INVALIDCALL);
    T.fpu.cw = 0x027f;
    CHECK_EQ(gcall(d3d, S_D3D9_CreateDevice, 6, 0, D3DDEVTYPE_HAL, 0x00c0ffee, 0x40, pp, ppdev), D3D_OK);
    uint32_t dev = gld(ppdev);
    CHECK(dev != 0);
    CHECK_EQ(T.fpu.cw, 0x007f);                          /* 24-bit precision, as D3D9 without FPU_PRESERVE */
    CHECK_EQ(gcall(dev, S_Device_TestCooperativeLevel, 0), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_GetAvailableTextureMem, 0) >> 20, 768);
    uint32_t out = gscratch(4), out2 = gscratch(4), out3 = gscratch(4);
    CHECK_EQ(gcall(dev, S_Device_GetDirect3D, 1, out), D3D_OK);
    CHECK_EQ(gld(out), d3d);
    gcall(d3d, 2, 0); /* release the GetDirect3D reference */
    uint32_t cp = gscratch(16);
    CHECK_EQ(gcall(dev, S_Device_GetCreationParameters, 1, cp), D3D_OK);
    CHECK_EQ(gld(cp + 12), 0x40);
    CHECK_EQ(gcall(dev, S_Device_GetDeviceCaps, 1, caps), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_GetDisplayMode, 2, 0, mode), D3D_OK);
    /* back buffer and depth surfaces, as the attach step 0x11f9280 gets them */
    CHECK_EQ(gcall(dev, S_Device_GetRenderTarget, 2, 0, out), D3D_OK);
    uint32_t bb = gld(out), desc = gscratch(32);
    CHECK_EQ(gcall(bb, S_Surface_GetDesc, 1, desc), D3D_OK);
    CHECK_EQ(gld(desc), D3DFMT_A8R8G8B8);
    CHECK_EQ(gld(desc + 4), D3DRTYPE_SURFACE);
    CHECK_EQ(gld(desc + 24), 1024);
    CHECK_EQ(gld(desc + 28), 768);
    CHECK_EQ(gcall(dev, S_Device_GetDepthStencilSurface, 1, out2), D3D_OK);
    uint32_t ds = gld(out2);
    CHECK_EQ(gcall(ds, S_Surface_GetDesc, 1, desc), D3D_OK);
    CHECK_EQ(gld(desc), D3DFMT_D24S8);
    CHECK_EQ(gcall(dev, S_Device_GetBackBuffer, 4, 0, 0, 0, out3), D3D_OK);
    CHECK_EQ(gld(out3), bb);
    gcall(bb, 2, 0); gcall(bb, 2, 0); gcall(ds, 2, 0);
    CHECK_EQ(gcall(dev, S_Device_GetRenderTarget, 2, 1, out), D3DERR_NOTFOUND);
    CHECK_EQ(gld(out), 0);

    /* ---- textures: sizes, pitches, levels, lock memory ---- */
    CHECK_EQ(gcall(dev, S_Device_CreateTexture, 8, 256, 128, 0, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, out, 0), D3D_OK);
    uint32_t tex = gld(out);
    CHECK_EQ(gcall(tex, S_Texture_GetLevelCount, 0), 9);
    CHECK_EQ(gcall(tex, S_Texture_GetType, 0), D3DRTYPE_TEXTURE);
    uint32_t lr = gscratch(8);
    CHECK_EQ(gcall(tex, S_Texture_LockRect, 4, 0, lr, 0, 0), D3D_OK);
    CHECK_EQ(gld(lr), 512);                              /* 64 blocks * 8 bytes */
    uint32_t bits0 = gld(lr + 4);
    CHECK(bits0 >= 0x20000000u && !(bits0 & 15));
    memset(M + bits0, 0xab, 512 * 32);                   /* the whole level 0 is writable guest memory */
    CHECK_EQ(gcall(tex, S_Texture_UnlockRect, 1, 0), D3D_OK);
    CHECK_EQ(gcall(tex, S_Texture_LockRect, 4, 8, lr, 0, 0), D3D_OK); /* 1x1 level: one block */
    CHECK_EQ(gld(lr), 8);
    CHECK_EQ(gcall(tex, S_Texture_LockRect, 4, 9, lr, 0, 0), D3DERR_INVALIDCALL);
    CHECK_EQ(gcall(tex, S_Texture_GetLevelDesc, 2, 3, desc), D3D_OK);
    CHECK_EQ(gld(desc + 24), 32);
    CHECK_EQ(gld(desc + 28), 16);
    CHECK_EQ(gld(desc + 12), D3DPOOL_MANAGED);
    uint32_t rect = gscratch(16);
    gst(rect, 8); gst(rect + 4, 4); gst(rect + 8, 16); gst(rect + 12, 8);
    CHECK_EQ(gcall(tex, S_Texture_LockRect, 4, 0, lr, rect, 0), D3D_OK);
    CHECK_EQ(gld(lr + 4), bits0 + 1 * 512 + 2 * 8);      /* block row 1, block column 2 */
    /* surface levels share the texture's memory and count on the texture */
    CHECK_EQ(gcall(tex, S_Texture_GetSurfaceLevel, 2, 1, out), D3D_OK);
    uint32_t lvl1 = gld(out);
    CHECK_EQ(gcall(lvl1, S_Surface_GetDesc, 1, desc), D3D_OK);
    CHECK_EQ(gld(desc + 24), 128);
    CHECK_EQ(gld(desc + 28), 64);
    CHECK_EQ(gcall(lvl1, S_Surface_LockRect, 3, lr, 0, D3DLOCK_READONLY), D3D_OK);
    CHECK_EQ(gld(lr), 256);
    CHECK(gld(lr + 4) > bits0);
    uint32_t iid = gscratch(16);
    memcpy(M + iid, &d3d9_iface_texture.iid.d1, 4); memcpy(M + iid + 4, &d3d9_iface_texture.iid.d2, 2);
    memcpy(M + iid + 6, &d3d9_iface_texture.iid.d3, 2); memcpy(M + iid + 8, d3d9_iface_texture.iid.d4, 8);
    CHECK_EQ(gcall(lvl1, S_Surface_GetContainer, 2, iid, out2), D3D_OK);
    CHECK_EQ(gld(out2), tex);
    CHECK_EQ(gcall(tex, 2, 0), 2);                       /* refs: 1 + surface + container */
    CHECK_EQ(gcall(lvl1, 2, 0), 1);
    /* A8R8G8B8 render-target texture: 4-byte pixels, pitch = 4 * width */
    CHECK_EQ(gcall(dev, S_Device_CreateTexture, 8, 300, 200, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, out, 0), D3D_OK);
    uint32_t rtt = gld(out);
    CHECK_EQ(gcall(rtt, S_Texture_GetSurfaceLevel, 2, 0, out), D3D_OK);
    uint32_t rts = gld(out);
    CHECK_EQ(gcall(dev, S_Device_SetRenderTarget, 2, 0, rts), D3D_OK);
    uint32_t vp = gscratch(24);
    CHECK_EQ(gcall(dev, S_Device_GetViewport, 1, vp), D3D_OK);
    CHECK_EQ(gld(vp + 8), 300);
    CHECK_EQ(gcall(dev, S_Device_GetRenderTarget, 2, 0, out), D3D_OK);
    CHECK_EQ(gld(out), rts);
    gcall(rts, 2, 0);
    CHECK_EQ(gcall(dev, S_Device_SetRenderTarget, 2, 0, 0), D3DERR_INVALIDCALL);
    CHECK_EQ(gcall(rts, S_Surface_LockRect, 3, lr, 0, 0), D3D_OK);
    CHECK_EQ(gld(lr), 1200);
    gcall(rts, 2, 0);
    /* read-back path: offscreen SYSTEMMEM surface + GetRenderTargetData + LockRect */
    CHECK_EQ(gcall(dev, S_Device_CreateOffscreenPlainSurface, 6, 300, 200, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, out, 0), D3D_OK);
    uint32_t off = gld(out);
    CHECK_EQ(gcall(dev, S_Device_GetRenderTargetData, 2, rts, off), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_StretchRect, 5, rts, 0, off, 0, 2), D3D_OK);
    CHECK_EQ(gcall(off, S_Surface_LockRect, 3, lr, 0, D3DLOCK_READONLY), D3D_OK);
    CHECK_EQ(gcall(off, S_Surface_UnlockRect, 0), D3D_OK);
    CHECK_EQ(gcall(off, 2, 0), 0);
    /* cube and volume textures */
    CHECK_EQ(gcall(dev, S_Device_CreateCubeTexture, 7, 64, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, out, 0), D3D_OK);
    uint32_t cube = gld(out), lr2 = gscratch(8);
    CHECK_EQ(gcall(cube, S_CubeTexture_LockRect, 5, 0, 0, lr, 0, 0), D3D_OK);
    CHECK_EQ(gcall(cube, S_CubeTexture_LockRect, 5, 5, 0, lr2, 0, 0), D3D_OK);
    CHECK_EQ(gld(lr), 256);
    CHECK_EQ(gld(lr2 + 4) - gld(lr + 4), 5 * (64 * 256 + 32 * 128 + 16 * 64 + 8 * 32 + 4 * 16 + 16 + 16)); /* face stride, 16-aligned levels */
    CHECK_EQ(gcall(cube, S_CubeTexture_GetCubeMapSurface, 3, 2, 1, out), D3D_OK);
    gcall(gld(out), 2, 0);
    CHECK_EQ(gcall(dev, S_Device_CreateVolumeTexture, 9, 32, 16, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, out, 0), D3D_OK);
    uint32_t vol = gld(out), lb = gscratch(12);
    CHECK_EQ(gcall(vol, S_VolumeTexture_LockBox, 4, 0, lb, 0, 0), D3D_OK);
    CHECK_EQ(gld(lb), 128);
    CHECK_EQ(gld(lb + 4), 128 * 16);
    CHECK_EQ(gcall(vol, S_VolumeTexture_UnlockBox, 1, 0), D3D_OK);
    /* buffers: the dynamic ring's DISCARD / NOOVERWRITE locks */
    CHECK_EQ(gcall(dev, S_Device_CreateVertexBuffer, 6, 65536, 0x208, 0, D3DPOOL_DEFAULT, out, 0), D3D_OK);
    uint32_t vb = gld(out), pb = gscratch(4);
    CHECK_EQ(gcall(vb, S_VertexBuffer_Lock, 4, 0, 0, pb, D3DLOCK_DISCARD), D3D_OK);
    uint32_t vbase = gld(pb);
    CHECK(vbase != 0);
    memset(M + vbase, 1, 65536);
    CHECK_EQ(gcall(vb, S_VertexBuffer_Lock, 4, 4096, 1024, pb, D3DLOCK_NOOVERWRITE), D3D_OK);
    CHECK_EQ(gld(pb), vbase + 4096);
    CHECK_EQ(gcall(vb, S_VertexBuffer_Unlock, 0), D3D_OK);
    CHECK_EQ(gcall(vb, S_VertexBuffer_Lock, 4, 65000, 1024, pb, 0), D3DERR_INVALIDCALL);
    CHECK_EQ(gcall(vb, S_VertexBuffer_GetDesc, 1, desc), D3D_OK);
    CHECK_EQ(gld(desc + 16), 65536);
    CHECK_EQ(gcall(dev, S_Device_CreateIndexBuffer, 6, 6000, 0x208, D3DFMT_INDEX16, D3DPOOL_DEFAULT, out, 0), D3D_OK);
    uint32_t ib = gld(out);
    CHECK_EQ(gcall(ib, S_IndexBuffer_Lock, 4, 0, 0, pb, 0), D3D_OK);
    CHECK_EQ(gcall(ib, S_IndexBuffer_Unlock, 0), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_CreateIndexBuffer, 6, 6000, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, out, 0), D3DERR_INVALIDCALL);
    /* declarations and shaders are kept */
    uint32_t decl = gscratch(24);
    gst(decl, 0); gst(decl + 4, 0x00000002u); /* stream 0 offset 0, FLOAT3 POSITION */
    gst(decl + 8, 0x000000ffu); gst(decl + 12, 0x00000011u); /* D3DDECL_END */
    CHECK_EQ(gcall(dev, S_Device_CreateVertexDeclaration, 2, decl, out), D3D_OK);
    uint32_t vd = gld(out), cnt = gscratch(4);
    CHECK_EQ(gcall(vd, S_VertexDeclaration_GetDeclaration, 2, 0, cnt), D3D_OK);
    CHECK_EQ(gld(cnt), 2);
    CHECK_EQ(gcall(vd, 1, 0), 2);                       /* the cache's AddRef at 0x11f2ca9 */
    CHECK_EQ(gcall(dev, S_Device_SetVertexDeclaration, 1, vd), D3D_OK);
    uint32_t code = gscratch(32);
    gst(code, 0xffff0300u); gst(code + 4, 0x0000fffeu); gst(code + 8, 0x02000001u); gst(code + 12, 0x800f0800u);
    gst(code + 16, 0xa0e40000u); gst(code + 20, 0x0000ffffu); /* empty comment; mov oC0, c0 */
    CHECK_EQ(gcall(dev, S_Device_CreatePixelShader, 2, code, out), D3D_OK);
    uint32_t ps = gld(out);
    CHECK_EQ(gcall(ps, S_PixelShader_GetFunction, 2, 0, cnt), D3D_OK);
    CHECK_EQ(gld(cnt), 24);
    code = gscratch(64);
    { static const uint32_t vs30[] = {0xfffe0300u, 0x0200001fu, 0x80000000u, 0x900f0000u, 0x0200001fu, 0x80000000u,
                                      0xe00f0000u, 0x02000001u, 0xe00f0000u, 0x90e40000u, 0x0000ffffu};
      for (int k = 0; k < 11; k++) gst(code + 4 * k, vs30[k]); }
    CHECK_EQ(gcall(dev, S_Device_CreateVertexShader, 2, code, out), D3D_OK);
    uint32_t vs = gld(out);
    gst(code, 0x12345678u);
    CHECK_EQ(gcall(dev, S_Device_CreateVertexShader, 2, code, out), D3DERR_INVALIDCALL);
    CHECK_EQ(gcall(dev, S_Device_SetVertexShader, 1, vs), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetPixelShader, 1, ps), D3D_OK);
    uint32_t consts = gscratch(16 * 8);
    for (int i = 0; i < 32; i++) gst(consts + 4 * i, 0x3f800000u + i);
    CHECK_EQ(gcall(dev, S_Device_SetVertexShaderConstantF, 3, 250, consts, 6), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetVertexShaderConstantF, 3, 252, consts, 6), D3DERR_INVALIDCALL);
    CHECK_EQ(gcall(dev, S_Device_SetPixelShaderConstantF, 3, 220, consts, 4), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetPixelShaderConstantF, 3, 221, consts, 4), D3DERR_INVALIDCALL);
    uint32_t back = gscratch(64);
    CHECK_EQ(gcall(dev, S_Device_GetPixelShaderConstantF, 3, 221, back, 2), D3D_OK);
    CHECK_EQ(gld(back), 0x3f800004u);
    CHECK_EQ(gcall(dev, S_Device_SetVertexShaderConstantI, 3, 0, consts, 2), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetVertexShaderConstantB, 3, 0, consts, 16), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetPixelShaderConstantI, 3, 15, consts, 1), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetPixelShaderConstantB, 3, 3, consts, 4), D3D_OK);
    /* state: render states (SCISSORTESTENABLE get/set at 0x82e6dd), samplers, scissor */
    CHECK_EQ(gcall(dev, S_Device_GetRenderState, 2, 22, out), D3D_OK);
    CHECK_EQ(gld(out), 3);                               /* CULLMODE default CCW */
    CHECK_EQ(gcall(dev, S_Device_SetRenderState, 2, 0xae, 1), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_GetRenderState, 2, 0xae, out), D3D_OK);
    CHECK_EQ(gld(out), 1);
    CHECK_EQ(gcall(dev, S_Device_SetSamplerState, 3, 257, 5, 2), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_GetSamplerState, 3, 257, 5, out), D3D_OK);
    CHECK_EQ(gld(out), 2);
    gst(rect, 1); gst(rect + 4, 2); gst(rect + 8, 3); gst(rect + 12, 4);
    CHECK_EQ(gcall(dev, S_Device_SetScissorRect, 1, rect), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_GetScissorRect, 1, back), D3D_OK);
    CHECK_EQ(gld(back + 12), 4);
    CHECK_EQ(gcall(dev, S_Device_SetTextureStageState, 3, 0, 1, 4), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetTransform, 2, 2, consts), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetPaletteEntries, 2, 0, consts), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetCurrentTexturePalette, 1, 0), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetSoftwareVertexProcessing, 1, 0), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetGammaRamp, 3, 0, 0, gscratch(1536)), 0);
    CHECK_EQ(gcall(dev, S_Device_EvictManagedResources, 0), D3D_OK);
    /* bindings and draws */
    CHECK_EQ(gcall(dev, S_Device_SetTexture, 2, 0, tex), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_GetTexture, 2, 0, out), D3D_OK);
    CHECK_EQ(gld(out), tex);
    gcall(tex, 2, 0);
    CHECK_EQ(gcall(dev, S_Device_SetStreamSource, 4, 0, vb, 0, 32), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetStreamSourceFreq, 2, 0, 0x40000000u | 10), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetStreamSourceFreq, 2, 1, 0x80000001u), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetIndices, 1, ib), D3D_OK);
    d3d9null_stats st0, st1;
    d3d9null_get_stats(&st0);
    CHECK_EQ(gcall(dev, S_Device_BeginScene, 0), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_BeginScene, 0), D3DERR_INVALIDCALL);
    CHECK_EQ(gcall(dev, S_Device_Clear, 6, 0, 0, 7, 0xff000000u, 0x3f800000u, 0), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_DrawIndexedPrimitive, 6, 4, 0, 0, 100, 0, 50), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_DrawPrimitive, 3, 5, 0, 20), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_EndScene, 0), D3D_OK);
    /* the frame limiter: EVENT query Issue(END) then GetData(NULL, 0, FLUSH) spin, and the NULL probe */
    CHECK_EQ(gcall(dev, S_Device_CreateQuery, 2, D3DQUERYTYPE_EVENT, 0), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_CreateQuery, 2, D3DQUERYTYPE_VCACHE, out), D3DERR_NOTAVAILABLE);
    CHECK_EQ(gcall(dev, S_Device_CreateQuery, 2, D3DQUERYTYPE_EVENT, out), D3D_OK);
    uint32_t q = gld(out);
    CHECK_EQ(gcall(q, S_Query_Issue, 1, D3DISSUE_END), D3D_OK);
    CHECK_EQ(gcall(q, S_Query_GetData, 3, 0, 0, D3DGETDATA_FLUSH), D3D_OK);
    CHECK_EQ(gcall(q, S_Query_GetData, 3, out2, 4, D3DGETDATA_FLUSH), D3D_OK);
    CHECK_EQ(gld(out2), 1);
    CHECK_EQ(gcall(q, S_Query_GetDataSize, 0), 4);
    setenv("SPORE_D3D9_FPS", "0", 1);
    CHECK_EQ(gcall(dev, S_Device_Present, 4, 0, 0, 0, 0), D3D_OK);
    d3d9null_get_stats(&st1);
    CHECK_EQ(st1.frames - st0.frames, 1);
    CHECK_EQ(st1.draws - st0.draws, 2);
    CHECK_EQ(st1.primitives - st0.primitives, 70);
    /* additional swap chain (type-1 rasters, 0x11f00f0) */
    uint32_t pp2 = gscratch(56);
    for (int i = 0; i < 14; i++) gst(pp2 + 4 * i, ppv[i]);
    gst(pp2, 0); gst(pp2 + 4, 0);                        /* windowed 0x0: client size (1024x768 default) */
    gst(pp2 + 4 * PP_AUTODS, 0);
    CHECK_EQ(gcall(dev, S_Device_CreateAdditionalSwapChain, 2, pp2, out), D3D_OK);
    uint32_t sc = gld(out);
    CHECK_EQ(gld(pp2), 1024);
    CHECK_EQ(gcall(sc, S_SwapChain_GetBackBuffer, 3, 0, 0, out), D3D_OK);
    gcall(gld(out), 2, 0);
    CHECK_EQ(gcall(sc, S_SwapChain_Present, 5, 0, 0, 0, 0, 0), D3D_OK);
    CHECK_EQ(gcall(sc, 2, 0), 0);
    /* lost-device path: release DEFAULT resources, Reset at a new size, re-get the surfaces */
    CHECK_EQ(gcall(q, 2, 0), 0);
    CHECK_EQ(gcall(vb, 2, 0), 0);
    CHECK_EQ(gcall(ib, 2, 0), 0);
    CHECK_EQ(gcall(rtt, 2, 0), 0);
    gst(pp, 1280); gst(pp + 4, 720);
    T.fpu.cw = 0x027f;
    CHECK_EQ(gcall(dev, S_Device_Reset, 1, pp), D3D_OK);
    CHECK_EQ(T.fpu.cw, 0x007f);
    CHECK_EQ(gcall(dev, S_Device_GetRenderTarget, 2, 0, out), D3D_OK);
    CHECK_EQ(gcall(gld(out), S_Surface_GetDesc, 1, desc), D3D_OK);
    CHECK_EQ(gld(desc + 24), 1280);
    gcall(gld(out), 2, 0);
    CHECK_EQ(gcall(dev, S_Device_GetRenderState, 2, 0xae, out), D3D_OK);
    CHECK_EQ(gld(out), 0);                               /* Reset restores default state */
    /* resource priority (every call through gcall also checks the stack clean-up) */
    CHECK_EQ(gcall(tex, S_Texture_GetPriority, 0), 0);
    /* the D3DX buffer helper */
    uint32_t data = 0, xb = d3d9null_d3dx_buffer(100, &data);
    CHECK(xb && data);
    CHECK_EQ(gcall(xb, S_D3DXBuffer_GetBufferPointer, 0), data);
    CHECK_EQ(gcall(xb, S_D3DXBuffer_GetBufferSize, 0), 100);
    CHECK_EQ(gcall(xb, 2, 0), 0);
    /* release everything */
    CHECK_EQ(gcall(vd, 2, 0), 1);
    CHECK_EQ(gcall(vd, 2, 0), 0);
    CHECK_EQ(gcall(ps, 2, 0), 0);
    CHECK_EQ(gcall(vs, 2, 0), 0);
    CHECK_EQ(gcall(tex, 2, 0), 0);
    CHECK_EQ(gcall(cube, 2, 0), 0);
    CHECK_EQ(gcall(vol, 2, 0), 0);
    CHECK_EQ(gcall(dev, 2, 0), 0);
    CHECK_EQ(gcall(d3d, 2, 0), 0);
    CHECK_EQ(sg_com_live_objects(), live0);
    d3d9null_get_stats(&st1);
    CHECK_EQ(st1.live_resources, 0);
    CHECK_EQ(st1.resource_bytes, 0);
    /* a call on the released device is caught (the record is invalid) */
    CHECK(sg_com_lookup(M, dev) == NULL);
    printf("d3d9null: %llu COM calls, %u bindings, %llu locks\n", (unsigned long long)sg_com_total_calls(),
           sg_com_bindings_used(), (unsigned long long)st1.locks);
    return harness_report("runtime/d3d9null");
}
