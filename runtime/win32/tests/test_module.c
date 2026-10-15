/* test_module.c -- registration into runtime/hle's table, the loader steps, modules, GetProcAddress,
 * environment, command line and system information. */
#include "harness.h"
#include "w32_int.h"

#include <stdlib.h>

#include "hle_imports.h"

extern const int w32_overrides_linked;

static uint32_t enum_names[8], nenum;
static uint32_t cb_enum(sg_ctx *ctx, unsigned n, const uint32_t *a) {
    (void)ctx; (void)n;
    enum_names[nenum++] = a[2];
    return a[3] == 77 ? 1 : 0; /* lParam 77: continue; anything else: stop after the first */
}

/* A tiny PE header and resource tree of our own at the image base (the harness maps the image
 * range read/write but leaves it empty): RT_ICON names 1 and 2, RT_VERSION 1 holding a
 * VS_VERSIONINFO with file version 1.5.3.7. */
static void build_fake_image(void) {
    uint32_t b = SG_IMAGE_BASE, pe = b + 0x80, r = b + 0x1000;
    sg_st16(M, b, 0x5a4d);
    sg_st32(M, b + 0x3c, 0x80);
    sg_st32(M, pe, 0x4550);
    sg_st16(M, pe + 0x18, 0x10b);
    sg_st32(M, pe + 0x74, 16);
    sg_st32(M, pe + 0x78 + 16, 0x1000);
    sg_st32(M, pe + 0x78 + 20, 0x200);
#define DIR(at, nids) do { memset(M + (at), 0, 16); sg_st16(M, (at) + 14, (nids)); } while (0)
#define ENT(at, i, id, off) do { sg_st32(M, (at) + 16 + 8 * (i), (id)); sg_st32(M, (at) + 20 + 8 * (i), (off)); } while (0)
    DIR(r, 2);
    ENT(r, 0, 3, 0x80000000u | 0x30);  /* RT_ICON -> dir at +0x30 */
    ENT(r, 1, 16, 0x80000000u | 0x60); /* RT_VERSION -> dir at +0x60 */
    DIR(r + 0x30, 2);
    ENT(r + 0x30, 0, 1, 0x80000000u | 0x80);
    ENT(r + 0x30, 1, 2, 0x80000000u | 0x80);
    DIR(r + 0x60, 1);
    ENT(r + 0x60, 0, 1, 0x80000000u | 0x80);
    DIR(r + 0x80, 1);
    ENT(r + 0x80, 0, 0x409, 0xa0);      /* language -> data entry at +0xa0 */
    sg_st32(M, r + 0xa0, 0x1100);       /* data RVA */
    sg_st32(M, r + 0xa4, 0x5c);         /* size */
    uint32_t v = b + 0x1100;
    sg_st16(M, v, 0x5c);
    sg_st16(M, v + 2, 0x34);
    const char *key = "VS_VERSION_INFO";
    for (int i = 0; i <= 15; i++) sg_st16(M, v + 6 + 2 * i, i < 15 ? (uint8_t)key[i] : 0);
    sg_st32(M, v + 40, 0xfeef04bdu);
    sg_st32(M, v + 44, 0x10000);
    sg_st32(M, v + 48, 0x00010005);
    sg_st32(M, v + 52, 0x00030007);
#undef DIR
#undef ENT
}

