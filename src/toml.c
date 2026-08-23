/* A small TOML reader.
 *
 * It covers the subset shard's configuration uses: comments, tables,
 * arrays of tables, strings, integers, booleans and string arrays. Values
 * land in a flat list of tables kept in file order, which is exactly what
 * `[[nodes]]` needs — the order of the file is the order of the cluster.
 */

#include "shard.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    toml_doc *doc;
    char     *err;
    size_t    errlen;
    int       line;
} parser;

static toml_table *table_new(toml_doc *d, const char *path, int is_array)
{
    d->tables = xrealloc(d->tables, sizeof(toml_table) * (size_t)(d->ntables + 1));
    toml_table *t = &d->tables[d->ntables++];
    memset(t, 0, sizeof *t);
    t->path = xstrdup(path);
    t->is_array = is_array;
    return t;
}

static toml_kv *kv_new(toml_table *t, const char *key)
{
    t->kv = xrealloc(t->kv, sizeof(toml_kv) * (size_t)(t->nkv + 1));
    toml_kv *kv = &t->kv[t->nkv++];
    memset(kv, 0, sizeof *kv);
    kv->key = xstrdup(key);
    return kv;
}

/* Reads a quoted string starting at *p (which points at the quote).
 * Advances *p past the closing quote. Returns a malloc'd value. */
static char *read_quoted(const char **p, parser *ps)
{
    char quote = **p;
    (*p)++;

    strbuf b;
    sb_init(&b);
    while (**p && **p != quote) {
        if (**p == '\\' && quote == '"' && (*p)[1]) {
            (*p)++;
            char c = **p;
            switch (c) {
            case 'n':  sb_puts(&b, "\n"); break;
            case 't':  sb_puts(&b, "\t"); break;
            case 'r':  sb_puts(&b, "\r"); break;
            case '"':  sb_puts(&b, "\"");  break;
            case '\\': sb_puts(&b, "\\"); break;
            default:   sb_add(&b, &c, 1);
            }
            (*p)++;
            continue;
        }
        sb_add(&b, *p, 1);
        (*p)++;
    }
    if (**p != quote) {
        snprintf(ps->err, ps->errlen, "line %d: unterminated string", ps->line);
        sb_free(&b);
        return NULL;
    }
    (*p)++;
    return b.data;
}

/* Everything after `key =`. `more` is called to fetch the next line when an
 * array runs past the end of this one. */
static int read_value(const char *p, toml_kv *kv, parser *ps,
                      char *(*more)(void *), void *more_ctx)
{
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '"' || *p == '\'') {
        char *s = read_quoted(&p, ps);
        if (!s) return -1;
        kv->kind = TOML_STR;
        kv->sval = s;
        return 0;
    }

    if (*p == '[') {
        p++;
        kv->kind = TOML_ARRAY;
        char *line = NULL;                 /* holds a continuation line */
        for (;;) {
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            if (*p == '\0' || *p == '#') {
                free(line);
                line = more ? more(more_ctx) : NULL;
                if (!line) {
                    snprintf(ps->err, ps->errlen,
                             "line %d: unterminated array", ps->line);
                    return -1;
                }
                p = line;
                continue;
            }
            if (*p == ']') { free(line); return 0; }

            char *item;
            if (*p == '"' || *p == '\'') {
                item = read_quoted(&p, ps);
                if (!item) { free(line); return -1; }
            } else {
                const char *start = p;
                while (*p && *p != ',' && *p != ']') p++;
                item = xstrndup(start, (size_t)(p - start));
                str_trim(item);
            }
            kv->items = xrealloc(kv->items,
                                 sizeof(char *) * (size_t)(kv->nitems + 1));
            kv->items[kv->nitems++] = item;
        }
    }

    if (str_has_prefix(p, "true"))  { kv->kind = TOML_BOOL; kv->bval = 1; return 0; }
    if (str_has_prefix(p, "false")) { kv->kind = TOML_BOOL; kv->bval = 0; return 0; }

    /* Anything else: a number if it parses as one, a bare string otherwise. */
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end && end != p) {
        const char *rest = end;
        while (*rest == ' ' || *rest == '\t') rest++;
        if (*rest == '\0' || *rest == '#') {
            kv->kind = TOML_INT;
            kv->ival = v;
            return 0;
        }
    }

    {
        char *s = xstrdup(p);
        char *hash = strchr(s, '#');
        if (hash) *hash = '\0';
        kv->kind = TOML_STR;
        kv->sval = xstrdup(str_trim(s));
        free(s);
    }
    return 0;
}

