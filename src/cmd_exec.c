/* The commands that put work on the cluster. Each one parses a command line
 * and hands the result to a strategy; the work itself lives in strategy.c. */

#include "shard.h"

#include <stdlib.h>
#include <string.h>

static void add_ship(run_opts *o, const char *path)
{
    for (int i = 0; i < o->nship; i++)
        if (str_eq(o->ship[i], path)) return;      /* --with may be parsed twice */
    o->ship = xrealloc(o->ship, sizeof(char *) * (size_t)(o->nship + 1));
    o->ship[o->nship++] = xstrdup(path);
}

/* Returns the index of the first argument that is not an option. */
static int parse_opts(int argc, char **argv, int from, run_opts *o)
{
    int i = from;
    for (; i < argc; i++) {
        char *a = argv[i];
        if (str_eq(a, "--on") && i + 1 < argc)           o->on = argv[++i];
        else if (str_eq(a, "--jobs") && i + 1 < argc)    o->jobs = atoi(argv[++i]);
        else if (str_eq(a, "--timeout") && i + 1 < argc) o->timeout = atoi(argv[++i]);
        else if (str_eq(a, "--count") && i + 1 < argc)   o->count = atoi(argv[++i]);
        else if (str_eq(a, "--start") && i + 1 < argc)   o->start = atoi(argv[++i]);
        else if (str_eq(a, "--stream"))                  o->stream = 1;
        else if (str_eq(a, "--dry-run"))                 o->dry_run = 1;
        else if (str_eq(a, "--no-log"))                  o->no_log = 1;
        else if (str_eq(a, "--static"))                  o->static_split = 1;
        else if (str_eq(a, "--retry") && i + 1 < argc)   o->retry = atoi(argv[++i]);
        else if ((str_eq(a, "--with") || str_eq(a, "--ship")) && i + 1 < argc)
            add_ship(o, argv[++i]);
        else if (str_eq(a, "--keep"))                    o->keep_stage = 1;
        else if (str_eq(a, "--balance"))                 o->balance = 1;
        else if (str_eq(a, "--distribute"))              ;  /* read by the caller */
        else if (str_has_prefix(a, "--"))
            warn_msg("ignoring unknown option %s", a);
        else break;
    }
    return i;
}

int cmd_exec(cluster_t *c, int argc, char **argv)
{
    run_opts o;
    run_opts_default(&o);

    int i = parse_opts(argc, argv, 0, &o);
    if (i >= argc) {
        warn_msg("usage: shard exec \"<command>\" [--on all|<tag>|<node>]");
        return 1;
    }

    /* The command may be quoted as one argument, or left bare on the command
     * line; joining whatever is left over covers both. */
    strbuf cmd;
    sb_init(&cmd);
    for (int k = i; k < argc; k++) {
        if (str_has_prefix(argv[k], "--")) {
            parse_opts(argc, argv, k, &o);
            break;
        }
        if (k > i) sb_puts(&cmd, " ");
        sb_puts(&cmd, argv[k]);
    }

    int rc = strategy_broadcast(c, cmd.data, &o);
    sb_free(&cmd);
    return rc;
}

int cmd_exec_script(cluster_t *c, int argc, char **argv)
{
    run_opts o;
    run_opts_default(&o);

    int distribute = 0;
    for (int i = 0; i < argc; i++)
        if (str_eq(argv[i], "--distribute")) distribute = 1;

    int i = parse_opts(argc, argv, 0, &o);
    if (i >= argc) {
        warn_msg("usage: shard exec-script <file> [--distribute] [--on <sel>]");
        return 1;
    }
    const char *path = argv[i];
    parse_opts(argc, argv, i + 1, &o);

    return strategy_script(c, path, distribute, &o);
}

int cmd_map(cluster_t *c, int argc, char **argv)
{
    run_opts o;
    run_opts_default(&o);

    int i = parse_opts(argc, argv, 0, &o);
    if (i >= argc) {
        warn_msg("usage: shard map \"<command with {i}>\" --count <n>");
        return 1;
    }
    const char *tmpl = argv[i];
    parse_opts(argc, argv, i + 1, &o);

    return strategy_map(c, tmpl, &o);
}

/* shard pipeline --step "cmd" --on <sel> --step "cmd" --on <sel> …
 *
 * `--on` after a `--step` belongs to that step; before the first one it is
 * the default for steps that do not name their own. */
int cmd_pipeline(cluster_t *c, int argc, char **argv)
{
    run_opts o;
    run_opts_default(&o);

    step_t *steps = NULL;
    int nsteps = 0;
    int parallel = 0;

    for (int i = 0; i < argc; i++) {
        char *a = argv[i];

        if (str_eq(a, "--step") && i + 1 < argc) {
            steps = xrealloc(steps, sizeof(step_t) * (size_t)(nsteps + 1));
            memset(&steps[nsteps], 0, sizeof(step_t));
            steps[nsteps].cmd = xstrdup(argv[++i]);
            steps[nsteps].name = xasprintf("%d", nsteps + 1);
            nsteps++;
        } else if (str_eq(a, "--produces") && i + 1 < argc && nsteps > 0) {
            step_t *st = &steps[nsteps - 1];
            st->produces = xrealloc(st->produces,
                                    sizeof(char *) * (size_t)(st->nproduces + 1));
            st->produces[st->nproduces++] = xstrdup(argv[++i]);
        } else if (str_eq(a, "--name") && i + 1 < argc && nsteps > 0) {
            free(steps[nsteps - 1].name);
            steps[nsteps - 1].name = xstrdup(argv[++i]);
        } else if (str_eq(a, "--on") && i + 1 < argc) {
            if (nsteps > 0) steps[nsteps - 1].on = xstrdup(argv[++i]);
            else            o.on = argv[++i];
        } else if (str_eq(a, "--parallel")) {
            parallel = 1;
        } else {
            parse_opts(argc, argv, i, &o);
        }
    }

    if (nsteps == 0) {
        warn_msg("usage: shard pipeline --step \"<command>\" [--on <sel>] "
                 "[--produces <file>] --step \"<command>\" …");
        return 1;
    }

    o.label = "pipeline";
    int rc = strategy_steps(c, steps, nsteps, !parallel, &o);

    for (int i = 0; i < nsteps; i++) {
        free(steps[i].name);
        free(steps[i].cmd);
        free(steps[i].on);
        for (int k = 0; k < steps[i].nproduces; k++) free(steps[i].produces[k]);
        free(steps[i].produces);
    }
    free(steps);
    return rc;
}