static void test_resources(void) {
    build_fake_image();
    uint32_t exe = gbuf(300), h = gbuf(4);
    CHECK_EQ(CALL(w32_imp_kernel32_GetModuleFileNameA, 12, 0, exe, 260), 33);
    uint32_t sz = CALL(w32_imp_version_GetFileVersionInfoSizeA, 8, exe, h);
    CHECK(sz >= 0x5c);
    uint32_t blk = CALL(w32_imp_kernel32_LocalAlloc, 8, 0x40, sz);
    CHECK_EQ(CALL(w32_imp_version_GetFileVersionInfoA, 16, exe, 0, sz, blk), 1);
    uint32_t pp = gbuf(4), pl = gbuf(4);
    CHECK_EQ(CALL(w32_imp_version_VerQueryValueA, 16, blk, gstr("\\"), pp, pl), 1);
    uint32_t ffi = sg_ld32(M, pp);
    CHECK_EQ(ffi, blk + 40);
    CHECK_EQ(sg_ld16(M, ffi + 0xa), 1); /* the fields 0x6b8bfc..0x6b8c0a read: 1.5.3.7 */
    CHECK_EQ(sg_ld16(M, ffi + 0x8), 5);
    CHECK_EQ(sg_ld16(M, ffi + 0xe), 3);
    CHECK_EQ(sg_ld16(M, ffi + 0xc), 7);
    CHECK_EQ(sg_ld32(M, pl), 0x34);
    CHECK_EQ(CALL(w32_imp_version_VerQueryValueW, 16, blk, gwstr("\\StringFileInfo\\040904b0\\FileVersion"), pp, pl), 0);
    CHECK_EQ(CALL(w32_imp_version_GetFileVersionInfoSizeA, 8, gstr("C:\\Windows\\system32\\d3d9.dll"), h), 0);
    CHECK_EQ(last_error(), W32_ERROR_FILE_NOT_FOUND);
    CHECK_EQ(CALL(w32_imp_version_GetFileVersionInfoSizeA, 8, gstr("C:\\Windows"), h), 0);
    CHECK_EQ(last_error(), 1813);
    uint32_t cb = register_callback(cb_enum);
    nenum = 0;
    CHECK_EQ(CALL(w32_imp_kernel32_EnumResourceNamesA, 16, 0, 3, cb, 77), 1);
    CHECK_EQ(nenum, 2);
    CHECK(enum_names[0] == 1 && enum_names[1] == 2);
    nenum = 0;
    CALL(w32_imp_kernel32_EnumResourceNamesA, 16, SG_IMAGE_BASE, 3, cb, 0);
    CHECK_EQ(nenum, 1); /* the callback's FALSE stops the enumeration */
    CHECK_EQ(CALL(w32_imp_kernel32_EnumResourceNamesA, 16, 0, 14, cb, 0), 0); /* no RT_GROUP_ICON */
    memset(M + SG_IMAGE_BASE, 0, 0x1200);
    CHECK_EQ(CALL(w32_imp_version_GetFileVersionInfoSizeA, 8, exe, h), 0); /* no image mapped */

    /* identity */
    uint32_t nb = gbuf(64), ns = gbuf(4);
    sg_st32(M, ns, 64);
    CHECK_EQ(CALL(w32_imp_kernel32_GetComputerNameExA, 12, 1, nb, ns), 1);
    CHECK_STR((const char *)M + nb, "SPORE-MAC");
    CHECK_EQ(sg_ld32(M, ns), 9);
    sg_st32(M, ns, 4);
    CHECK_EQ(CALL(w32_imp_kernel32_GetComputerNameExA, 12, 1, nb, ns), 0);
    CHECK_EQ(sg_ld32(M, ns), 10);
    CHECK_EQ(last_error(), W32_ERROR_MORE_DATA);
    sg_st32(M, ns, 64);
    CHECK_EQ(CALL(w32_imp_advapi32_GetUserNameW, 8, nb, ns), 1);
    CHECK_STR(hwstr(nb), "Player");
    CHECK_EQ(sg_ld32(M, ns), 7);
    uint32_t ev = CALL(w32_imp_advapi32_RegisterEventSourceA, 8, 0, gstr("OpenSSL"));
    uint32_t strs = gbuf(4);
    sg_st32(M, strs, gstr("test report"));
    CHECK_EQ(CALL(w32_imp_advapi32_ReportEventA, 36, ev, 1, 0, 0, 0, 1, 0, strs, 0), 1);
    CHECK_EQ(CALL(w32_imp_advapi32_DeregisterEventSource, 4, ev), 1);
}

static unsigned find_import(const char *dll, const char *name) {
    for (unsigned i = 0; i < hle_import_count; i++)
        if (hle_imports[i].name && !strcmp(hle_imports[i].name, name) && !strcasecmp(hle_imports[i].dll, dll)) return i;
    return ~0u;
}