/* Strips a trailing comment that is not inside a string. */
static void strip_comment(char *line)
{
    int in_str = 0;
    char quote = 0;
    for (char *p = line; *p; p++) {
        if (in_str) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == quote) in_str = 0;
        } else if (*p == '"' || *p == '\'') {
            in_str = 1;
            quote = *p;
        } else if (*p == '#') {
            *p = '\0';
            return;
        }
    }
}

typedef struct {
    char **lines;
    int    n, i;
} line_src;

static char *next_line(void *ctx)
{
    line_src *s = ctx;
    if (s->i >= s->n) return NULL;
    return xstrdup(s->lines[s->i++]);
}

toml_doc *toml_parse_file(const char *path, char *errbuf, size_t errlen)
{
    size_t len;
    char *text = read_file(path, &len);
    if (!text) {
        snprintf(errbuf, errlen, "cannot read %s", path);
        return NULL;
    }

    /* Split into lines up front: arrays may span several of them. */
    line_src src = {0};
    for (char *p = text; ; ) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        src.lines = xrealloc(src.lines, sizeof(char *) * (size_t)(src.n + 1));
        src.lines[src.n++] = p;
        if (!nl) break;
        p = nl + 1;
    }

    toml_doc *doc = xmalloc(sizeof *doc);
    memset(doc, 0, sizeof *doc);
    parser ps = { doc, errbuf, errlen, 0 };
    errbuf[0] = '\0';

    toml_table *cur = table_new(doc, "", 0);   /* root: keys before any table */

    while (src.i < src.n) {
        char *raw = src.lines[src.i++];
        ps.line = src.i;

        char *line = xstrdup(raw);
        strip_comment(line);
        char *s = str_trim(line);

        if (*s == '\0') { free(line); continue; }

        if (*s == '[') {
            int is_array = (s[1] == '[');
            char *name = s + (is_array ? 2 : 1);
            char *close = strchr(name, ']');
            if (!close) {
                snprintf(errbuf, errlen, "line %d: unterminated table header", ps.line);
                free(line);
                goto fail;
            }
            *close = '\0';
            cur = table_new(doc, str_trim(name), is_array);
            free(line);
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            snprintf(errbuf, errlen, "line %d: expected key = value", ps.line);
            free(line);
            goto fail;
        }
        *eq = '\0';
        char *key = str_trim(s);
        if (*key == '"' || *key == '\'') {
            const char *kp = key;
            char *unq = read_quoted(&kp, &ps);
            if (!unq) { free(line); goto fail; }
            toml_kv *kv = kv_new(cur, unq);
            free(unq);
            if (read_value(eq + 1, kv, &ps, next_line, &src) != 0) {
                free(line);
                goto fail;
            }
        } else {
            toml_kv *kv = kv_new(cur, key);
            if (read_value(eq + 1, kv, &ps, next_line, &src) != 0) {
                free(line);
                goto fail;
            }
        }
        free(line);
    }

    free(src.lines);
    /* `text` backs nothing we kept: every value was copied. */
    free(text);
    return doc;

fail:
    free(src.lines);
    free(text);
    toml_free(doc);
    return NULL;
}

void toml_free(toml_doc *d)
{
    if (!d) return;
    for (int i = 0; i < d->ntables; i++) {
        toml_table *t = &d->tables[i];
        for (int j = 0; j < t->nkv; j++) {
            toml_kv *kv = &t->kv[j];
            free(kv->key);
            free(kv->sval);
            for (int k = 0; k < kv->nitems; k++) free(kv->items[k]);
            free(kv->items);
        }
        free(t->kv);
        free(t->path);
    }
    free(d->tables);
    free(d);
}

const toml_kv *toml_get(const toml_table *t, const char *key)
{
    if (!t) return NULL;
    for (int i = 0; i < t->nkv; i++)
        if (strcmp(t->kv[i].key, key) == 0) return &t->kv[i];
    return NULL;
}

const char *toml_str(const toml_table *t, const char *key, const char *def)
{
    const toml_kv *kv = toml_get(t, key);
    if (!kv) return def;
    if (kv->kind == TOML_STR) return kv->sval;
    return def;
}

long toml_int(const toml_table *t, const char *key, long def)
{
    const toml_kv *kv = toml_get(t, key);
    if (!kv) return def;
    if (kv->kind == TOML_INT) return kv->ival;
    if (kv->kind == TOML_STR) return strtol(kv->sval, NULL, 10);
    return def;
}
