/* Import table, loader, GetProcAddress back end and the unimplemented-stub policy. */
#include "harness.h"

static char last_log[512];
static void capture(int level, const char *line) {
    (void)level;
    snprintf(last_log, sizeof last_log, "%s", line);
}

void test_table(void) {
    unsigned i, j, data = 0, impl = 0;
    CHECK_EQ(hle_import_count, 495);
    CHECK_EQ(HLE_IMP_COUNT, 495);
    for (i = 0; i < hle_import_count; i++) {
        const sg_import *m = &hle_imports[i];
        if (m->cc == HLE_CC_DATA) { data++; CHECK(m->fn == NULL); continue; }
        CHECK(m->fn != NULL);
        CHECK(m->iat >= SG_IAT_START && m->iat < SG_IAT_END && (m->iat & 3) == 0);
        if (i) CHECK(m->iat > hle_imports[i - 1].iat);
        if (m->cc == SG_CC_CDECL || m->cc == SG_CC_X87) CHECK_EQ(m->argbytes, 0);
        impl += hle_import_info[i].implemented;
    }
    CHECK_EQ(data, 2); /* _adjust_fdiv, _acmdln */
    for (i = 0; i < hle_import_count; i++)
        for (j = i + 1; j < hle_import_count; j++)
            if (!strcmp(hle_import_info[i].symbol, hle_import_info[j].symbol)) { t_fail++; fprintf(stderr, "dup %s\n", hle_import_info[i].symbol); }
    printf("  import table: %u imports, %u data, %u implemented, %u generated stubs\n", hle_import_count, data, impl,
           hle_import_count - data - impl);

    /* known slots [obs: data/imports.json] */
    CHECK_EQ(hle_imports[HLE_IMP_advapi32_GetUserNameW].iat, 0x013cc000u);
    CHECK_EQ(hle_imports[HLE_IMP_msvcr90_atol].iat, 0x013cc4c8u);
    CHECK_EQ(hle_imports[HLE_IMP_kernel32_MultiByteToWideChar].iat, 0x013cc220u);
    CHECK_EQ(hle_imports[HLE_IMP_kernel32_WideCharToMultiByte].iat, 0x013cc224u);
    CHECK_EQ(hle_imports[HLE_IMP_kernel32_WideCharToMultiByte].argbytes, 32);
    CHECK_EQ(hle_imports[HLE_IMP_msvcr90__CxxThrowException].cc, SG_CC_STDCALL);
    CHECK_EQ(hle_imports[HLE_IMP_msvcr90__CIpow].cc, SG_CC_X87);
    CHECK_EQ(hle_imports[HLE_IMP_wsock32_ord19].ordinal, 19);

    /* loader: every IAT slot written; thunk slots resolve back to the shim */
    CHECK_EQ(hle_install_imports(M), 495);
    for (i = 0; i < hle_import_count; i++) {
        uint32_t v = sg_ld32(M, hle_imports[i].iat);
        CHECK(v == hle_imports[i].guest && v != 0);
        if (hle_imports[i].cc != HLE_CC_DATA)
            CHECK(sg_thunk_fn[(v - SG_THUNK_BASE) / SG_THUNK_SLOT] == hle_imports[i].fn);
    }

    /* GetProcAddress back end */
    CHECK_EQ(hle_resolve("KERNEL32.DLL", "GetTickCount", 0), hle_imports[HLE_IMP_kernel32_GetTickCount].guest);
    CHECK_EQ(hle_resolve("kernel32", "GetTickCount", 0), hle_imports[HLE_IMP_kernel32_GetTickCount].guest);
    CHECK_EQ(hle_resolve("WSOCK32.dll", NULL, 19), hle_imports[HLE_IMP_wsock32_ord19].guest);
    CHECK_EQ(hle_resolve("ntdll", "TGGetOS", 0), 0);             /* keeps $cider false */
    CHECK_EQ(hle_resolve("nvapi.dll", "nvapi_QueryInterface", 0), 0);
    CHECK_EQ(hle_resolve("gdi32.dll", "D3DKMTEscape", 0), 0);
    CHECK(hle_resolve("xinput1_1.dll", "XInputGetState", 0) != 0);
    CHECK_EQ(hle_resolve("xinput9_1_0.dll", "XInputGetState", 0), hle_resolve("xinput1_1.dll", "XInputGetState", 0));
    CHECK(hle_resolve("kernel32.dll", "GetProductInfo", 0) != 0);
    CHECK_EQ(hle_resolve("dbghelp.dll", "SymInitialize", 0), hle_imports[HLE_IMP_dbghelp_SymInitialize].guest);
    CHECK_EQ(hle_resolve("kernel32.dll", "NoSuchFunction", 0), 0);
    CHECK(hle_module_known("Kernel32.dll"));
    CHECK(hle_module_known("XINPUT1_1.DLL"));
    CHECK(!hle_module_known("nvapi.dll"));
    CHECK(!hle_module_known("rasapi32.dll"));

    /* unimplemented stub under the "continue" policy: logs once, returns 0, pops stdcall args */
    {
        void (*old)(int, const char *) = hle_log_sink;
        uint32_t r;
        hle_log_sink = capture;
        last_log[0] = 0;
        T.eax = 0x1234;
        r = CALL(sg_imp_kernel32_CreateFileW, 28, gwstr("x"), 0, 0, 0, 0, 0, 0);
        CHECK_EQ(r, 0);
        CHECK(strstr(last_log, "unimplemented: KERNEL32.dll!CreateFileW") != NULL);
        CHECK(strstr(last_log, "0x00401234") != NULL);
        last_log[0] = 0;
        r = CALL(sg_imp_kernel32_CreateFileW, 28, gwstr("x"), 0, 0, 0, 0, 0, 0);
        CHECK(last_log[0] == 0); /* logged only once */
        /* a cdecl stub pops nothing */
        r = CALL(sg_imp_msvcr90_fopen, 0, gstr("a"), gstr("r"));
        CHECK_EQ(r, 0);
        hle_log_sink = old;
    }
}