void test_module(void) {
    /* direct calls by symbol, as translated code makes them (SG_CALL(sg_imp_..., ra)) */
    if (w32_overrides_linked) {
        uint32_t p = CALL(sg_imp_msvcr90_malloc, 0, 40);
        CHECK(p != 0);
        CHECK_EQ(w32_msize(p), 40);
        CALL(sg_imp_msvcr90_free, 0, p);
        CHECK_EQ(CALL(sg_imp_kernel32_GetFileAttributesW, 4, gwstr("C:\\Spore")), 0x10);
        unsigned m = 0;
        for (unsigned i = 0; i < hle_import_count; i++)
            if (hle_imports[i].name && !strcmp(hle_imports[i].name, "malloc")) m = i;
        CHECK(hle_imports[m].fn == (sg_func *)sg_imp_msvcr90_malloc); /* the table holds the override already */
        printf("module tests: sg_imp_* overrides linked; direct calls reach runtime/win32\n");
    } else {
        printf("module tests: overrides off (runtime/hle stubs are strong); only the table swap is tested\n");
    }
    /* registration: every shim for an import of this exe is swapped in, with matching clean-up */
    unsigned bad = 99, n = w32_register_shims(&bad);
    CHECK_EQ(bad, 0);
    unsigned imported = 0;
    for (unsigned s = 0; s < w32_shim_count; s++)
        for (unsigned i = 0; i < hle_import_count; i++)
            if (hle_imports[i].name && !strcmp(hle_imports[i].name, w32_shims[s].name) &&
                !strcasecmp(hle_imports[i].dll, w32_shims[s].dll)) imported++;
    CHECK_EQ(n, imported);
    CHECK(n >= 120);
    unsigned k = find_import("KERNEL32.dll", "CreateFileW");
    CHECK(k != ~0u && hle_imports[k].fn == (sg_func *)w32_imp_kernel32_CreateFileW);
    k = find_import("MSVCR90.dll", "malloc");
    CHECK(k != ~0u && hle_imports[k].fn == (sg_func *)w32_imp_msvcr90_malloc);
    k = find_import("KERNEL32.dll", "TlsGetValue");
    CHECK(k != ~0u && hle_imports[k].fn != NULL); /* not ours: still runtime/hle's stub */
    /* GetModuleFileNameW is also a dynamic name (0x92b874): the dynamic entry follows */
    int dyn_ok = 0;
    for (unsigned d = 0; d < hle_dynamic_count; d++)
        if (!strcmp(hle_dynamics[d].name, "GetModuleFileNameW")) dyn_ok = hle_dynamics[d].fn == (sg_func *)w32_imp_kernel32_GetModuleFileNameW;
    CHECK(dyn_ok);

    /* the loader steps: install the IAT, then point _acmdln at the command line */
    CHECK_EQ(hle_install_imports(M), hle_import_count);
    w32_after_install(M);
    k = find_import("MSVCR90.dll", "_acmdln");
    uint32_t acmdln = sg_ld32(M, sg_ld32(M, hle_imports[k].iat));
    CHECK_STR((const char *)M + acmdln, "\"C:\\Spore\\SporebinEP1\\SporeApp.exe\" -ep1 -dataDir:\"C:\\My Data\\\" x");
    k = find_import("KERNEL32.dll", "VirtualAlloc");
    uint32_t thunk = sg_ld32(M, hle_imports[k].iat);
    CHECK(sg_thunk_fn[(thunk - SG_THUNK_BASE) / 16] == (sg_func *)w32_imp_kernel32_VirtualAlloc);

    /* command line */
    CHECK_EQ(CALL0(w32_imp_kernel32_GetCommandLineA, 0), acmdln);
    uint32_t cw = CALL0(w32_imp_kernel32_GetCommandLineW, 0);
    CHECK_STR(hwstr(cw), "\"C:\\Spore\\SporebinEP1\\SporeApp.exe\" -ep1 -dataDir:\"C:\\My Data\\\" x");

    /* modules */
    CHECK_EQ(CALL(w32_imp_kernel32_GetModuleHandleA, 4, 0), SG_IMAGE_BASE);
    CHECK_EQ(CALL(w32_imp_kernel32_GetModuleHandleW, 4, gwstr("SporeApp.exe")), SG_IMAGE_BASE);
    uint32_t nt = CALL(w32_imp_kernel32_GetModuleHandleA, 4, gstr("ntdll"));
    CHECK(nt != 0 && !(nt & 0xffff));
    CHECK_EQ(sg_ld16(M, nt), 0x5a4d);
    CHECK_EQ(CALL(w32_imp_kernel32_GetModuleHandleA, 4, gstr("NTDLL.DLL")), nt);
    /* TGGetOS probe (0x6b8130): ntdll exists, TGGetOS does not -> $cider stays false */
    CHECK_EQ(CALL(w32_imp_kernel32_GetProcAddress, 8, nt, gstr("TGGetOS")), 0);
    CHECK_EQ(last_error(), W32_ERROR_PROC_NOT_FOUND);
    uint32_t k32 = CALL(w32_imp_kernel32_GetModuleHandleA, 4, gstr("kernel32.dll"));
    CHECK(k32 != 0);
    uint32_t gp = CALL(w32_imp_kernel32_GetProcAddress, 8, k32, gstr("GetProductInfo"));
    CHECK(gp >= SG_THUNK_BASE && gp < SG_THUNK_LIMIT);
    uint32_t gm = CALL(w32_imp_kernel32_GetProcAddress, 8, k32, gstr("GetModuleFileNameW"));
    CHECK(sg_thunk_fn[(gm - SG_THUNK_BASE) / 16] == (sg_func *)w32_imp_kernel32_GetModuleFileNameW);
    /* XInput: not loaded until LoadLibrary, then resolvable; xinput1_1 is the same module */
    CHECK_EQ(CALL(w32_imp_kernel32_GetModuleHandleA, 4, gstr("xinput9_1_0.dll")), 0);
    uint32_t xi = CALL(w32_imp_kernel32_LoadLibraryA, 4, gstr("xinput9_1_0.dll"));
    CHECK(xi != 0);
    CHECK(CALL(w32_imp_kernel32_GetProcAddress, 8, xi, gstr("XInputGetState")) != 0);
    CHECK_EQ(CALL(w32_imp_kernel32_GetModuleHandleA, 4, gstr("XINPUT9_1_0")), xi);
    /* absent components fail as on a GOG install */
    CHECK_EQ(CALL(w32_imp_kernel32_LoadLibraryA, 4, gstr("rasapi32.dll")), 0);
    CHECK_EQ(last_error(), W32_ERROR_MOD_NOT_FOUND);
    CHECK_EQ(CALL(w32_imp_kernel32_LoadLibraryW, 4, gwstr("nvapi.dll")), 0);
    CHECK_EQ(CALL(w32_imp_kernel32_LoadLibraryA, 4, gstr("DasmX86Dll.dll")), 0);
    CHECK_EQ(CALL(w32_imp_kernel32_FreeLibrary, 4, xi), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_FreeLibrary, 4, 0x1234), 0);
    CHECK_EQ(CALL(w32_imp_kernel32_GetProcAddress, 8, SG_IMAGE_BASE, gstr("anything")), 0);
    CHECK_EQ(CALL(w32_imp_kernel32_GetProcAddress, 8, 0x5550000, gstr("anything")), 0);
    CHECK_EQ(last_error(), W32_ERROR_MOD_NOT_FOUND);

    /* GetModuleFileName (Vista+ truncation rules) */
    uint32_t buf = gbuf(600);
    CHECK_EQ(CALL(w32_imp_kernel32_GetModuleFileNameW, 12, 0, buf, 260), 33);
    CHECK_STR(hwstr(buf), "C:\\Spore\\SporebinEP1\\SporeApp.exe");
    CHECK_EQ(CALL(w32_imp_kernel32_GetModuleFileNameA, 12, SG_IMAGE_BASE, buf, 10), 10);
    CHECK_EQ(last_error(), W32_ERROR_INSUFFICIENT_BUFFER);
    CHECK_STR((const char *)M + buf, "C:\\Spore\\");
    CHECK_EQ(CALL(w32_imp_kernel32_GetModuleFileNameA, 12, k32, buf, 260), 32);
    CHECK_STR((const char *)M + buf, "C:\\Windows\\system32\\kernel32.dll");

    /* environment: the SingleMgrMutex pattern of 0x935ae0 */
    char name[64];
    snprintf(name, sizeof name, "SingleMgrMutex%08x", 1234);
    uint32_t gn = gstr(name), vb = gbuf(32);
    CHECK_EQ(CALL(w32_imp_kernel32_GetEnvironmentVariableA, 12, gn, vb, 32), 0);
    CHECK_EQ(last_error(), W32_ERROR_ENVVAR_NOT_FOUND);
    CHECK_EQ(CALL(w32_imp_kernel32_SetEnvironmentVariableA, 8, gn, gstr("1a2b3c4d")), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_GetEnvironmentVariableA, 12, gn, vb, 32), 8);
    CHECK_STR((const char *)M + vb, "1a2b3c4d");
    CHECK_EQ(CALL(w32_imp_kernel32_GetEnvironmentVariableA, 12, gn, vb, 8), 9); /* needs 9 with the NUL */
    CHECK_EQ(CALL(w32_imp_kernel32_GetEnvironmentVariableA, 12, gstr("singlemgrmutex000004d2"), vb, 32), 8);
    CHECK_EQ(CALL(w32_imp_kernel32_SetEnvironmentVariableA, 8, gn, 0), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_GetEnvironmentVariableA, 12, gn, vb, 32), 0);
    CHECK_EQ(CALL(w32_imp_kernel32_GetEnvironmentVariableA, 12, gstr("JPEGMEM"), vb, 32), 0);
    CHECK_EQ(CALL(w32_imp_kernel32_GetEnvironmentVariableA, 12, gstr("windir"), vb, 32), 10);
    CHECK_STR((const char *)M + vb, "C:\\Windows");

    /* start-up and system information */
    uint32_t si = gbuf(68);
    memset(M + si, 0xcc, 68);
    CALL(w32_imp_kernel32_GetStartupInfoA, 4, si);
    CHECK_EQ(sg_ld32(M, si), 68);
    CHECK_EQ(sg_ld32(M, si + 44), 0); /* dwFlags: no STARTF_USESHOWWINDOW */
    uint32_t sys = gbuf(36);
    CALL(w32_imp_kernel32_GetSystemInfo, 4, sys);
    CHECK_EQ(sg_ld32(M, sys + 4), 4096);
    CHECK_EQ(sg_ld32(M, sys + 28), 65536);
    CHECK(sg_ld32(M, sys + 20) >= 1 && sg_ld32(M, sys + 20) <= 16);
    uint32_t ov = gbuf(284);
    sg_st32(M, ov, 148);
    CHECK_EQ(CALL(w32_imp_kernel32_GetVersionExA, 4, ov), 1);
    CHECK_EQ(sg_ld32(M, ov + 4), 6);
    CHECK_EQ(sg_ld32(M, ov + 8), 1);
    CHECK_EQ(sg_ld32(M, ov + 16), 2);
    CHECK_STR((const char *)M + ov + 20, "Service Pack 1");
    sg_st32(M, ov, 284);
    CHECK_EQ(CALL(w32_imp_kernel32_GetVersionExW, 4, ov), 1);
    CHECK_STR(hwstr(ov + 20), "Service Pack 1");
    CHECK_EQ(sg_ld16(M, ov + 276), 1);
    sg_st32(M, ov, 100);
    CHECK_EQ(CALL(w32_imp_kernel32_GetVersionExA, 4, ov), 0);
    CHECK_EQ(CALL0(w32_imp_kernel32_GetVersion, 0), (7601u << 16) | 0x106);
    CHECK_EQ(CALL0(w32_imp_kernel32_IsDebuggerPresent, 0), 0);
    test_resources();
}
