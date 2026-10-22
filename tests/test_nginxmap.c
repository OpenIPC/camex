/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * test_nginxmap.c — the nginx map of camex --status --format nginx-map
 * (#84)
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

int main(void)
{
    char warn[1024];
    size_t n;
    char *m;

    /* No clients: an empty map, with the default when given */
    memset(warn, 0, sizeof(warn));
    m = fmt("", "cam.example.lan", 80, NULL, &n, warn, sizeof(warn));
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
            "cam.example.lan", 8080, NULL, &n, warn, sizeof(warn));
    CHECK(m != NULL && n == 3U, "three clients");
    CHECK(m != NULL && strcmp(m + strlen(head),
                              "    cam-a.cam.example.lan 10.200.0.2:8080;\n"
                              "    cam-b.cam.example.lan 10.200.0.3:8080;\n"
                              "    cam-c.cam.example.lan 10.200.0.4:8080;\n"
                              "}\n") == 0,
          "sorted, lower case, with domain and port");
    free(m);

    /* The same input in another order: an identical map */
    {
        char *a = fmt("x1 10.0.0.1\nx2 10.0.0.2\n", "d.lan", 80, NULL, &n,
                      warn, sizeof(warn));
        char *b = fmt("x2 10.0.0.2\nx1 10.0.0.1\n", "d.lan", 80, NULL, &n,
                      warn, sizeof(warn));

        CHECK(a != NULL && b != NULL && strcmp(a, b) == 0,
              "order of the input does not matter");
        free(a);
        free(b);
    }

    /* IDs that are not host labels are left out with a warning */
    memset(warn, 0, sizeof(warn));
    m = fmt("good 10.0.0.1\nbad_id 10.0.0.2\n-dash 10.0.0.3\n"
            "dot.ted 10.0.0.4\n", "d.lan", 80, NULL, &n, warn, sizeof(warn));
    CHECK(m != NULL && n == 1U && strstr(m, "good.d.lan 10.0.0.1:80;") &&
              strstr(m, "bad_id") == NULL && strstr(m, "dash") == NULL &&
              strstr(m, "dot") == NULL,
          "invalid IDs are left out");
    CHECK(strstr(warn, "bad_id") != NULL && strstr(warn, "-dash") != NULL,
          "with a warning");
    free(m);

    /* The same ID twice (two servers): the lower address, a warning */
    memset(warn, 0, sizeof(warn));
    m = fmt("cam 10.0.0.9\nCAM 10.0.0.5\n", "d.lan", 80, NULL, &n, warn,
            sizeof(warn));
    CHECK(m != NULL && n == 1U && strstr(m, "cam.d.lan 10.0.0.5:80;") &&
              strstr(m, "10.0.0.9") == NULL,
          "a duplicate keeps the lower address");
    CHECK(strstr(warn, "listed twice") != NULL, "a duplicate is reported");
    free(m);

    /* Without a domain the key is the ID alone; junk lines are skipped */
    m = fmt("cam1 10.0.0.1\n\nnot-an-address x\n", "", 80, NULL, &n, warn,
            sizeof(warn));
    CHECK(m != NULL && n == 1U && strstr(m, "    cam1 10.0.0.1:80;\n"),
          "no domain, junk skipped");
    free(m);

    /* Bad parameters */
    CHECK(fmt("", "bad..domain", 80, NULL, &n, warn, sizeof(warn)) == NULL,
          "bad domain refused");
    CHECK(fmt("", "d.lan", 0, NULL, &n, warn, sizeof(warn)) == NULL,
          "bad port refused");
    CHECK(fmt("", "d.lan", 80, "x;y", &n, warn, sizeof(warn)) == NULL,
          "a default with ; refused");

    if (failures != 0) {
        printf("test_nginxmap: %d FAILED\n", failures);
        return 1;
    }
    printf("test_nginxmap: OK\n");
    return 0;
}
