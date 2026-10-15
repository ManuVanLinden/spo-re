/* test_process.c -- TerminateProcess, GetProcessTimes, _searchenv, _spawnv, _getch. */
#include "harness.h"
#include "w32_dirwatch.h"
#include "w32_int.h"

#include <setjmp.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

static jmp_buf exit_jb;
static int exit_code;
static void catch_exit(int code) {
    exit_code = code;
    longjmp(exit_jb, 1);
}

static void touch(const char *rel) {
    char p[2048];
    snprintf(p, sizeof p, "%s/%s", t_root, rel);
    FILE *f = fopen(p, "w");
    if (f) { fputs("x", f); fclose(f); }
}

void test_process(void) {
    /* GetProcessTimes on the current process */
    uint32_t t = gbuf(32);
    for (volatile int i = 0, x = 0; i < 2000000; i++) x += i; /* burn a little user time */
    CHECK_EQ(CALL(w32_imp_kernel32_GetProcessTimes, 20, 0xffffffffu, t, t + 8, t + 16, t + 24), 1);
    uint64_t created = sg_ld64(M, t), user = sg_ld64(M, t + 24);
    uint64_t now = w32_filetime_from_unix(time(NULL) + 1, 0);
    CHECK(created > 116444736000000000ull && created <= now);    /* after 1970, not in the future */
    CHECK(now - created < 3600ull * 10000000ull);                  /* this test started within the hour */
    CHECK_EQ(sg_ld64(M, t + 8), 0);
    CHECK(user > 0);
    CHECK_EQ(CALL(w32_imp_kernel32_GetProcessTimes, 20, 0x1234, t, t + 8, t + 16, t + 24), 0);
    CHECK_EQ(last_error(), 6);

    /* TerminateProcess(GetCurrentProcess(), STATUS_STACK_BUFFER_OVERRUN): the exit hook gets the code */
    void (*saved)(int) = w32_exit_hook;
    w32_exit_hook = catch_exit;
    uint32_t esp0 = T.esp;
    exit_code = -1;
    if (!setjmp(exit_jb)) {
        CALL(w32_imp_kernel32_TerminateProcess, 8, 0xffffffffu, 0xC0000409u);
        CHECK(0); /* not reached */
    }
    T.esp = esp0;
    w32_exit_hook = saved;
    CHECK_EQ((uint32_t)exit_code, 0xC0000409u);
    CHECK_EQ(CALL(w32_imp_kernel32_TerminateProcess, 8, 0x44, 1), 0);
    CHECK_EQ(last_error(), 6);

    /* _searchenv / _wsearchenv: current directory (the exe folder) first, then the variable */
    char p[2048];
    snprintf(p, sizeof p, "%s/game/Data", t_root);
    mkdir(p, 0755);
    touch("game/SporebinEP1/here.dll");
    touch("game/Data/there.txt");
    w32_env_set("SPORE_TEST_PATH", "C:\\nowhere;\"C:\\Spore\\Data\";");
    uint32_t out = gbuf(600);
    CALL(w32_imp_msvcr90__wsearchenv, 0, gwstr("there.txt"), gwstr("SPORE_TEST_PATH"), out);
    CHECK_STR(hwstr(out), "C:\\Spore\\Data\\there.txt");
    CALL(w32_imp_msvcr90__wsearchenv, 0, gwstr("THERE.TXT"), gwstr("SPORE_TEST_PATH"), out);
    CHECK_STR(hwstr(out), "C:\\Spore\\Data\\THERE.TXT"); /* the name as given, as Windows returns it */
    CALL(w32_imp_msvcr90__searchenv, 0, gstr("here.dll"), gstr("SPORE_TEST_PATH"), out);
    CHECK_STR((const char *)M + out, "C:\\Spore\\SporebinEP1\\here.dll");
    hle_set_errno(M, 0);
    CALL(w32_imp_msvcr90__wsearchenv, 0, gwstr("missing.dll"), gwstr("PATH"), out);
    CHECK_STR(hwstr(out), "");
    CHECK_EQ(hle_get_errno(M), MSVC_ENOENT);
    CALL(w32_imp_msvcr90__wsearchenv, 0, gwstr("there.txt"), gwstr("NO_SUCH_VARIABLE"), out);
    CHECK_STR(hwstr(out), "");

    /* _spawnv / _wspawnv never start anything */
    uint32_t argv = gbuf(16);
    sg_st32(M, argv, gstr("tool.exe"));
    sg_st32(M, argv + 4, gstr("-x"));
    sg_st32(M, argv + 8, 0);
    hle_set_errno(M, 0);
    CHECK_EQ(CALL(w32_imp_msvcr90__spawnv, 0, 0, gstr("C:\\tool.exe"), argv), 0xffffffffu);
    CHECK_EQ(hle_get_errno(M), MSVC_ENOENT);
    CHECK_EQ(CALL(w32_imp_msvcr90__wspawnv, 0, 1, gwstr("C:\\tool.exe"), 0), 0xffffffffu);

    /* _getch: no console */
    CHECK_EQ(CALL0(w32_imp_msvcr90__getch, 0), 0xffffffffu);
}
