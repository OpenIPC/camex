/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_nginxmap.c — the nginx map of camex --status --format nginx-map
 * (#84) and the start page of --format nginx-index (#105)
 *
 */

#include "nginxmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
        if (!(cond)) { \
            printf("FAIL: %s\n", msg); \
            failures++; \
        } \
    } while (0)

static const char head[] =
    "# camex --status --format nginx-map: generated, do not edit\n"
    "map $host $camex_upstream {\n";

static char *fmt(const char *lines, const char *domain, int port,
                 const char *dflt, size_t *count, char *warn, size_t wsize)
{
    FILE *w = fmemopen(warn, wsize, "w");
    char *m;

    m = nginx_map_format(lines, domain, port, dflt, w, count);
    if (w != NULL) {
        fclose(w);
    }
    return m;
}

static char *page(const char *lines, const char *domain, size_t *count,
                  char *warn, size_t wsize)
{
    FILE *w = fmemopen(warn, wsize, "w");
    char *m;

    m = nginx_index_format(lines, domain, w, count);
    if (w != NULL) {
        fclose(w);
    }
    return m;
}

/* The start of a row on the start page, up to the name's cell */
#define ROW "<tr class=\"dev\"><td class=\"lamp\"><span class=\"bulb\">" \
            "</span></td><td>"

