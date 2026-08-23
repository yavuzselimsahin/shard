/* Reading cluster.toml into the model the rest of the program uses, and the
 * three edits `shard cluster` makes to that file. */

#include "shard.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *CONFIG_NAME = "cluster.toml";

char *cluster_find_config(void)
{
    const char *env = getenv("SHARD_CONFIG");
    if (env && *env) {
        char *p = expand_path(env);
        if (file_exists(p)) return p;
        free(p);
        return NULL;
    }

    if (file_exists(CONFIG_NAME)) return xstrdup(CONFIG_NAME);

    char *home = home_dir();
    char *p = xasprintf("%s/.shard/%s", home, CONFIG_NAME);
    free(home);
    if (file_exists(p)) return p;
    free(p);
    return NULL;
}

static int host_is_local(const char *host)
{
    return str_eq(host, "localhost") || str_eq(host, "127.0.0.1") ||
           str_eq(host, "::1");
}

cluster_t *cluster_load(const char *path)
{
    char *found = NULL;
    if (!path) {
        found = cluster_find_config();
        if (!found)
            die("no cluster.toml found. Run `shard cluster init` to create one.");
        path = found;
    }

    char err[256];
    toml_doc *doc = toml_parse_file(path, err, sizeof err);
    if (!doc) die("%s: %s", path, err[0] ? err : "parse error");

    cluster_t *c = xmalloc(sizeof *c);
    memset(c, 0, sizeof *c);
    c->config_path = xstrdup(path);
    c->name = xstrdup("cluster");

    for (int i = 0; i < doc->ntables; i++) {
        toml_table *t = &doc->tables[i];

        if (strcmp(t->path, "cluster") == 0) {
            const char *n = toml_str(t, "name", NULL);
            if (n) { free(c->name); c->name = xstrdup(n); }
            const char *ld = toml_str(t, "log_dir", NULL);
            if (ld) c->log_dir = expand_path(ld);
            continue;
        }

        if (strcmp(t->path, "nodes") != 0) continue;

        const char *name = toml_str(t, "name", NULL);
        const char *host = toml_str(t, "host", NULL);
        if (!name && !host) {
            warn_msg("%s: a [[nodes]] entry has neither name nor host, skipped",
                     path);
            continue;
        }

        c->nodes = xrealloc(c->nodes, sizeof(node_t) * (size_t)(c->nnodes + 1));
        node_t *n = &c->nodes[c->nnodes++];
        memset(n, 0, sizeof *n);

        n->host = xstrdup(host ? host : name);
        n->name = xstrdup(name ? name : host);
        const char *user = toml_str(t, "user", NULL);
        if (user) n->user = xstrdup(user);
        const char *key = toml_str(t, "key", NULL);
        if (key) n->key = expand_path(key);
        const char *wd = toml_str(t, "workdir", NULL);
        if (wd) n->workdir = xstrdup(wd);
        n->port   = (int)toml_int(t, "port", 0);
        n->cpu    = (int)toml_int(t, "cpu", 0);
        n->ram_gb = (int)toml_int(t, "ram_gb", 0);

        const toml_kv *local = toml_get(t, "local");
        n->is_local = local ? (local->kind == TOML_BOOL ? local->bval : 0)
                            : host_is_local(n->host);

        const toml_kv *tags = toml_get(t, "tags");
        if (tags && tags->kind == TOML_ARRAY) {
            for (int k = 0; k < tags->nitems && n->ntags < SHARD_MAX_TAGS; k++)
                n->tags[n->ntags++] = xstrdup(tags->items[k]);
        }
    }

    toml_free(doc);
    free(found);

    if (!c->log_dir) {
        char *home = home_dir();
        c->log_dir = xasprintf("%s/.shard/logs", home);
        free(home);
    }
    return c;
}

void cluster_free(cluster_t *c)
{
    if (!c) return;
    for (int i = 0; i < c->nnodes; i++) {
        node_t *n = &c->nodes[i];
        free(n->name); free(n->host); free(n->user);
        free(n->key);  free(n->workdir);
        for (int k = 0; k < n->ntags; k++) free(n->tags[k]);
    }
    free(c->nodes);
    free(c->name);
    free(c->log_dir);
    free(c->config_path);
    free(c);
}

node_t *cluster_node(cluster_t *c, const char *name)
{
    for (int i = 0; i < c->nnodes; i++)
        if (str_eq(c->nodes[i].name, name)) return &c->nodes[i];
    return NULL;
}

static int node_has_tag(const node_t *n, const char *tag)
{
    for (int i = 0; i < n->ntags; i++)
        if (str_eq(n->tags[i], tag)) return 1;
    return 0;
}

int node_weight(const node_t *n)
{
    return n->cpu > 0 ? n->cpu : 1;
}

int cluster_select(cluster_t *c, const char *selector, node_t ***out)
{
    node_t **sel = xmalloc(sizeof(node_t *) * (size_t)(c->nnodes ? c->nnodes : 1));
    int count = 0;

    if (!selector || !*selector || str_eq(selector, "all")) {
        for (int i = 0; i < c->nnodes; i++) sel[count++] = &c->nodes[i];
        *out = sel;
        return count;
    }

    char *copy = xstrdup(selector);
    for (char *tok = strtok(copy, ","); tok; tok = strtok(NULL, ",")) {
        char *want = str_trim(tok);
        if (!*want) continue;
        for (int i = 0; i < c->nnodes; i++) {
            node_t *n = &c->nodes[i];
            if (!str_eq(n->name, want) && !node_has_tag(n, want)) continue;

            int already = 0;
            for (int j = 0; j < count; j++)
                if (sel[j] == n) already = 1;
            if (!already) sel[count++] = n;
        }
    }
    free(copy);

    *out = sel;
    return count;
}

