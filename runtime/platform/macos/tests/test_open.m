/* test_open.m -- sp_mac_open_url (src/sp_open.m), the host side of ShellExecuteW.
 *   test_open            checks only; opens nothing
 *   test_open --real     also opens https://example.com once in the default browser (one tab appears)
 * Build: make check-open / check-open-real, or
 *   clang -fobjc-arc -Iinclude -Isrc -Itests src/sp_open.m tests/test_open.m -framework AppKit -o test_open */
#import <AppKit/AppKit.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "sp_macos.h"

static int pass, fail;
#define CHECK(c) do { if (c) pass++; else { fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void *off_main(void *arg) {
    (void)arg;
    /* Off the main thread a refused URL still returns 0 and queues nothing. */
    return (void *)(intptr_t)sp_mac_open_url("file:///etc/hosts");
}

int main(int argc, char **argv) {
    int real = argc > 1 && !strcmp(argv[1], "--real");
    pthread_t t;
    void *r = (void *)1;

    /* The HLE finds the opener in the executable by name, as the game binary will. */
    CHECK(dlsym(RTLD_DEFAULT, "sp_mac_open_url") == (void *)sp_mac_open_url);

    /* Accepted (a default browser is registered on any Mac). */
    CHECK(sp_mac_open_url_check("https://example.com"));
    CHECK(sp_mac_open_url_check("http://www.spore.com"));
    CHECK(sp_mac_open_url_check("http://youtube.com/t/terms"));
    CHECK(sp_mac_open_url_check("HTTPS://Example.com/a?b=c#d"));

    /* Refused here too (defence in depth behind the HLE filter); sp_mac_open_url opens nothing for them. */
    static const char *bad[] = {
        NULL, "", "file:///etc/hosts", "mailto:a@example.com", "javascript:alert(1)", "data:text/html,x",
        "ftp://example.com/", "/Applications/Calculator.app", "README.txt", "C:\\Spore\\README.txt",
        "https:///nohost", "http://", "x-apple.systempreferences:", "facetime://123", "vnc://host",
        "smb://host/share", "https://exa mple.com",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(!sp_mac_open_url_check(bad[i]));
        CHECK(!sp_mac_open_url(bad[i]));
    }
    pthread_create(&t, NULL, off_main, NULL);
    pthread_join(t, &r);
    CHECK(r == NULL);

    if (real) {
        printf("test_open: opening https://example.com in the default browser (one tab)\n");
        CHECK(sp_mac_open_url("https://example.com") == 1);
    }
    printf("test_open: %d passed, %d failed%s\n", pass, fail, real ? " (one real open)" : "");
    return fail ? 1 : 0;
}