/* The start page (#105) and the names on it */
static void test_index(void)
{
    char warn[1024];
    size_t n;
    char *m, *a, *b;

    /* Names: UTF-8 text, at most 64 bytes, no control characters */
    CHECK(camex_name_valid("Warehouse, entrance"), "a plain name");
    CHECK(camex_name_valid("\xd0\xa1\xd0\xba\xd0\xbb\xd0\xb0\xd0\xb4 <1> & \"x\""),
          "Cyrillic and HTML characters are text");
    CHECK(camex_name_valid("\xe4\xbb\x93\xe5\xba\x93 \xf0\x9f\x93\xb7"),
          "CJK and a 4-byte character");
    CHECK(!camex_name_valid(""), "an empty name refused");
    CHECK(camex_name_valid("0123456789012345678901234567890123456789"
                           "012345678901234567890123"), "64 bytes");
    CHECK(!camex_name_valid("0123456789012345678901234567890123456789"
                            "0123456789012345678901234"), "65 bytes refused");
    CHECK(!camex_name_valid("a\tb"), "a tab refused");
    CHECK(!camex_name_valid("a\x7f"), "DEL refused");
    CHECK(!camex_name_valid("a\xc2\x85"), "C1 (NEL) refused");
    CHECK(!camex_name_valid("a\xc0\xaf"), "an overlong form refused");
    CHECK(!camex_name_valid("a\xe0\x80\xaf"), "a 3-byte overlong refused");
    CHECK(!camex_name_valid("a\xed\xa0\x80"), "a surrogate refused");
    CHECK(!camex_name_valid("a\xf4\x90\x80\x80"), "above U+10FFFF refused");
    CHECK(!camex_name_valid("a\xd0"), "a cut sequence refused");
    CHECK(!camex_name_valid("a\x80"), "a lone continuation byte refused");

    /* No clients: a page that says so */
    m = page("", "openipc.click", &n, warn, sizeof(warn));
    CHECK(m != NULL && n == 0U && strstr(m, "<!doctype html>\n") == m &&
              strstr(m, "<div class=\"empty\">No devices online</div>") != NULL &&
              strstr(m, "<table>") == NULL && strstr(m, "</html>\n") != NULL,
          "an empty page");
    free(m);

    /* Names, IDs as names, links, sorted by name in any case */
    memset(warn, 0, sizeof(warn));
    m = page("CAM-B 10.200.0.3 zoo gate\n"
             "cam-c 10.200.0.4\n"
             "Cam-A 10.200.0.2 Back <yard> & \"shed\"\n",
             "openipc.click", &n, warn, sizeof(warn));
    CHECK(m != NULL && n == 3U && strstr(m, "<div class=\"sub\">3 devices online</div>"),
          "three devices");
    CHECK(m != NULL && strstr(m,
              ROW "<a href=\"//cam-a.openipc.click/\">Back &lt;yard&gt; "
              "&amp; &quot;shed&quot;</a></td><td class=\"id\">cam-a</td>"
              "<td class=\"addr\">10.200.0.2</td></tr>\n"
              ROW "<a href=\"//cam-c.openipc.click/\">cam-c</a></td>"
              "<td class=\"id\">cam-c</td><td class=\"addr\">10.200.0.4"
              "</td></tr>\n"
              ROW "<a href=\"//cam-b.openipc.click/\">zoo gate</a></td>"
              "<td class=\"id\">cam-b</td><td class=\"addr\">10.200.0.3"
              "</td></tr>\n") != NULL,
          "escaped names, the ID without one, sorted by name");
    CHECK(warn[0] == '\0', "no warnings");
    free(m);

    /* One device; no domain */
    m = page("cam1 10.0.0.1\n", NULL, &n, warn, sizeof(warn));
    CHECK(m != NULL && n == 1U && strstr(m, "<div class=\"sub\">1 device online</div>") &&
              strstr(m, "<a href=\"//cam1/\">cam1</a>"),
          "one device, a link without a domain");
    free(m);

    /* The same state in another order: an identical page */
    a = page("x1 10.0.0.1 Same\nx2 10.0.0.2 same\n", "openipc.click", &n, warn,
             sizeof(warn));
    b = page("x2 10.0.0.2 same\nx1 10.0.0.1 Same\n", "openipc.click", &n, warn,
             sizeof(warn));
    CHECK(a != NULL && b != NULL && strcmp(a, b) == 0,
          "order of the input does not matter");
    CHECK(a != NULL && strstr(a, "//x1.openipc.click/") < strstr(a, "//x2.openipc.click/"),
          "equal names in other case: a fixed order");
    free(a);
    free(b);
    a = page("x2 10.0.0.2 Same\nx1 10.0.0.1 same\n", "openipc.click", &n,
             warn, sizeof(warn));
    b = NULL;
    CHECK(a != NULL && strstr(a, "//x1.openipc.click/") <
                           strstr(a, "//x2.openipc.click/"),
          "equal names in other case: by ID, not by the spelling");
    free(a);
    free(b);

    /* Bad IDs and duplicates as in the map */
    memset(warn, 0, sizeof(warn));
    m = page("bad_id 10.0.0.2 Name\ncam 10.0.0.9 First\n"
             "CAM 10.0.0.10 Second\n", "openipc.click", &n, warn, sizeof(warn));
    CHECK(m != NULL && n == 1U && strstr(m, "bad") == NULL &&
              strstr(m, ">First<") != NULL && strstr(m, "Second") == NULL,
          "a bad ID left out, a duplicate keeps the lower address");
    CHECK(strstr(warn, "bad_id is not a host name label: left out of the "
                 "start page") != NULL && strstr(warn, "listed twice"),
          "both reported");
    free(m);

    /* The same ID and address with other names (two servers): the same
     * name is kept whatever the order */
    a = page("cam 10.0.0.9 Zulu\ncam 10.0.0.9 Alpha\n", "openipc.click", &n, warn,
             sizeof(warn));
    b = page("cam 10.0.0.9 Alpha\ncam 10.0.0.9 Zulu\n", "openipc.click", &n, warn,
             sizeof(warn));
    CHECK(a != NULL && b != NULL && strcmp(a, b) == 0 &&
              strstr(a, ">Alpha<") != NULL && strstr(a, "Zulu") == NULL,
          "a duplicate with another name: the same one in any order");
    free(a);
    free(b);

    /* A name that is not valid text (not from a camex server): the ID */
    memset(warn, 0, sizeof(warn));
    m = page("cam 10.0.0.1 x\x01<script>\n", "openipc.click", &n, warn,
             sizeof(warn));
    CHECK(m != NULL && n == 1U && strstr(m, "script") == NULL &&
              strstr(m, ">cam</a>") != NULL &&
              strstr(warn, "not valid text") != NULL,
          "an invalid name is replaced by the ID");
    free(m);

    /* Bad domain */
    CHECK(page("", "bad..domain", &n, warn, sizeof(warn)) == NULL,
          "bad domain refused");
}

