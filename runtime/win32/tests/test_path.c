/* test_path.c -- GetFullPathName rules, the mount table, case-insensitive resolution and
 * wildcard matching. Set SPORE_W32_CS_DIR to a directory on a case-sensitive volume to test
 * the case-insensitive fallback scan for real (make test-cs does this with a disk image). */
#include "harness.h"
#include "w32_int.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *full(const char *in) {
    static char b[4][W32_PATH_CAP];
    static int k;
    char *o = b[k++ & 3];
    if (w32_path_full(in, o, W32_PATH_CAP) < 0) return "(fail)";
    return o;
}

static void touch(const char *host) {
    FILE *f = fopen(host, "wb");
    if (f) { fputs("x", f); fclose(f); }
}

void test_path(void) {
    char host[W32_PATH_CAP], p[W32_PATH_CAP];
    int ex, pex;

    /* current directory: the exe folder, as a GOG shortcut sets it */
    CHECK_STR(w32_path_cwd(), "C:\\Spore\\SporebinEP1");
    CHECK_STR(w32_folder(W32_F_EXE), "C:\\Spore\\SporebinEP1\\SporeApp.exe");

    /* normalisation */
    CHECK_STR(full("C:\\a\\b\\..\\c"), "C:\\a\\c");
    CHECK_STR(full("c:/x/./y//z"), "c:\\x\\y\\z");
    CHECK_STR(full("..\\Data\\Config\\ConfigManager.txt"), "C:\\Spore\\Data\\Config\\ConfigManager.txt");
    CHECK_STR(full("../EP1Data/"), "C:\\Spore\\EP1Data\\");
    CHECK_STR(full("Data"), "C:\\Spore\\SporebinEP1\\Data");
    CHECK_STR(full("\\Windows"), "C:\\Windows");
    CHECK_STR(full("C:relative"), "C:\\Spore\\SporebinEP1\\relative");
    CHECK_STR(full("D:x"), "D:\\x");
    CHECK_STR(full("C:\\..\\..\\a"), "C:\\a");
    CHECK_STR(full("C:\\dir\\name. . "), "C:\\dir\\name");
    CHECK_STR(full("\\\\?\\C:\\long\\path"), "C:\\long\\path");
    CHECK_STR(full("\\\\server\\share\\x"), "(fail)");
    CHECK_STR(full("\\\\.\\C:"), "(fail)");
    CHECK_STR(full("C:\\"), "C:\\");
    CHECK_STR(full("C:\\a\\"), "C:\\a\\");

    /* mounts: install root, AppData, Documents, temp, and C:\ for the rest */
    CHECK_EQ(w32_path_to_host("C:\\Spore\\SporebinEP1\\SporeApp.exe", host, sizeof host, &ex, &pex), 0);
    snprintf(p, sizeof p, "%s/game/SporebinEP1/SporeApp.exe", t_root);
    CHECK_STR(host, p);
    CHECK_EQ(ex, 0);
    CHECK_EQ(pex, 1);
    CHECK_EQ(w32_path_to_host("C:\\Users\\Player\\AppData\\Roaming\\Spore\\Preferences", host, sizeof host, &ex, &pex), 0);
    snprintf(p, sizeof p, "%s/support/AppData/Spore/Preferences", t_root);
    CHECK_STR(host, p);
    CHECK_EQ(pex, 0);
    CHECK_EQ(w32_path_to_host("c:\\users\\PLAYER\\documents\\My Spore Creations", host, sizeof host, &ex, &pex), 0);
    snprintf(p, sizeof p, "%s/docs/My Spore Creations", t_root);
    CHECK_STR(host, p);
    CHECK_EQ(w32_path_to_host("C:\\Users\\Player\\AppData\\Local\\Temp\\x.tmp", host, sizeof host, &ex, &pex), 0);
    snprintf(p, sizeof p, "%s/tmp/x.tmp", t_root);
    CHECK_STR(host, p);
    CHECK_EQ(pex, 1);
    CHECK_EQ(w32_path_to_host("C:\\Windows\\system32", host, sizeof host, &ex, &pex), 0);
    snprintf(p, sizeof p, "%s/support/drive_c/Windows/system32", t_root);
    CHECK_STR(host, p);
    CHECK_EQ(ex, 1); /* the standard folders are created at start-up */
    CHECK_EQ(w32_path_to_host("C:\\Sporex\\a", host, sizeof host, &ex, &pex), 0); /* not the install mount */
    snprintf(p, sizeof p, "%s/support/drive_c/Sporex/a", t_root);
    CHECK_STR(host, p);
    CHECK_EQ(w32_path_to_host("D:\\x", host, sizeof host, &ex, &pex), -1); /* no D: drive */
    CHECK_EQ(w32_path_to_host("C:\\Spore", host, sizeof host, &ex, &pex), 0);
    CHECK_EQ(ex, 1);

    /* case-insensitive resolution on the real tree */
    snprintf(p, sizeof p, "%s/game/Data", t_root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/game/Data/Spore_DDFList.txt", t_root);
    touch(p);
    CHECK_EQ(w32_path_to_host("C:\\SPORE\\data\\spore_ddflist.TXT", host, sizeof host, &ex, &pex), 0);
    CHECK_EQ(ex, 1);

    /* the case-insensitive fallback scan on a case-sensitive volume */
    const char *cs = getenv("SPORE_W32_CS_DIR");
    if (cs) {
        char d[W32_PATH_CAP];
        snprintf(d, sizeof d, "%s/w32test", cs);
        mkdir(d, 0755);
        snprintf(p, sizeof p, "%s/DataEP1", d);
        mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/DataEP1/Spore_EP1_Data.package", d);
        touch(p);
        CHECK_EQ(w32_path_mount("C:\\CS", d), 0);
        CHECK_EQ(w32_path_to_host("C:\\cs\\dataep1\\SPORE_ep1_data.PACKAGE", host, sizeof host, &ex, &pex), 0);
        CHECK_EQ(ex, 1);
        CHECK_STR(host, p); /* the stored case comes back */
        CHECK_EQ(w32_path_to_host("C:\\cs\\DATAEP1\\new.txt", host, sizeof host, &ex, &pex), 0);
        CHECK_EQ(ex, 0);
        CHECK_EQ(pex, 1);
        snprintf(p, sizeof p, "%s/DataEP1/new.txt", d);
        CHECK_STR(host, p); /* new names keep the guest's spelling below the real parent */
        printf("path tests: case-sensitive volume checks ran in %s\n", cs);
    } else {
        printf("path tests: SPORE_W32_CS_DIR not set; case-sensitive fallback not exercised (make test-cs)\n");
    }

    /* cwd changes */
    CHECK_EQ(w32_path_set_cwd("C:\\Spore\\Data"), 0);
    CHECK_STR(full("Config"), "C:\\Spore\\Data\\Config");
    CHECK_EQ(w32_path_set_cwd("C:\\nonexistent"), -1);
    CHECK_EQ(w32_path_set_cwd(w32_folder(W32_F_EXE_DIR)), 0);

    /* wildcards */
    CHECK(w32_wildcard_match("*.package", "Spore_EP1_Data.package"));
    CHECK(w32_wildcard_match("*.PACKAGE", "a.package"));
    CHECK(!w32_wildcard_match("*.package", "a.packagex"));
    CHECK(w32_wildcard_match("*", "anything"));
    CHECK(w32_wildcard_match("*.*", "noext"));
    CHECK(w32_wildcard_match("Game?", "Game0"));
    CHECK(!w32_wildcard_match("Game?", "Game10"));
    CHECK(w32_wildcard_match("a*b*c", "aXXbYYc"));
    CHECK(!w32_wildcard_match("a*b*c", "aXXbYY"));
    CHECK(w32_wildcard_match("readme.*", "README"));
    CHECK(w32_wildcard_match("*.png", "Créature.PNG"));

    /* name folding */
    CHECK_EQ(w32_name_casecmp("ÉCOLE", "école"), 0);
    CHECK(w32_name_casecmp("a", "b") < 0);
    CHECK_EQ(w32_name_casecmp("My Spore Creations", "my spore creations"), 0);
}
