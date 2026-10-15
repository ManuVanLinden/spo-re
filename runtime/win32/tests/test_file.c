/* test_file.c -- the KERNEL32 file layer and msvcr90 stdio, through the shims. */
#include "harness.h"
#include "w32_int.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define GR 0x80000000u
#define GW 0x40000000u
#define INV 0xffffffffu

static uint32_t create_w(const char *p, uint32_t acc, uint32_t disp, uint32_t flags) {
    return CALL(w32_imp_kernel32_CreateFileW, 28, gwstr(p), acc, 1, 0, disp, flags, 0);
}

static char *host_read(const char *rel, size_t *n) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", t_root, rel);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    static char buf[4096];
    *n = fread(buf, 1, sizeof buf - 1, f);
    buf[*n] = 0;
    fclose(f);
    return buf;
}

static void host_write(const char *rel, const char *text) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", t_root, rel);
    FILE *f = fopen(p, "wb");
    fputs(text, f);
    fclose(f);
}

void w32_io_report(FILE *out);

void test_file(void) {
    uint32_t buf = gbuf(4096), n = gbuf(4), hi = gbuf(4);
    size_t hn;
    char p[1200];

    /* game layout: Data/Config under the install root, mixed case */
    snprintf(p, sizeof p, "%s/game/Data", t_root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/game/Data/Config", t_root); mkdir(p, 0755);
    host_write("game/Data/Config/ConfigManager.txt", "line1\r\nline2\r\n");
    host_write("game/Data/Spore_DDFList.txt", "x");
    host_write("game/Data/b.package", "bb");
    host_write("game/Data/A.package", "a");
    host_write("game/Data/.DS_Store", "junk");

    /* CreateFileW: open existing, read, size, seek */
    uint32_t h = create_w("..\\Data\\config\\CONFIGMANAGER.TXT", GR, 3 /* OPEN_EXISTING */, 0x80);
    CHECK(h != INV);
    CHECK_EQ(last_error(), 0);
    CHECK_EQ(CALL(w32_imp_kernel32_GetFileSize, 8, h, hi), 14);
    CHECK_EQ(sg_ld32(M, hi), 0);
    CHECK_EQ(CALL(w32_imp_kernel32_ReadFile, 20, h, buf, 5, n, 0), 1);
    CHECK_EQ(sg_ld32(M, n), 5);
    CHECK(!memcmp(M + buf, "line1", 5));
    CHECK_EQ(CALL(w32_imp_kernel32_SetFilePointer, 16, h, 2, 0, 1 /* FILE_CURRENT */), 7);
    CHECK_EQ(CALL(w32_imp_kernel32_ReadFile, 20, h, buf, 100, n, 0), 1);
    CHECK_EQ(sg_ld32(M, n), 7); /* short read at end of file is success */
    CHECK_EQ(CALL(w32_imp_kernel32_ReadFile, 20, h, buf, 100, n, 0), 1);
    CHECK_EQ(sg_ld32(M, n), 0);
    CHECK_EQ(CALL(w32_imp_kernel32_SetFilePointer, 16, h, (uint32_t)-3, 0, 2 /* FILE_END */), 11);
    CHECK_EQ(CALL(w32_imp_kernel32_SetFilePointer, 16, h, (uint32_t)-100, 0, 1), INV);
    CHECK_EQ(last_error(), W32_ERROR_NEGATIVE_SEEK);
    sg_st32(M, hi, 0);
    CHECK_EQ(CALL(w32_imp_kernel32_SetFilePointer, 16, h, 1, hi, 0), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_WriteFile, 20, h, buf, 1, n, 0), 0); /* read-only handle */
    CHECK_EQ(last_error(), W32_ERROR_ACCESS_DENIED);
    CHECK_EQ(CALL(w32_imp_kernel32_GetFileType, 4, h), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_CloseHandle, 4, h), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_ReadFile, 20, h, buf, 1, n, 0), 0);
    CHECK_EQ(last_error(), W32_ERROR_INVALID_HANDLE);

    /* per-path I/O accounting (w32_io_report) counts the 3 reads / 12 bytes above */
    {
        char *rep = NULL;
        size_t rn = 0;
        FILE *mf = open_memstream(&rep, &rn);
        w32_io_report(mf);
        fclose(mf);
        CHECK(rep && strstr(rep, "reads 3 (12 bytes, furthest 14) writes 0 (0 bytes)") && strcasestr(rep, "configmanager.txt"));
        free(rep);
    }

    /* 64-bit offsets (packages near 1 GB, and the API allows more): seek past 4 GiB in a
     * sparse file through lpDistanceToMoveHigh, write, then read the byte back */
    h = create_w("C:\\Spore\\Data\\big.bin", GR | GW, 2 /* CREATE_ALWAYS */, 0x80);
    CHECK(h != INV);
    sg_st32(M, hi, 1);
    CHECK_EQ(CALL(w32_imp_kernel32_SetFilePointer, 16, h, 0x10, hi, 0), 0x10); /* 4 GiB + 16 */
    CHECK_EQ(sg_ld32(M, hi), 1);
    M[buf] = 'Z';
    CHECK_EQ(CALL(w32_imp_kernel32_WriteFile, 20, h, buf, 1, n, 0), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_GetFileSize, 8, h, hi), 0x11);
    CHECK_EQ(sg_ld32(M, hi), 1);
    sg_st32(M, hi, 1);
    CHECK_EQ(CALL(w32_imp_kernel32_SetFilePointer, 16, h, 0x10, hi, 0), 0x10);
    M[buf] = 0;
    CHECK_EQ(CALL(w32_imp_kernel32_ReadFile, 20, h, buf, 4096, n, 0), 1);
    CHECK_EQ(sg_ld32(M, n), 1);
    CHECK_EQ(M[buf], 'Z');
    sg_st32(M, hi, 0xffffffffu); /* FILE_END - 1 */
    CHECK_EQ(CALL(w32_imp_kernel32_SetFilePointer, 16, h, 0xffffffffu, hi, 2), 0x10);
    CHECK_EQ(sg_ld32(M, hi), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_CloseHandle, 4, h), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_DeleteFileW, 4, gwstr("C:\\Spore\\Data\\big.bin")), 1);

    /* dispositions and their errors */
    CHECK_EQ(create_w("C:\\Spore\\Data\\nope.txt", GR, 3, 0), INV);
    CHECK_EQ(last_error(), W32_ERROR_FILE_NOT_FOUND);
    CHECK_EQ(create_w("C:\\Spore\\NoDir\\x.txt", GR, 3, 0), INV);
    CHECK_EQ(last_error(), W32_ERROR_PATH_NOT_FOUND);
    CHECK_EQ(create_w("C:\\Spore\\NoDir\\x.txt", GW, 2, 0), INV);
    CHECK_EQ(last_error(), W32_ERROR_PATH_NOT_FOUND);
    h = create_w("C:\\Spore\\Data\\new.bin", GR | GW, 1 /* CREATE_NEW */, 0);
    CHECK(h != INV);
    CHECK_EQ(last_error(), 0);
    memcpy(M + buf, "hello world", 11);
    CHECK_EQ(CALL(w32_imp_kernel32_WriteFile, 20, h, buf, 11, n, 0), 1);
    CHECK_EQ(sg_ld32(M, n), 11);
    CHECK_EQ(CALL(w32_imp_kernel32_SetFilePointer, 16, h, 5, 0, 0), 5);
    CHECK_EQ(CALL(w32_imp_kernel32_SetEndOfFile, 4, h), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_FlushFileBuffers, 4, h), 1);
    CALL(w32_imp_kernel32_CloseHandle, 4, h);
    CHECK_STR(host_read("game/Data/new.bin", &hn), "hello");
    CHECK_EQ(create_w("C:\\Spore\\Data\\new.bin", GW, 1, 0), INV);
    CHECK_EQ(last_error(), W32_ERROR_FILE_EXISTS);
    h = create_w("C:\\Spore\\Data\\new.bin", GW, 2 /* CREATE_ALWAYS */, 0);
    CHECK(h != INV);
    CHECK_EQ(last_error(), W32_ERROR_ALREADY_EXISTS);
    CALL(w32_imp_kernel32_CloseHandle, 4, h);
    CHECK_STR(host_read("game/Data/new.bin", &hn), "");
    h = create_w("C:\\Spore\\Data\\open-always.txt", GW, 4 /* OPEN_ALWAYS */, 0);
    CHECK_EQ(last_error(), 0);
    CALL(w32_imp_kernel32_CloseHandle, 4, h);
    h = create_w("C:\\Spore\\Data\\open-always.txt", GW, 4, 0);
    CHECK_EQ(last_error(), W32_ERROR_ALREADY_EXISTS);
    CALL(w32_imp_kernel32_CloseHandle, 4, h);
    CHECK_EQ(create_w("C:\\Spore\\Data", GR, 3, 0), INV); /* directory without BACKUP_SEMANTICS */
    CHECK_EQ(last_error(), W32_ERROR_ACCESS_DENIED);
    h = create_w("C:\\Spore\\Data", 1, 3, 0x02000000u | 0x40000000u); /* the watcher's open (0x92fdc8) */
    CHECK(h != INV);
    CHECK_EQ(CALL(w32_imp_kernel32_CloseHandle, 4, h), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_CreateFileA, 28, gstr("\\\\.\\C:"), 0, 3, 0, 3, 0, 0), INV); /* volume device */
    h = create_w("C:\\Spore\\Data\\tmp.del", GW, 2, 0x04000000u); /* DELETE_ON_CLOSE */
    CALL(w32_imp_kernel32_CloseHandle, 4, h);
    CHECK(host_read("game/Data/tmp.del", &hn) == NULL);
    /* overlapped-style read at an offset */
    h = create_w("C:\\Spore\\Data\\Config\\ConfigManager.txt", GR, 3, 0);
    uint32_t ovl = gbuf(20);
    sg_st32(M, ovl + 8, 7);
    CHECK_EQ(CALL(w32_imp_kernel32_ReadFile, 20, h, buf, 5, n, ovl), 1);
    CHECK(!memcmp(M + buf, "line2", 5));
    CHECK_EQ(sg_ld32(M, ovl + 4), 5);
    CALL(w32_imp_kernel32_CloseHandle, 4, h);
    /* a handle we do not own goes to the foreign hook; the core-facing closer refuses it */
    CHECK_EQ(CALL(w32_imp_kernel32_CloseHandle, 4, 0x1234), 1);
    CHECK_EQ(w32_close_own_handle(&T, 0x1234), 0);
    CHECK_EQ(last_error(), W32_ERROR_INVALID_HANDLE);
    h = create_w("C:\\Spore\\Data\\Config\\ConfigManager.txt", GR, 3, 0);
    CHECK_EQ(w32_close_own_handle(&T, h), 1);
    CHECK_EQ(w32_close_own_handle(&T, h), 0);

    /* attributes */
    CHECK_EQ(CALL(w32_imp_kernel32_GetFileAttributesW, 4, gwstr("C:\\Spore\\data")), 0x10);
    CHECK_EQ(CALL(w32_imp_kernel32_GetFileAttributesW, 4, gwstr("C:\\Spore\\Data\\a.PACKAGE")), 0x20);
    CHECK_EQ(CALL(w32_imp_kernel32_GetFileAttributesW, 4, gwstr("C:\\Spore\\Data\\zz")), INV);
    CHECK_EQ(last_error(), W32_ERROR_FILE_NOT_FOUND);
    CHECK_EQ(CALL(w32_imp_kernel32_GetFileAttributesW, 4, gwstr("C:\\Spore\\zz\\zz")), INV);
    CHECK_EQ(last_error(), W32_ERROR_PATH_NOT_FOUND);
    CHECK_EQ(CALL(w32_imp_kernel32_SetFileAttributesW, 8, gwstr("C:\\Spore\\Data\\A.package"), 1), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_GetFileAttributesW, 4, gwstr("C:\\Spore\\Data\\A.package")), 0x21);
    CALL(w32_imp_kernel32_SetFileAttributesW, 8, gwstr("C:\\Spore\\Data\\A.package"), 0x80);

    /* FindFirstFileW: pattern, case-insensitive sort, . and .., no macOS metadata */
    uint32_t fd = gbuf(592);
    uint32_t fh = CALL(w32_imp_kernel32_FindFirstFileW, 8, gwstr("C:\\Spore\\Data\\*"), fd);
    CHECK(fh != INV);
    const char *want[] = {".", "..", "A.package", "b.package", "Config", "new.bin", "open-always.txt", "Spore_DDFList.txt"};
    int k = 0, order_ok = 1;
    do {
        const char *nm = hwstr(fd + 44);
        if (k < 8 && strcmp(nm, want[k])) { order_ok = 0; fprintf(stderr, "  find[%d] = %s want %s\n", k, nm, want[k]); }
        if (!strcmp(nm, "Config")) CHECK_EQ(sg_ld32(M, fd), 0x10);
        if (!strcmp(nm, "b.package")) CHECK_EQ(sg_ld32(M, fd + 32), 2);
        k++;
    } while (CALL(w32_imp_kernel32_FindNextFileW, 8, fh, fd));
    CHECK_EQ(last_error(), W32_ERROR_NO_MORE_FILES);
    CHECK_EQ(k, 8);
    CHECK(order_ok);
    CHECK_EQ(CALL(w32_imp_kernel32_FindClose, 4, fh), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_FindClose, 4, fh), 0);
    fh = CALL(w32_imp_kernel32_FindFirstFileW, 8, gwstr("C:/Spore/Data/*.PACKAGE"), fd);
    CHECK_STR(hwstr(fd + 44), "A.package");
    CHECK_EQ(CALL(w32_imp_kernel32_FindNextFileW, 8, fh, fd), 1);
    CHECK_STR(hwstr(fd + 44), "b.package");
    CHECK_EQ(CALL(w32_imp_kernel32_FindNextFileW, 8, fh, fd), 0);
    CALL(w32_imp_kernel32_FindClose, 4, fh);
    CHECK_EQ(CALL(w32_imp_kernel32_FindFirstFileW, 8, gwstr("C:\\Spore\\Data\\*.xyz"), fd), INV);
    CHECK_EQ(last_error(), W32_ERROR_FILE_NOT_FOUND);
    CHECK_EQ(CALL(w32_imp_kernel32_FindFirstFileW, 8, gwstr("C:\\Spore\\Nope\\*"), fd), INV);
    CHECK_EQ(last_error(), W32_ERROR_PATH_NOT_FOUND);
    uint32_t fda = gbuf(320);
    fh = CALL(w32_imp_kernel32_FindFirstFileA, 8, gstr("..\\data\\spore_ddflist.txt"), fda);
    CHECK(fh != INV);
    CHECK_STR((const char *)M + fda + 44, "Spore_DDFList.txt");
    CALL(w32_imp_kernel32_FindClose, 4, fh);

    /* directories, delete, move, copy */
    CHECK_EQ(CALL(w32_imp_kernel32_CreateDirectoryW, 8, gwstr("C:\\Users\\Player\\AppData\\Roaming\\Spore"), 0), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_CreateDirectoryW, 8, gwstr("C:\\Users\\Player\\AppData\\Roaming\\Spore"), 0), 0);
    CHECK_EQ(last_error(), W32_ERROR_ALREADY_EXISTS);
    CHECK_EQ(CALL(w32_imp_kernel32_CreateDirectoryW, 8, gwstr("C:\\Users\\Player\\AppData\\Roaming\\Spore\\a\\b"), 0), 0);
    CHECK_EQ(last_error(), W32_ERROR_PATH_NOT_FOUND);
    CHECK_EQ(CALL(w32_imp_kernel32_CreateDirectoryA, 8, gstr("C:\\Users\\Player\\AppData\\Roaming\\Spore\\Preferences"), 0), 1);
    snprintf(p, sizeof p, "%s/support/AppData/Spore/Preferences", t_root);
    struct stat st;
    CHECK(stat(p, &st) == 0 && S_ISDIR(st.st_mode));
    h = create_w("C:\\Users\\Player\\AppData\\Roaming\\Spore\\Preferences\\Options.txt", GW, 2, 0);
    memcpy(M + buf, "opt", 3);
    CALL(w32_imp_kernel32_WriteFile, 20, h, buf, 3, n, 0);
    CALL(w32_imp_kernel32_CloseHandle, 4, h);
    CHECK_STR(host_read("support/AppData/Spore/Preferences/Options.txt", &hn), "opt");
    CHECK_EQ(CALL(w32_imp_kernel32_CopyFileW, 12, gwstr("%APPDATA%"), gwstr("x"), 0), 0); /* no expansion: not found */
    const char *pref = "C:\\Users\\Player\\AppData\\Roaming\\Spore\\Preferences\\";
    char a1[300], a2[300];
    snprintf(a1, sizeof a1, "%sOptions.txt", pref);
    snprintf(a2, sizeof a2, "%sOptions.bak", pref);
    CHECK_EQ(CALL(w32_imp_kernel32_CopyFileW, 12, gwstr(a1), gwstr(a2), 1), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_CopyFileW, 12, gwstr(a1), gwstr(a2), 1), 0);
    CHECK_EQ(last_error(), W32_ERROR_FILE_EXISTS);
    CHECK_EQ(CALL(w32_imp_kernel32_MoveFileExW, 12, gwstr(a2), gwstr(a1), 0), 0);
    CHECK_EQ(last_error(), W32_ERROR_ALREADY_EXISTS);
    CHECK_EQ(CALL(w32_imp_kernel32_MoveFileExW, 12, gwstr(a2), gwstr(a1), 1 /* REPLACE_EXISTING */), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_GetFileAttributesW, 4, gwstr(a2)), INV);
    snprintf(a2, sizeof a2, "%sOptions2.txt", pref);
    CHECK_EQ(CALL(w32_imp_kernel32_MoveFileA, 8, gstr(a1), gstr(a2)), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_DeleteFileW, 4, gwstr(a2)), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_DeleteFileW, 4, gwstr(a2)), 0);
    CHECK_EQ(last_error(), W32_ERROR_FILE_NOT_FOUND);
    snprintf(p, sizeof p, "%s/support/AppData/Spore/Preferences/.DS_Store", t_root);
    host_write("support/AppData/Spore/Preferences/.DS_Store", "x");
    CHECK_EQ(CALL(w32_imp_kernel32_RemoveDirectoryW, 4, gwstr("C:\\Users\\Player\\AppData\\Roaming\\Spore")), 0);
    CHECK_EQ(last_error(), W32_ERROR_DIR_NOT_EMPTY);
    CHECK_EQ(CALL(w32_imp_kernel32_RemoveDirectoryW, 4, gwstr("C:\\Users\\Player\\AppData\\Roaming\\Spore\\Preferences")), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_RemoveDirectoryA, 4, gstr("C:\\Users\\Player\\AppData\\Roaming\\Spore\\x")), 0);
    CHECK_EQ(last_error(), W32_ERROR_FILE_NOT_FOUND);

    /* path queries */
    uint32_t part = gbuf(4);
    CHECK_EQ(CALL(w32_imp_kernel32_GetFullPathNameW, 16, gwstr("..\\Data\\x.txt"), 260, buf, part), 19);
    CHECK_STR(hwstr(buf), "C:\\Spore\\Data\\x.txt");
    CHECK_EQ(sg_ld32(M, part), buf + 2 * 14);
    CHECK_EQ(CALL(w32_imp_kernel32_GetFullPathNameW, 16, gwstr("..\\Data\\x.txt"), 5, buf, part), 20); /* needed incl. NUL */
    CHECK_EQ(CALL(w32_imp_kernel32_GetFullPathNameA, 16, gstr("C:\\a\\"), 260, buf, part), 5);
    CHECK_EQ(sg_ld32(M, part), 0);
    CHECK_EQ(CALL(w32_imp_kernel32_GetTempPathW, 8, 260, buf), 35);
    CHECK_STR(hwstr(buf), "C:\\Users\\Player\\AppData\\Local\\Temp\\");
    CHECK_EQ(CALL(w32_imp_kernel32_GetTempPathW, 8, 10, buf), 36);
    CHECK_EQ(CALL(w32_imp_kernel32_GetCurrentDirectoryA, 8, 260, buf), 20);
    CHECK_STR((const char *)M + buf, "C:\\Spore\\SporebinEP1");
    CHECK_EQ(CALL(w32_imp_kernel32_GetWindowsDirectoryW, 8, buf, 260), 10);
    CHECK_EQ(CALL(w32_imp_kernel32_GetSystemDirectoryW, 8, buf, 260), 19);
    CHECK_EQ(CALL(w32_imp_kernel32_GetLongPathNameW, 12, gwstr("C:\\SPORE\\data"), buf, 260), 13);
    CHECK_STR(hwstr(buf), "C:\\SPORE\\data");
    CHECK_EQ(CALL(w32_imp_kernel32_GetLongPathNameW, 12, gwstr("C:\\SPORE\\nothing"), buf, 260), 0);
    CHECK_EQ(CALL(w32_imp_shell32_SHGetFolderPathW, 20, 0, 5, 0, 0, buf), 0);
    CHECK_STR(hwstr(buf), "C:\\Users\\Player\\Documents");
    CHECK_EQ(CALL(w32_imp_shell32_SHGetFolderPathW, 20, 0, 0x801a, 0, 0, buf), 0);
    CHECK_STR(hwstr(buf), "C:\\Users\\Player\\AppData\\Roaming");
    CHECK_EQ(CALL(w32_imp_shell32_SHGetFolderPathW, 20, 0, 0x2e, 0, 0, buf), 0);
    CHECK_EQ(CALL(w32_imp_shell32_SHGetFolderPathW, 20, 0, 0x99, 0, 0, buf), 0x80070057u);

    /* volumes */
    uint32_t ser = gbuf(4), fsn = gbuf(64);
    CHECK_EQ(CALL(w32_imp_kernel32_GetVolumeInformationA, 32, gstr("C:\\"), buf, 64, ser, 0, 0, fsn, 64), 1);
    CHECK_EQ(sg_ld32(M, ser), 0x53504f52u);
    CHECK_STR((const char *)M + fsn, "NTFS");
    CHECK_STR((const char *)M + buf, ""); /* no label, so the "yzexo" probe (0x606530) never matches */
    CHECK_EQ(CALL(w32_imp_kernel32_GetVolumeInformationA, 32, gstr("D:\\"), buf, 64, ser, 0, 0, fsn, 64), 0);
    CHECK_EQ(CALL(w32_imp_kernel32_GetDriveTypeA, 4, gstr("C:\\")), 3);
    CHECK_EQ(CALL(w32_imp_kernel32_GetDriveTypeA, 4, gstr("E:\\")), 1);
    CHECK_EQ(CALL0(w32_imp_kernel32_GetLogicalDrives, 0), 4);
    uint32_t q = gbuf(24);
    CHECK_EQ(CALL(w32_imp_kernel32_GetDiskFreeSpaceExA, 16, gstr("C:\\"), q, q + 8, q + 16), 1);
    CHECK(sg_ld64(M, q + 8) > 0);
    CHECK_EQ(CALL(w32_imp_kernel32_GetVolumePathNameW, 12, gwstr("C:\\Spore\\Data\\x"), buf, 10), 1);
    CHECK_STR(hwstr(buf), "C:\\");
    CHECK_EQ(CALL(w32_imp_kernel32_DeviceIoControl, 32, 1, 0x560000, 0, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(CALL(w32_imp_kernel32_GetStdHandle, 4, 0xfffffff4u), 0); /* GUI process: no console */

    /* stdio: text mode writes CRLF, reads translate back; binary does not */
    uint32_t f = CALL(w32_imp_msvcr90_fopen, 0, gstr("C:\\Spore\\Data\\Logging.txt"), gstr("w"));
    CHECK(f != 0);
    CHECK_EQ(CALL(w32_imp_msvcr90_fprintf, 0, f, gstr("%s=%d\n"), gstr("frames"), 60), 10);
    CHECK_EQ(CALL(w32_imp_msvcr90_fputs, 0, gstr("end\n"), f), 0);
    memcpy(M + buf, "raw", 3);
    CHECK_EQ(CALL(w32_imp_msvcr90_fwrite, 0, buf, 1, 3, f), 3);
    CHECK_EQ(CALL(w32_imp_msvcr90_ftell, 0, f), 19);
    CHECK_EQ(CALL(w32_imp_msvcr90_fclose, 0, f), 0);
    CHECK_STR(host_read("game/Data/Logging.txt", &hn), "frames=60\r\nend\r\nraw");
    f = CALL(w32_imp_msvcr90_fopen, 0, gstr("..\\data\\logging.TXT"), gstr("r"));
    CHECK_EQ(CALL(w32_imp_msvcr90_fgets, 0, buf, 100, f), buf);
    CHECK_STR((const char *)M + buf, "frames=60\n");
    CHECK_EQ(CALL(w32_imp_msvcr90_ftell, 0, f), 11);
    CHECK_EQ(CALL(w32_imp_msvcr90_fread, 0, buf, 1, 100, f), 7);
    CHECK(!memcmp(M + buf, "end\nraw", 7));
    CHECK(CALL(w32_imp_msvcr90_feof, 0, f) != 0);
    CHECK_EQ(CALL(w32_imp_msvcr90_fseek, 0, f, 0, 0), 0);
    CHECK_EQ(CALL(w32_imp_msvcr90_feof, 0, f), 0);
    CHECK_EQ(CALL(w32_imp_msvcr90_fseek, 0, f, -3, 2), 0);
    CHECK_EQ(CALL(w32_imp_msvcr90_fread, 0, buf, 1, 3, f), 3);
    CHECK(!memcmp(M + buf, "raw", 3));
    CHECK_EQ(CALL(w32_imp_msvcr90_ferror, 0, f), 0);
    CALL(w32_imp_msvcr90_fclose, 0, f);
    f = CALL(w32_imp_msvcr90_fopen, 0, gstr("C:\\Spore\\Data\\Logging.txt"), gstr("rb"));
    CHECK_EQ(CALL(w32_imp_msvcr90_fread, 0, buf, 1, 100, f), 19);
    CALL(w32_imp_msvcr90_fclose, 0, f);
    f = CALL(w32_imp_msvcr90_fopen, 0, gstr("C:\\Spore\\Data\\Logging.txt"), gstr("a"));
    CALL(w32_imp_msvcr90_fputs, 0, gstr("!"), f);
    CALL(w32_imp_msvcr90_fclose, 0, f);
    CHECK_STR(host_read("game/Data/Logging.txt", &hn), "frames=60\r\nend\r\nraw!");
    CHECK_EQ(CALL(w32_imp_msvcr90_fopen, 0, gstr("C:\\Spore\\nodir\\x"), gstr("w")), 0);
    CHECK_EQ(hle_get_errno(M), MSVC_ENOENT);
    CHECK_EQ(CALL(w32_imp_msvcr90_fopen, 0, gstr("C:\\Spore\\Data\\missing"), gstr("r")), 0);
    /* binary _fmode switches the default */
    sg_st32(M, CALL0(w32_imp_msvcr90___p__fmode, 0), 0x8000);
    f = CALL(w32_imp_msvcr90_fopen, 0, gstr("C:\\Spore\\Data\\bin.txt"), gstr("w"));
    CALL(w32_imp_msvcr90_fputs, 0, gstr("a\n"), f);
    CALL(w32_imp_msvcr90_fclose, 0, f);
    CHECK_STR(host_read("game/Data/bin.txt", &hn), "a\n");
    sg_st32(M, CALL0(w32_imp_msvcr90___p__fmode, 0), 0);
    /* std streams: console-less, output reaches the log, calls succeed */
    uint32_t iob = CALL0(w32_imp_msvcr90___iob_func, 0);
    CHECK(iob != 0);
    CHECK_EQ(CALL(w32_imp_msvcr90_fprintf, 0, iob + 64, gstr("to stderr %d\n"), 7), 12);
    CHECK_EQ(CALL(w32_imp_msvcr90_fflush, 0, 0), 0);
    /* tmpfile (libjpeg) */
    f = CALL0(w32_imp_msvcr90_tmpfile, 0);
    CHECK(f != 0);
    CHECK_EQ(CALL(w32_imp_msvcr90_fwrite, 0, buf, 1, 3, f), 3);
    CHECK_EQ(CALL(w32_imp_msvcr90_fseek, 0, f, 0, 0), 0);
    CHECK_EQ(CALL(w32_imp_msvcr90_fread, 0, buf + 100, 1, 3, f), 3);
    CALL(w32_imp_msvcr90_fclose, 0, f);

    /* _stat64i32 / _wstat64i32 */
    uint32_t sb = gbuf(48);
    CHECK_EQ(CALL(w32_imp_msvcr90__wstat64i32, 0, gwstr("C:\\Spore\\Data\\b.package"), sb), 0);
    CHECK_EQ(sg_ld32(M, sb + 20), 2);
    CHECK_EQ(sg_ld16(M, sb + 6), 0x8000 | 0x1b6); /* _S_IFREG, read+write copied to group/other */
    CHECK_EQ(CALL(w32_imp_msvcr90__stat64i32, 0, gstr("C:\\Spore\\Data"), sb), 0);
    CHECK_EQ(sg_ld16(M, sb + 6) & 0xf000, 0x4000);
    CHECK_EQ(CALL(w32_imp_msvcr90__stat64i32, 0, gstr("C:\\Spore\\Data\\"), sb), (uint32_t)-1);
    CHECK_EQ(CALL(w32_imp_msvcr90__stat64i32, 0, gstr("C:\\Spore\\SporebinEP1\\SporeApp.exe"), sb), (uint32_t)-1);

    /* current directory through the CRT */
    uint32_t cw = CALL(w32_imp_msvcr90__wgetcwd, 0, 0, 0);
    CHECK_STR(hwstr(cw), "C:\\Spore\\SporebinEP1");
    CHECK_EQ(CALL(w32_imp_msvcr90__wgetcwd, 0, buf, 5), 0);
    CHECK_EQ(hle_get_errno(M), MSVC_ERANGE);
    CHECK_EQ(CALL0(w32_imp_msvcr90__getdrive, 0), 3);
    CHECK_EQ(CALL(w32_imp_msvcr90__wgetdcwd, 0, 3, buf, 260), buf);
    CHECK_EQ(CALL(w32_imp_msvcr90__wgetdcwd, 0, 4, buf, 260), 0);
}
