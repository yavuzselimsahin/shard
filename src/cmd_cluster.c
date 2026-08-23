/* `shard cluster …` — everything that reads or edits cluster.toml. */

#include "shard.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(void)
{
    printf("Usage:\n"
           "  shard cluster init              create a cluster.toml here\n"
           "  shard cluster add-node          add a machine, interactively\n"
           "  shard cluster list              show the machines in the cluster\n"
           "  shard cluster health            check which machines answer\n"
           "  shard cluster remove <name>     drop a machine from the file\n");
}

static char *ask(const char *prompt, const char *def)
{
    char buf[512];
    if (def && *def) printf("%s [%s]: ", prompt, def);
    else             printf("%s: ", prompt);
    fflush(stdout);

    if (!fgets(buf, sizeof buf, stdin)) return def ? xstrdup(def) : NULL;
    char *s = str_trim(buf);
    if (!*s) return def && *def ? xstrdup(def) : NULL;
    return xstrdup(s);
}

static int do_init(void)
{
    const char *path = "cluster.toml";
    if (cluster_init_file(path) != 0) return 1;

    printf("Created %s.\n\n", path);
    printf("It starts with this machine only. Open it, add the machines you\n"
           "want to use, then run:\n\n"
           "  shard cluster health\n\n");
    return 0;
}

static int do_add_node(cluster_t *c)
{
    node_t n;
    memset(&n, 0, sizeof n);

    printf("Adding a machine to %s\n\n", c->config_path);
    n.name = ask("Name (how you will refer to it)", NULL);
    if (!n.name) { warn_msg("a name is required"); return 1; }
    if (cluster_node(c, n.name)) {
        warn_msg("there is already a node called %s", n.name);
        return 1;
    }

    n.host = ask("Host (IP or hostname)", n.name);
    n.user = ask("SSH user (blank for your current user)", NULL);
    n.key  = ask("SSH key file (blank for your default key)", NULL);

    char *tags = ask("Tags, comma separated (optional)", NULL);
    if (tags) {
        for (char *t = strtok(tags, ","); t && n.ntags < SHARD_MAX_TAGS;
             t = strtok(NULL, ",")) {
            char *v = str_trim(t);
            if (*v) n.tags[n.ntags++] = xstrdup(v);
        }
        free(tags);
    }

    char *cpu = ask("CPU cores (used to share work out; optional)", NULL);
    if (cpu) { n.cpu = atoi(cpu); free(cpu); }
    char *ram = ask("RAM in GB (optional)", NULL);
    if (ram) { n.ram_gb = atoi(ram); free(ram); }

    if (cluster_append_node(c->config_path, &n) != 0) {
        warn_msg("cannot write %s", c->config_path);
        return 1;
    }

    printf("\nAdded %s to %s.\n", n.name, c->config_path);
    printf("Check that it answers with:  shard cluster health\n");

    free(n.name); free(n.host); free(n.user); free(n.key);
    for (int i = 0; i < n.ntags; i++) free(n.tags[i]);
    return 0;
}

static void print_tags(const node_t *n, char *out, size_t len)
{
    out[0] = '\0';
    for (int i = 0; i < n->ntags; i++) {
        strncat(out, i ? "," : "", len - strlen(out) - 1);
        strncat(out, n->tags[i], len - strlen(out) - 1);
    }
}

static int do_list(cluster_t *c)
{
    printf("%s%s%s — %d node%s (%s)\n\n", c_bold(), c->name, c_reset(),
           c->nnodes, c->nnodes == 1 ? "" : "s", c->config_path);

    printf("%-16s %-28s %-6s %-7s %s\n", "NAME", "HOST", "CPU", "RAM", "TAGS");
    for (int i = 0; i < c->nnodes; i++) {
        node_t *n = &c->nodes[i];
        char tags[128];
        print_tags(n, tags, sizeof tags);

        char cpu[16] = "-", ram[16] = "-";
        if (n->cpu)    snprintf(cpu, sizeof cpu, "%d", n->cpu);
        if (n->ram_gb) snprintf(ram, sizeof ram, "%d GB", n->ram_gb);

        char host[32];
        snprintf(host, sizeof host, "%s%s", n->host, n->is_local ? " (local)" : "");

        printf("%-16s %-28s %-6s %-7s %s%s%s\n",
               n->name, host, cpu, ram, c_dim(), tags, c_reset());
    }
    return 0;
}

static void bar(int pct, char *out, size_t len)
{
    int filled = pct * 10 / 100;
    out[0] = '\0';
    for (int i = 0; i < 10 && strlen(out) + 4 < len; i++)
        strcat(out, i < filled ? "\xe2\x96\x93" : "\xe2\x96\x91");
}

static int do_health(cluster_t *c, const char *selector)
{
    node_t **nodes;
    int n = cluster_select(c, selector, &nodes);
    if (n == 0) {
        warn_msg("no nodes matched \"%s\"", selector ? selector : "all");
        free(nodes);
        return 1;
    }

    printf("Checking %d node%s…\n\n", n, n == 1 ? "" : "s");

    health_t *h = xmalloc(sizeof(health_t) * (size_t)n);
    int online = health_check(c, nodes, n, h);
    health_write(c, h, n);

    for (int i = 0; i < n; i++) {
        if (!h[i].online) {
            printf("%s\xe2\x97\x8b%s %-16s %soffline%s  %s%s%s\n",
                   c_dim(), c_reset(), h[i].name, c_red(), c_reset(),
                   c_dim(), h[i].error, c_reset());
            continue;
        }
        char cbar[64], rbar[64];
        bar(h[i].cpu_pct, cbar, sizeof cbar);
        bar(h[i].ram_pct, rbar, sizeof rbar);
        printf("%s\xe2\x97\x8f%s %-16s %sonline%s   %d CPU  %ld MB  "
               "%s %3d%% cpu  %s %3d%% ram  %s%.0f ms%s\n",
               c_green(), c_reset(), h[i].name, c_green(), c_reset(),
               h[i].ncpu, h[i].ram_mb, cbar, h[i].cpu_pct, rbar, h[i].ram_pct,
               c_dim(), h[i].ms, c_reset());
    }

    printf("\n%d/%d online\n", online, n);
    free(h);
    free(nodes);
    return online == n ? 0 : 1;
}

static int do_remove(cluster_t *c, const char *name)
{
    if (!cluster_node(c, name)) {
        warn_msg("no node called %s in %s", name, c->config_path);
        return 1;
    }
    int rc = cluster_remove_node(c->config_path, name);
    if (rc != 0) {
        warn_msg("could not remove %s from %s", name, c->config_path);
        return 1;
    }
    printf("Removed %s from %s.\n", name, c->config_path);
    return 0;
}

int cmd_cluster(cluster_t *c, int argc, char **argv)
{
    if (argc < 1) { usage(); return 1; }
    const char *sub = argv[0];

    if (str_eq(sub, "init"))     return do_init();
    if (str_eq(sub, "list"))     return do_list(c);
    if (str_eq(sub, "add-node")) return do_add_node(c);
    if (str_eq(sub, "remove")) {
        if (argc < 2) { warn_msg("usage: shard cluster remove <name>"); return 1; }
        return do_remove(c, argv[1]);
    }
    if (str_eq(sub, "health")) {
        const char *sel = NULL;
        for (int i = 1; i < argc; i++)
            if (str_eq(argv[i], "--on") && i + 1 < argc) sel = argv[++i];
        return do_health(c, sel);
    }

    warn_msg("unknown subcommand: cluster %s", sub);
    usage();
    return 1;
}
