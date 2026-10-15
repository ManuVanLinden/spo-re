/* ShellExecuteW: the refusal matrix and the URL handed to the host opener, with a test opener
 * that records the URL instead of opening a browser. Nothing here opens anything. */
#include <stdlib.h>

#include "harness.h"

typedef int (*hle_url_opener)(const char *utf8);
hle_url_opener hle_shell_set_opener(hle_url_opener fn);

static char got_url[4096];
static int got_calls, opener_result = 1;

static int capture(const char *url) {
    snprintf(got_url, sizeof got_url, "%s", url);
    got_calls++;
    return opener_result;
}

/* ShellExecuteW(hwnd 0, op, file, params, NULL, SW_SHOWNORMAL); op/file/params are ASCII or NULL. */
static uint32_t shex(const char *op, const char *file, const char *params) {
    return CALL(sg_imp_shell32_ShellExecuteW, 24, 0, op ? gwstr(op) : 0, file ? gwstr(file) : 0,
                params ? gwstr(params) : 0, 0, 1);
}

static void expect_open(const char *op, const char *url) {
    int before = got_calls;
    hle_shell_set_opener(capture); /* also clears the repeat filter */
    got_url[0] = 0;
    CHECK_EQ(shex(op, url, NULL), 42);
    CHECK_EQ(got_calls, before + 1);
    CHECK_STR(got_url, url);
}

static void expect_refused(const char *op, const char *file, const char *params) {
    int before = got_calls;
    hle_shell_set_opener(capture);
    CHECK_EQ(shex(op, file, params), 5);
    CHECK_EQ(got_calls, before); /* the opener never sees a refused call */
}

void test_shell(void) {
    unsetenv("SPORE_OPEN_LINKS");

    /* What the game passes [obs]: the YouTube constants and the queued UI link seen in runs 078/082. */
    expect_open("open", "http://youtube.com/t/terms");
    expect_open("open", "http://www.youtube.com/signup");
    expect_open("open", "http://www.spore.com");
    expect_open("open", "https://example.com/a/b?c=d&e=f#g");
    expect_open(NULL, "https://example.com");           /* NULL verb = default verb = open */
    expect_open("OPEN", "HTTPS://Example.com/");         /* verbs and schemes are case-insensitive */
    expect_open("open", "http://user@host:8080/path");

    /* Refusals: verbs other than open. */
    expect_refused("edit", "http://www.spore.com", NULL);
    expect_refused("print", "https://example.com", NULL);
    expect_refused("runas", "https://example.com", NULL);
    expect_refused("explore", "https://example.com", NULL);
    expect_refused("", "https://example.com", NULL);
    /* Other schemes and file paths. */
    expect_refused("open", "file:///etc/passwd", NULL);
    expect_refused("open", "mailto:someone@example.com", NULL);
    expect_refused("open", "javascript:alert(1)", NULL);
    expect_refused("open", "data:text/html,<b>x</b>", NULL);
    expect_refused("open", "ftp://example.com/", NULL);
    expect_refused("open", "C:\\Spore\\SporebinEP1\\README.txt", NULL);
    expect_refused("open", "README.txt", NULL);
    expect_refused("open", "../../../Spore/SporeApp/README.txt", NULL);
    expect_refused("open", "\\\\server\\share\\x.exe", NULL);
    expect_refused("open", "/Applications/Calculator.app", NULL);
    expect_refused("open", "url:http://www.spore.com", NULL);
    expect_refused("open", "httpx://example.com", NULL);
    expect_refused("open", "http:example.com", NULL);
    expect_refused("open", "http://", NULL);
    expect_refused("open", "https:///path", NULL);
    expect_refused("open", " https://example.com", NULL);
    expect_refused("open", "https://example.com/a b", NULL);
    expect_refused("open", "https://example.com/\x01", NULL);
    expect_refused("open", "https://example.com\\..\\x", NULL);
    expect_refused("open", "", NULL);
    expect_refused("open", NULL, NULL);
    /* Parameters are never passed on. */
    expect_refused("open", "https://example.com", "--foo");
    {   /* non-ASCII (an IRI) is refused, not mangled */
        static const uint16_t iri[] = {'h', 't', 't', 'p', 's', ':', '/', '/', 'x', '.', 'c', 'o', 'm', '/', 0xe9, 0};
        int before = got_calls;
        hle_shell_set_opener(capture);
        CHECK_EQ(CALL(sg_imp_shell32_ShellExecuteW, 24, 0, gwstr("open"), gwstr16(iri), 0, 0, 1), 5);
        CHECK_EQ(got_calls, before);
    }
    {   /* a URL longer than the 2047-unit cap is refused rather than truncated */
        static char big[3000];
        int before = got_calls;
        memcpy(big, "https://example.com/", 20);
        memset(big + 20, 'a', sizeof big - 21);
        big[sizeof big - 1] = 0;
        hle_shell_set_opener(capture);
        CHECK_EQ(shex("open", big, NULL), 5);
        CHECK_EQ(got_calls, before);
    }

    /* SPORE_OPEN_LINKS=0 refuses everything, even a good URL. */
    setenv("SPORE_OPEN_LINKS", "0", 1);
    expect_refused("open", "https://example.com", NULL);
    expect_refused(NULL, "http://www.spore.com", NULL);
    setenv("SPORE_OPEN_LINKS", "1", 1);
    expect_open("open", "https://example.com");
    unsetenv("SPORE_OPEN_LINKS");

    /* The same URL twice within a second opens once (both report success); a different URL opens. */
    hle_shell_set_opener(capture);
    {
        int before = got_calls;
        CHECK_EQ(shex("open", "http://www.spore.com", NULL), 42);
        CHECK_EQ(shex("open", "http://www.spore.com", NULL), 42);
        CHECK_EQ(got_calls, before + 1);
        CHECK_EQ(shex("open", "https://example.com", NULL), 42);
        CHECK_EQ(got_calls, before + 2);
    }

    /* A host opener that fails: SE_ERR_NOASSOC (31, <= 32 so the game takes it as failure). */
    opener_result = 0;
    hle_shell_set_opener(capture);
    CHECK_EQ(shex("open", "https://example.com", NULL), 31);
    opener_result = 1;

    /* No host opener: the HLE test binary has no sp_mac_open_url, so the dlsym lookup finds nothing. */
    hle_shell_set_opener(NULL);
    CHECK_EQ(shex("open", "https://example.com", NULL), 31);
}
