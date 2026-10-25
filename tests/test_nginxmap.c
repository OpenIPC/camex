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

/* The "# domain: ..." line of the last map (#106), cut out of it so that
 * the checks below compare the rest */
static char domain_line[300];

static char *fmt(const char *lines, const char *domain, int port,
                 const char *dflt, size_t *count, char *warn, size_t wsize)
{
    FILE *w = fmemopen(warn, wsize, "w");
    char *m, *d, *e;

    m = nginx_map_format(lines, domain, port, dflt, w, count);
    if (w != NULL) {
        fclose(w);
    }
    domain_line[0] = '\0';
    if (m != NULL && (d = strstr(m, "\n# domain: ")) != NULL &&
        (e = strchr(d + 1, '\n')) != NULL) {
        snprintf(domain_line, sizeof(domain_line), "%.*s",
                 (int)(e - d - 1), d + 1);
        memmove(d + 1, e + 1, strlen(e + 1) + 1U);
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

/* A live CLIENTS INDEX record: 65 s connected, last packet 2 s ago,
 * last of 3 handshakes 30 s ago, 2048 bytes in, 1024 out */
#define LIVE(id, addr, conf, name) \
    "L\t" id "\t" addr "\t" conf "\t65\t2\t30\t3\t2048\t1024\t" name "\n"

/* The start page (#105, #110) and the names on it */
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

    /* No clients: both lists say so, Configured opens */
    m = page("", "openipc.click", &n, warn, sizeof(warn));
    CHECK(m != NULL && n == 0U && strstr(m, "<!doctype html>\n") == m &&
              strstr(m, "<div class=\"sub\">0 online &middot; 0 configured"
                     "</div>") != NULL &&
              strstr(m, "No [client ID] blocks in the server&#39;s file") ==
                  NULL &&
              strstr(m, "No [client ID] blocks") != NULL &&
              strstr(m, "No auto devices online") != NULL &&
              strstr(m, "<table>") == NULL &&
              strstr(m, "id=\"tab-conf\" checked") != NULL &&
              strstr(m, "</html>\n") != NULL,
          "an empty page");
    free(m);

    /* Live auto and configured clients, an offline configured one; names
     * escaped, sorted by name in any case, the ID without a name */
    memset(warn, 0, sizeof(warn));
    m = page(LIVE("CAM-B", "10.200.0.3", "1", "zoo gate")
             LIVE("cam-c", "10.200.0.4", "1", "")
             LIVE("Cam-A", "10.200.0.2", "1", "Back <yard> & \"shed\"")
             LIVE("auto1", "10.200.0.9", "0", "")
             "O\tcam-d\t10.200.0.20\tDock\n"
             "O\tcam-e\t-\n",
             "openipc.click", &n, warn, sizeof(warn));
    CHECK(m != NULL && n == 4U &&
              strstr(m, "<div class=\"sub\">4 online &middot; 5 configured"
                     "</div>") != NULL,
          "four online, five configured");
    CHECK(m != NULL && strstr(m, "Configured <b>5</b> &middot; <i>2 offline"
                              "</i></label><label for=\"tab-auto\">Auto <b>1"
                              "</b>") != NULL,
          "the tabs with their counts");
    CHECK(m != NULL && strstr(m,
              ROW "<a href=\"//cam-a.openipc.click/\">Back &lt;yard&gt; "
              "&amp; &quot;shed&quot;</a></td><td class=\"id\">cam-a</td>"
              "<td class=\"addr\">10.200.0.2</td><td class=\"conn\">1m05s"
              "</td><td>2s ago</td><td class=\"hs\">30s ago &middot; 3</td>"
              "<td class=\"tr\">2.0 / 1.0</td></tr>\n") != NULL,
          "a live row with its values");
    CHECK(m != NULL && strstr(m,
              "<tr class=\"dev off\"><td class=\"lamp\"><span class=\"bulb\">"
              "</span></td><td><span class=\"nm\">cam-e</span></td><td "
              "class=\"id\">cam-e</td><td class=\"addr\">&mdash;</td>") !=
                  NULL &&
              strstr(m, "href=\"//cam-e.") == NULL &&
              strstr(m, "<span class=\"nm\">Dock</span>") != NULL &&
              strstr(m, ">10.200.0.20<") != NULL,
          "offline rows: no link, the configured address or a dash");
    {
        const char *conf = m != NULL
                               ? strstr(m, "<div class=\"list list-conf\">")
                               : NULL;
        const char *aut = m != NULL
                              ? strstr(m, "<div class=\"list list-auto\">")
                              : NULL;
        const char *a1 = m != NULL ? strstr(m, "//auto1.") : NULL;
        const char *pa = m != NULL ? strstr(m, "//cam-a.") : NULL;
        const char *pc = m != NULL ? strstr(m, ">cam-c</a>") : NULL;
        const char *pd = m != NULL ? strstr(m, ">Dock<") : NULL;
        const char *pe = m != NULL ? strstr(m, ">cam-e<") : NULL;
        const char *pb = m != NULL ? strstr(m, ">zoo gate<") : NULL;

        CHECK(conf != NULL && aut != NULL && conf < aut && a1 > aut &&
                  pa > conf && pa < aut,
              "auto1 under Auto, the configured ones under Configured");
        CHECK(pa != NULL && pc != NULL && pd != NULL && pe != NULL &&
                  pb != NULL && pa < pc && pc < pe && pe < pd && pd < pb,
              "sorted by name, offline ones among them");
    }
    CHECK(warn[0] == '\0', "no warnings");
    free(m);

    /* Only auto devices: Auto opens; ages, a missing last packet, traffic */
    m = page("L\tcam1\t10.0.0.1\t0\t93811\t-\t412\t172\t731234111\t20234111"
             "\n", NULL, &n, warn, sizeof(warn));
    CHECK(m != NULL && n == 1U && strstr(m, "id=\"tab-auto\" checked") &&
              strstr(m, "<a href=\"//cam1/\">cam1</a>") &&
              strstr(m, "<td class=\"conn\">1d02h</td><td>&mdash;</td>"
                     "<td class=\"hs\">6m52s ago &middot; 172</td><td "
                     "class=\"tr\">714&#8239;096 / 19&#8239;760</td>"),
          "one auto device, a link without a domain, the values");
    free(m);

    /* The same state in another order: an identical page */
    a = page(LIVE("x1", "10.0.0.1", "1", "Same")
             LIVE("x2", "10.0.0.2", "1", "same"), "openipc.click", &n, warn,
             sizeof(warn));
    b = page(LIVE("x2", "10.0.0.2", "1", "same")
             LIVE("x1", "10.0.0.1", "1", "Same"), "openipc.click", &n, warn,
             sizeof(warn));
    CHECK(a != NULL && b != NULL && strcmp(a, b) == 0,
          "order of the input does not matter");
    CHECK(a != NULL && strstr(a, "//x1.openipc.click/") < strstr(a, "//x2.openipc.click/"),
          "equal names in other case: a fixed order");
    free(a);
    free(b);
    a = page(LIVE("x2", "10.0.0.2", "1", "Same")
             LIVE("x1", "10.0.0.1", "1", "same"), "openipc.click", &n, warn,
             sizeof(warn));
    b = NULL;
    CHECK(a != NULL && strstr(a, "//x1.openipc.click/") <
                           strstr(a, "//x2.openipc.click/"),
          "equal names in other case: by ID, not by the spelling");
    free(a);
    free(b);

    /* Bad IDs and duplicates as in the map; a live record wins over an
     * offline one of the same ID (two servers) */
    memset(warn, 0, sizeof(warn));
    m = page(LIVE("bad_id", "10.0.0.2", "1", "Name")
             LIVE("cam", "10.0.0.9", "1", "First")
             LIVE("CAM", "10.0.0.10", "1", "Second")
             "O\tcam2\t10.0.0.1\tOff\n"
             LIVE("cam2", "10.0.0.30", "1", "On"),
             "openipc.click", &n, warn, sizeof(warn));
    CHECK(m != NULL && n == 2U && strstr(m, "bad") == NULL &&
              strstr(m, ">First<") != NULL && strstr(m, "Second") == NULL &&
              strstr(m, ">On<") != NULL && strstr(m, "Off") == NULL,
          "a bad ID left out, duplicates resolved");
    CHECK(strstr(warn, "bad_id is not a host name label: left out of the "
                 "start page") != NULL && strstr(warn, "listed twice"),
          "both reported");
    free(m);

    /* The same ID and address with other names (two servers): the same
     * name is kept whatever the order */
    a = page(LIVE("cam", "10.0.0.9", "1", "Zulu")
             LIVE("cam", "10.0.0.9", "1", "Alpha"), "openipc.click", &n, warn,
             sizeof(warn));
    b = page(LIVE("cam", "10.0.0.9", "1", "Alpha")
             LIVE("cam", "10.0.0.9", "1", "Zulu"), "openipc.click", &n, warn,
             sizeof(warn));
    CHECK(a != NULL && b != NULL && strcmp(a, b) == 0 &&
              strstr(a, ">Alpha<") != NULL && strstr(a, "Zulu") == NULL,
          "a duplicate with another name: the same one in any order");
    free(a);
    free(b);

    /* A name that is not valid text (not from a camex server): the ID */
    memset(warn, 0, sizeof(warn));
    m = page(LIVE("cam", "10.0.0.1", "1", "x\x01<script>"), "openipc.click", &n,
             warn, sizeof(warn));
    CHECK(m != NULL && n == 1U && strstr(m, "&lt;script") == NULL &&
              strstr(m, "x\x01") == NULL && strstr(m, ">cam</a>") != NULL &&
              strstr(warn, "not valid text") != NULL,
          "an invalid name is replaced by the ID");
    free(m);

    /* Malformed records are not used: old CLIENTS NAMES lines, a bad
     * number, too few fields */
    m = page("cam 10.0.0.1 Name\n"
             "L\tcam2\t10.0.0.2\t1\tx\t2\t30\t3\t2048\t1024\n"
             "L\tcam3\t10.0.0.3\t1\t60\n"
             "O\tcam4\n", "openipc.click", &n, warn, sizeof(warn));
    CHECK(m != NULL && n == 0U && strstr(m, "<tr class") == NULL,
          "malformed records are skipped");
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
    CHECK(strcmp(domain_line, "# domain: (none)") == 0,
          "a map without a domain says so");
    m = fmt("", "Openipc.Click", 80, NULL, &n, warn, sizeof(warn));
    CHECK(m != NULL && strcmp(domain_line, "# domain: Openipc.Click") == 0,
          "the map names its domain");
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