/* ------------------------------------------------------------ file edits */

static const char *INIT_TEMPLATE =
    "# shard cluster definition\n"
    "#\n"
    "# Every machine you can reach over SSH can be a node. Add one block per\n"
    "# machine; `shard cluster health` tells you which ones answer.\n"
    "\n"
    "[cluster]\n"
    "name    = \"%s\"\n"
    "log_dir = \"~/.shard/logs\"\n"
    "\n"
    "# This machine. Jobs sent here run directly, without SSH.\n"
    "[[nodes]]\n"
    "name = \"local\"\n"
    "host = \"localhost\"\n"
    "tags = [\"local\"]\n"
    "cpu  = %d\n"
    "\n"
    "# A machine on your network:\n"
    "#\n"
    "# [[nodes]]\n"
    "# name   = \"old-desktop\"\n"
    "# host   = \"192.168.1.15\"\n"
    "# user   = \"you\"\n"
    "# tags   = [\"cpu\"]\n"
    "# cpu    = 2\n"
    "# ram_gb = 4\n"
    "#\n"
    "# A cloud machine — same block, different address:\n"
    "#\n"
    "# [[nodes]]\n"
    "# name   = \"ec2-worker\"\n"
    "# host   = \"ec2-13-58-0-1.compute.amazonaws.com\"\n"
    "# user   = \"ubuntu\"\n"
    "# key    = \"~/.ssh/aws-key.pem\"\n"
    "# tags   = [\"cloud\", \"cpu\"]\n"
    "# cpu    = 4\n"
    "# ram_gb = 8\n";

int cluster_init_file(const char *path)
{
    if (file_exists(path)) {
        warn_msg("%s already exists, leaving it alone", path);
        return 1;
    }

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;

    /* The machine's own name makes a decent cluster name — unless the box
     * answers with an IP address, which makes a poor one. */
    char host[128] = "";
    if (gethostname(host, sizeof host) != 0) host[0] = '\0';
    host[sizeof host - 1] = '\0';
    char *dot = strchr(host, '.');
    if (dot) *dot = '\0';
    if (!host[0] || (host[0] >= '0' && host[0] <= '9'))
        snprintf(host, sizeof host, "home-lab");

    FILE *f = fopen(path, "w");
    if (!f) { warn_msg("cannot write %s", path); return 1; }
    fprintf(f, INIT_TEMPLATE, host, (int)ncpu);
    fclose(f);
    return 0;
}

int cluster_append_node(const char *path, const node_t *n)
{
    FILE *f = fopen(path, "a");
    if (!f) return -1;

    fprintf(f, "\n[[nodes]]\nname = \"%s\"\nhost = \"%s\"\n", n->name, n->host);
    if (n->user)    fprintf(f, "user = \"%s\"\n", n->user);
    if (n->key)     fprintf(f, "key  = \"%s\"\n", n->key);
    if (n->port)    fprintf(f, "port = %d\n", n->port);
    if (n->ntags) {
        fprintf(f, "tags = [");
        for (int i = 0; i < n->ntags; i++)
            fprintf(f, "%s\"%s\"", i ? ", " : "", n->tags[i]);
        fprintf(f, "]\n");
    }
    if (n->cpu)     fprintf(f, "cpu  = %d\n", n->cpu);
    if (n->ram_gb)  fprintf(f, "ram_gb = %d\n", n->ram_gb);
    fclose(f);
    return 0;
}

/* Removing a node is a line edit rather than a re-serialisation, so the
 * comments you wrote in cluster.toml survive it. */
int cluster_remove_node(const char *path, const char *name)
{
    size_t len;
    char *text = read_file(path, &len);
    if (!text) return -1;

    strbuf out;
    sb_init(&out);

    char *line = text;
    int in_block = 0, removed = 0;
    char *needle = xasprintf("\"%s\"", name);

    while (line && *line != '\0') {
        char *nl = strchr(line, '\n');
        size_t linelen = nl ? (size_t)(nl - line) : strlen(line);
        char *copy = xstrndup(line, linelen);
        char *trimmed = str_trim(copy);

        if (str_has_prefix(trimmed, "[[nodes]]")) {
            /* Look ahead: does this block belong to the node being removed? */
            char *scan = nl ? nl + 1 : NULL;
            int match = 0;
            while (scan && *scan) {
                char *snl = strchr(scan, '\n');
                size_t slen = snl ? (size_t)(snl - scan) : strlen(scan);
                char *sc = xstrndup(scan, slen);
                char *st = str_trim(sc);
                if (st[0] == '[') { free(sc); break; }
                if (str_has_prefix(st, "name") && strstr(st, needle)) match = 1;
                free(sc);
                if (!snl) break;
                scan = snl + 1;
            }
            in_block = match;
            if (match) removed = 1;
        } else if (trimmed[0] == '[') {
            in_block = 0;
        }

        if (!in_block) {
            sb_add(&out, line, linelen);
            sb_puts(&out, "\n");
        }

        free(copy);
        if (!nl) break;
        line = nl + 1;
    }

    free(needle);
    free(text);

    if (!removed) { sb_free(&out); return 1; }

    FILE *f = fopen(path, "w");
    if (!f) { sb_free(&out); return -1; }
    fwrite(out.data, 1, out.len, f);
    fclose(f);
    sb_free(&out);
    return 0;
}