int main(void)
{
    char warn[1024];
    size_t n;
    char *m;

    /* No clients: an empty map, with the default when given */
    memset(warn, 0, sizeof(warn));
    m = fmt("", "openipc.click", 80, NULL, &n, warn, sizeof(warn));
    CHECK(m != NULL && n == 0U, "empty list gives a map");
    CHECK(m != NULL && strncmp(m, head, strlen(head)) == 0 &&
              strcmp(m + strlen(head), "}\n") == 0,
          "empty map is the header and the closing brace");
    free(m);
    m = fmt("", NULL, 80, "127.0.0.1:8099", &n, warn, sizeof(warn));
    CHECK(m != NULL && strstr(m, "    default 127.0.0.1:8099;\n") != NULL,
          "the default line");
    free(m);

    /* Sorted, lower case, the domain and the port */
    m = fmt("CAM-B 10.200.0.3\nCam-A 10.200.0.2\ncam-c 10.200.0.4\n",
            "openipc.click", 8080, NULL, &n, warn, sizeof(warn));
    CHECK(m != NULL && n == 3U, "three clients");
    CHECK(m != NULL && strcmp(m + strlen(head),
                              "    \"cam-a.openipc.click\" 10.200.0.2:8080;\n"
                              "    \"cam-b.openipc.click\" 10.200.0.3:8080;\n"
                              "    \"cam-c.openipc.click\" 10.200.0.4:8080;\n"
                              "}\n") == 0,
          "sorted, lower case, with domain and port");
    free(m);

    /* The same input in another order: an identical map */
    {
        char *a = fmt("x1 10.0.0.1\nx2 10.0.0.2\n", "openipc.click", 80, NULL, &n,
                      warn, sizeof(warn));
        char *b = fmt("x2 10.0.0.2\nx1 10.0.0.1\n", "openipc.click", 80, NULL, &n,
                      warn, sizeof(warn));

        CHECK(a != NULL && b != NULL && strcmp(a, b) == 0,
              "order of the input does not matter");
        free(a);
        free(b);
    }

    /* IDs that are not host labels are left out with a warning */
    memset(warn, 0, sizeof(warn));
    m = fmt("good 10.0.0.1\nbad_id 10.0.0.2\n-dash 10.0.0.3\n"
            "dot.ted 10.0.0.4\n", "openipc.click", 80, NULL, &n, warn, sizeof(warn));
    CHECK(m != NULL && n == 1U && strstr(m, "\"good.openipc.click\" 10.0.0.1:80;") &&
              strstr(m, "bad_id") == NULL && strstr(m, "dash") == NULL &&
              strstr(m, "dot") == NULL,
          "invalid IDs are left out");
    CHECK(strstr(warn, "bad_id") != NULL && strstr(warn, "-dash") != NULL,
          "with a warning");
    free(m);

    /* The same ID twice (two servers): the lower address, a warning */
    memset(warn, 0, sizeof(warn));
    m = fmt("cam 10.0.0.9\nCAM 10.0.0.10\n", "openipc.click", 80, NULL, &n, warn,
            sizeof(warn));
    CHECK(m != NULL && n == 1U && strstr(m, "\"cam.openipc.click\" 10.0.0.9:80;") &&
              strstr(m, "10.0.0.10") == NULL,
          "a duplicate keeps the numerically lower address");
    CHECK(strstr(warn, "listed twice") != NULL, "a duplicate is reported");
    free(m);

    /* Without a domain the key is the ID alone; junk lines are skipped */
    m = fmt("cam1 10.0.0.1\n\nnot-an-address x\n", "", 80, NULL, &n, warn,
            sizeof(warn));
    CHECK(m != NULL && n == 1U && strstr(m, "    \"cam1\" 10.0.0.1:80;\n"),
          "no domain, junk skipped");
    free(m);

    /* A record with more than two fields is not used */
    m = fmt("victim 1.2.3.4 10.0.0.7\nok 10.0.0.8\n", "openipc.click", 80, NULL, &n,
            warn, sizeof(warn));
    CHECK(m != NULL && n == 1U && strstr(m, "victim") == NULL &&
              strstr(m, "1.2.3.4") == NULL,
          "a record with extra fields is ignored");
    free(m);

    /* An ID that is an nginx map keyword stays a host: nginx drops the
     * quotes, so the key needs map's backslash */
    m = fmt("default 10.0.0.3\ninclude 10.0.0.4\nhostnames 10.0.0.5\n"
            "volatile 10.0.0.6\n", "", 80, "127.0.0.1:8099", &n, warn,
            sizeof(warn));
    CHECK(m != NULL && n == 4U &&
              strstr(m, "    default 127.0.0.1:8099;\n") != NULL &&
              strstr(m, "    \"\\default\" 10.0.0.3:80;\n") != NULL &&
              strstr(m, "    \"\\include\" 10.0.0.4:80;\n") != NULL &&
              strstr(m, "    \"\\hostnames\" 10.0.0.5:80;\n") != NULL &&
              strstr(m, "    \"\\volatile\" 10.0.0.6:80;\n") != NULL,
          "map keywords as IDs get a backslash, the fallback stays one");
    free(m);
    m = fmt("default 10.0.0.3\n", "openipc.click", 80, NULL, &n, warn, sizeof(warn));
    CHECK(m != NULL && strstr(m, "    \"default.openipc.click\" 10.0.0.3:80;\n"),
          "with a domain the key is no keyword: no backslash");
    free(m);

    /* An ID with non-ASCII bytes: left out, the warning shows them as ? */
    memset(warn, 0, sizeof(warn));
    m = fmt("cam\xc3\xa9 10.0.0.1\n", "openipc.click", 80, NULL, &n, warn,
            sizeof(warn));
    CHECK(m != NULL && n == 0U && strstr(warn, "client ID cam?? is not") &&
              strchr(warn, '\xc3') == NULL,
          "a non-ASCII ID is reported with ? for its bytes");
    free(m);

    /* The default is written out anew */
    m = fmt("", NULL, 80, "127.000.0.1:08099", &n, warn, sizeof(warn));
    CHECK(m != NULL && strstr(m, "    default 127.0.0.1:8099;\n") != NULL,
          "the default is normalised");
    free(m);

    /* A host longer than 253 characters is left out */
    {
        char dom[260];

        snprintf(dom, sizeof(dom), "%.62s.%.62s.%.62s.%.62s",
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                 "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                 "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
                 "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
        memset(warn, 0, sizeof(warn));
        m = fmt("cam 10.0.0.1\n", dom, 80, NULL, &n, warn, sizeof(warn));
        CHECK(m != NULL && n == 0U && strstr(warn, "longer than a host"),
              "a too long host is left out with a warning");
        free(m);
    }

    /* Bad parameters */
    CHECK(fmt("", "bad..domain", 80, NULL, &n, warn, sizeof(warn)) == NULL,
          "bad domain refused");
    CHECK(fmt("", "openipc.click", 0, NULL, &n, warn, sizeof(warn)) == NULL,
          "bad port refused");
    CHECK(fmt("", "openipc.click", 80, "x;y", &n, warn, sizeof(warn)) == NULL,
          "a default with ; refused");
    CHECK(fmt("", "openipc.click", 80, ":80", &n, warn, sizeof(warn)) == NULL,
          "a default without a host refused");
    CHECK(fmt("", "openipc.click", 80, "foo:bar", &n, warn, sizeof(warn)) == NULL,
          "a default without a port refused");
    CHECK(fmt("", "openipc.click", 80, "host:70000", &n, warn, sizeof(warn)) == NULL,
          "a default port above 65535 refused");
    CHECK(fmt("", "openipc.click", 80, "offline.openipc.click:8099", &n, warn,
              sizeof(warn)) == NULL,
          "a host name default refused (no resolver in the example)");
    CHECK(fmt("", "openipc.click", 80, "1. 2.3.4:80", &n, warn, sizeof(warn)) == NULL,
          "a default with a blank in the address refused");
    CHECK(fmt("", "openipc.click", 80, "1.2.3.4: 80", &n, warn, sizeof(warn)) == NULL,
          "a default with a blank before the port refused");
    CHECK(fmt("", "openipc.click", 80, "1.+2.3.4:80", &n, warn, sizeof(warn)) == NULL,
          "a default with a signed number refused");
    CHECK(fmt("", "openipc.click", 80, "1.2.3.4:+80", &n, warn, sizeof(warn)) == NULL,
          "a default with a signed port refused");
    CHECK(fmt("", "openipc.click", 80, "1.2.3.4:080000", &n, warn, sizeof(warn)) == NULL,
          "a default port with too many digits refused");

    test_index();

    if (failures != 0) {
        printf("test_nginxmap: %d FAILED\n", failures);
        return 1;
    }
    printf("test_nginxmap: OK\n");
    return 0;
}
