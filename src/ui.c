/* `shard ui` — the web dashboard.
 *
 * A single-threaded HTTP server bound to localhost. The page is embedded in
 * the binary and served in one round trip; everything else it needs comes
 * from four small JSON endpoints. There is no framework, no build step and
 * no state of its own: the log directory is the database.
 */

#include "shard.h"
#include "ui_page.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    char method[8];
    char path[512];
    char query[512];
} request;

static void send_response(int fd, const char *status, const char *ctype,
                          const char *body, size_t len)
{
    char head[512];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     status, ctype, len);
    if (write(fd, head, (size_t)n) < 0) return;
    if (len && write(fd, body, len) < 0) return;
}

static void send_text(int fd, const char *status, const char *ctype, const char *body)
{
    send_response(fd, status, ctype, body, strlen(body));
}

/* Query values reach us percent-encoded; only a handful of characters ever
 * matter here, but decoding them all is three lines. */
static char *query_get(const char *query, const char *key)
{
    size_t klen = strlen(key);
    const char *p = query;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (len > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t vlen = len - klen - 1;
            char *out = xmalloc(vlen + 1);
            size_t o = 0;
            for (size_t i = 0; i < vlen; i++) {
                if (v[i] == '%' && i + 2 < vlen) {
                    char hex[3] = { v[i + 1], v[i + 2], 0 };
                    out[o++] = (char)strtol(hex, NULL, 16);
                    i += 2;
                } else if (v[i] == '+') {
                    out[o++] = ' ';
                } else {
                    out[o++] = v[i];
                }
            }
            out[o] = '\0';
            return out;
        }
        p = amp ? amp + 1 : NULL;
    }
    return NULL;
}

/* Names coming from the browser end up in file paths, so they may contain
 * nothing but the characters shard itself writes. */
static int safe_name(const char *s)
{
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++) {
        if (*p == '/' || *p == '\\') return 0;
        if (*p == '.' && p[1] == '.') return 0;
    }
    return 1;
}

static void api_cluster(int fd, cluster_t *c)
{
    strbuf b;
    sb_init(&b);
    sb_puts(&b, "{\"name\": \"");
    json_escape(&b, c->name);
    sb_puts(&b, "\", \"config\": \"");
    json_escape(&b, c->config_path);
    sb_puts(&b, "\", \"nodes\": [");

    for (int i = 0; i < c->nnodes; i++) {
        node_t *n = &c->nodes[i];
        sb_puts(&b, i ? ", {\"name\": \"" : "{\"name\": \"");
        json_escape(&b, n->name);
        sb_puts(&b, "\", \"host\": \"");
        json_escape(&b, n->host);
        sb_printf(&b, "\", \"cpu\": %d, \"ram_gb\": %d, \"local\": %s, \"tags\": [",
                  n->cpu, n->ram_gb, n->is_local ? "true" : "false");
        for (int t = 0; t < n->ntags; t++) {
            sb_puts(&b, t ? ", \"" : "\"");
            json_escape(&b, n->tags[t]);
            sb_puts(&b, "\"");
        }
        sb_puts(&b, "]}");
    }
    sb_puts(&b, "]}");

    send_response(fd, "200 OK", "application/json", b.data, b.len);
    sb_free(&b);
}

static void api_health(int fd, cluster_t *c, int refresh)
{
    if (refresh) {
        node_t **nodes;
        int n = cluster_select(c, "all", &nodes);
        if (n > 0) {
            health_t *h = xmalloc(sizeof(health_t) * (size_t)n);
            health_check(c, nodes, n, h);
            health_write(c, h, n);
            free(h);
        }
        free(nodes);
    }

    char *path = health_state_path(c);
    size_t len;
    char *text = read_file(path, &len);
    free(path);

    if (!text) {
        send_text(fd, "200 OK", "application/json", "{\"nodes\": {}}");
        return;
    }
    send_response(fd, "200 OK", "application/json", text, len);
    free(text);
}

/* Every task.json is already JSON that shard wrote, so the listing is a
 * concatenation rather than a re-serialisation. */
