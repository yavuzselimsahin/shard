/* Small helpers: allocation that cannot fail, a growable string buffer,
 * path expansion, and the handful of formatting routines the CLI needs. */

#include "shard.h"

#include <errno.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory");
    return q;
}

char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *xasprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) die("vsnprintf failed");
    char *out = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(out, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return out;
}

void sb_init(strbuf *b)
{
    b->data = xmalloc(128);
    b->data[0] = '\0';
    b->len = 0;
    b->cap = 128;
}

static void sb_grow(strbuf *b, size_t need)
{
    if (b->len + need + 1 <= b->cap) return;
    while (b->cap < b->len + need + 1) b->cap *= 2;
    b->data = xrealloc(b->data, b->cap);
}

void sb_add(strbuf *b, const char *s, size_t n)
{
    if (!b->data) sb_init(b);
    sb_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void sb_puts(strbuf *b, const char *s)
{
    if (s) sb_add(b, s, strlen(s));
}

void sb_printf(strbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (!b->data) sb_init(b);
    sb_grow(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

void sb_free(strbuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

char *str_trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n')) end--;
    *end = '\0';
    return s;
}

int str_eq(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

int str_has_prefix(const char *s, const char *pfx)
{
    return strncmp(s, pfx, strlen(pfx)) == 0;
}

char *str_replace(const char *s, const char *what, const char *with)
{
    size_t wl = strlen(what);
    if (wl == 0) return xstrdup(s);

    strbuf b;
    sb_init(&b);
    const char *p = s;
    for (;;) {
        const char *hit = strstr(p, what);
        if (!hit) { sb_puts(&b, p); break; }
        sb_add(&b, p, (size_t)(hit - p));
        sb_puts(&b, with);
        p = hit + wl;
    }
    return b.data;
}

char *home_dir(void)
{
    const char *h = getenv("HOME");
    if (h && *h) return xstrdup(h);
    struct passwd *pw = getpwuid(getuid());
    return xstrdup(pw && pw->pw_dir ? pw->pw_dir : ".");
}

char *expand_path(const char *p)
{
    if (!p) return NULL;
    if (p[0] != '~') return xstrdup(p);
    if (p[1] != '/' && p[1] != '\0') return xstrdup(p);  /* ~user: left alone */

    char *home = home_dir();
    char *out = xasprintf("%s%s", home, p + 1);
    free(home);
    return out;
}

int mkdir_p(const char *path)
{
    char *copy = xstrdup(path);
    size_t n = strlen(copy);
    for (size_t i = 1; i <= n; i++) {
        if (copy[i] != '/' && copy[i] != '\0') continue;
        char save = copy[i];
        copy[i] = '\0';
        if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
            free(copy);
            return -1;
        }
        copy[i] = save;
    }
    free(copy);
    return 0;
}

char *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    strbuf b;
    sb_init(&b);
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) sb_add(&b, chunk, n);
    fclose(f);

    if (len_out) *len_out = b.len;
    return b.data;
}

int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

double now_seconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

char *now_stamp(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return xstrdup(buf);
}

void fmt_duration(double secs, char *out, size_t n)
{
    if (secs < 0) secs = 0;
    if (secs < 60) {
        snprintf(out, n, "%.2fs", secs);
    } else if (secs < 3600) {
        snprintf(out, n, "%dm %02ds", (int)secs / 60, (int)secs % 60);
    } else {
        snprintf(out, n, "%dh %02dm", (int)secs / 3600, ((int)secs % 3600) / 60);
    }
}

void json_escape(strbuf *b, const char *s)
{
    if (!s) return;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\n': sb_puts(b, "\\n");  break;
        case '\r': sb_puts(b, "\\r");  break;
        case '\t': sb_puts(b, "\\t");  break;
        default:
            if (*p < 0x20) sb_printf(b, "\\u%04x", *p);
            else           sb_add(b, (const char *)p, 1);
        }
    }
}

static const char *json_value_at(const char *text, const char *key)
{
    char *needle = xasprintf("\"%s\"", key);
    const char *p = text ? strstr(text, needle) : NULL;
    free(needle);
    if (!p) return NULL;

    p = strchr(p, ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    return p;
}

char *json_peek_str(const char *text, const char *key)
{
    const char *p = json_value_at(text, key);
    if (!p || *p != '"') return NULL;
    p++;

    strbuf b;
    sb_init(&b);
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            char c = *p == 'n' ? '\n' : (*p == 't' ? '\t' : *p);
            sb_add(&b, &c, 1);
            p++;
            continue;
        }
        sb_add(&b, p, 1);
        p++;
    }
    return b.data;
}

double json_peek_num(const char *text, const char *key)
{
    const char *p = json_value_at(text, key);
    if (!p) return 0;
    return strtod(p, NULL);
}

void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "shard: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void warn_msg(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "shard: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* Colour is on for a terminal, off for a pipe, and off when NO_COLOR is set. */
int color_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *term = getenv("TERM");
        cached = isatty(STDOUT_FILENO) &&
                 !getenv("NO_COLOR") &&
                 !(term && strcmp(term, "dumb") == 0);
    }
    return cached;
}

const char *c_green(void)  { return color_enabled() ? "\033[32m" : ""; }
const char *c_red(void)    { return color_enabled() ? "\033[31m" : ""; }
const char *c_yellow(void) { return color_enabled() ? "\033[33m" : ""; }
const char *c_dim(void)    { return color_enabled() ? "\033[2m"  : ""; }
const char *c_bold(void)   { return color_enabled() ? "\033[1m"  : ""; }
const char *c_reset(void)  { return color_enabled() ? "\033[0m"  : ""; }