static void api_tasks(int fd, cluster_t *c, const char *query)
{
    int limit = 25;
    char *l = query_get(query, "limit");
    if (l) { limit = atoi(l); free(l); }
    if (limit <= 0 || limit > 200) limit = 25;

    char **ids;
    int n = task_list(c, &ids, limit);

    strbuf b;
    sb_init(&b);
    sb_puts(&b, "{\"tasks\": [");

    int written = 0;
    for (int i = 0; i < n; i++) {
        size_t len;
        char *meta = task_meta_read(c, ids[i], &len);
        if (meta && len > 2) {
            if (written++) sb_puts(&b, ",\n");
            sb_add(&b, meta, len);
        }
        free(meta);
        free(ids[i]);
    }
    free(ids);
    sb_puts(&b, "]}");

    send_response(fd, "200 OK", "application/json", b.data, b.len);
    sb_free(&b);
}

static void api_log(int fd, cluster_t *c, const char *query)
{
    char *id   = query_get(query, "id");
    char *file = query_get(query, "file");
    char *tail = query_get(query, "tail");

    if (!safe_name(id) || !safe_name(file)) {
        send_text(fd, "400 Bad Request", "text/plain", "bad name\n");
        goto done;
    }

    char *path = xasprintf("%s/%s/%s", c->log_dir, id, file);
    size_t len;
    char *text = read_file(path, &len);
    free(path);

    if (!text) {
        send_text(fd, "404 Not Found", "text/plain", "");
        goto done;
    }

    /* Only the last N lines: a long build should not push megabytes at the
     * browser every two seconds. */
    char *start = text;
    int want = tail ? atoi(tail) : 0;
    if (want > 0) {
        int lines = 0;
        for (char *p = text + len; p > text; p--) {
            if (p[-1] != '\n') continue;
            if (++lines > want) { start = p; break; }
        }
    }
    send_response(fd, "200 OK", "text/plain; charset=utf-8", start,
                  len - (size_t)(start - text));
    free(text);

done:
    free(id);
    free(file);
    free(tail);
}

static int read_request(int fd, request *req, char *buf, size_t bufsize)
{
    size_t got = 0;
    while (got < bufsize - 1) {
        ssize_t n = read(fd, buf + got, bufsize - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        buf[got] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    if (got == 0) return -1;
    buf[got] = '\0';

    char target[600];
    if (sscanf(buf, "%7s %599s", req->method, target) != 2) return -1;

    char *q = strchr(target, '?');
    if (q) {
        *q = '\0';
        snprintf(req->query, sizeof req->query, "%s", q + 1);
    } else {
        req->query[0] = '\0';
    }
    snprintf(req->path, sizeof req->path, "%s", target);
    return 0;
}

static void handle(int fd, cluster_t *c)
{
    char buf[8192];
    request req;
    memset(&req, 0, sizeof req);

    if (read_request(fd, &req, buf, sizeof buf) != 0) return;

    if (str_eq(req.path, "/") || str_eq(req.path, "/index.html")) {
        send_text(fd, "200 OK", "text/html; charset=utf-8", shard_ui_page);
    } else if (str_eq(req.path, "/api/cluster")) {
        api_cluster(fd, c);
    } else if (str_eq(req.path, "/api/health")) {
        api_health(fd, c, str_eq(req.method, "POST"));
    } else if (str_eq(req.path, "/api/tasks")) {
        api_tasks(fd, c, req.query);
    } else if (str_eq(req.path, "/api/log")) {
        api_log(fd, c, req.query);
    } else {
        send_text(fd, "404 Not Found", "text/plain", "not found\n");
    }
}

int cmd_ui(cluster_t *c, int argc, char **argv)
{
    int port = 8787;
    for (int i = 0; i < argc; i++) {
        if (str_eq(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (argv[i][0] >= '0' && argv[i][0] <= '9') port = atoi(argv[i]);
    }

    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) die("socket: %s", strerror(errno));

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* localhost only */

    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) != 0)
        die("cannot listen on port %d: %s", port, strerror(errno));
    if (listen(srv, 16) != 0)
        die("listen: %s", strerror(errno));

    printf("shard ui — %s (%d node%s)\n", c->name, c->nnodes,
           c->nnodes == 1 ? "" : "s");
    printf("Open http://localhost:%d  ·  Ctrl-C to stop\n", port);
    fflush(stdout);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handle(fd, c);
        close(fd);
    }

    close(srv);
    return 0;
}
