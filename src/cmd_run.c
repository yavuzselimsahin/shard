/* `shard run` and `shard tasks` — running the work written down in
 * tasks.toml. The strategies are the same ones the typed commands use; only
 * where the arguments came from is different. */

#include "shard.h"

#include <stdlib.h>
#include <string.h>

static int parse_run_opts(int argc, char **argv, run_opts *o, const char **file)
{
    int rc = 0;
    for (int i = 0; i < argc; i++) {
        char *a = argv[i];
        if (str_eq(a, "--on") && i + 1 < argc)           o->on = argv[++i];
        else if (str_eq(a, "--jobs") && i + 1 < argc)    o->jobs = atoi(argv[++i]);
        else if (str_eq(a, "--timeout") && i + 1 < argc) o->timeout = atoi(argv[++i]);
        else if (str_eq(a, "--tasks") && i + 1 < argc)   *file = argv[++i];
        else if (str_eq(a, "--stream"))                  o->stream = 1;
        else if (str_eq(a, "--dry-run"))                 o->dry_run = 1;
        else if (str_eq(a, "--no-log"))                  o->no_log = 1;
        else if (str_eq(a, "--static"))                  o->static_split = 1;
        else if (str_eq(a, "--retry") && i + 1 < argc)   o->retry = atoi(argv[++i]);
        else if ((str_eq(a, "--with") || str_eq(a, "--ship")) && i + 1 < argc) {
            o->ship = xrealloc(o->ship, sizeof(char *) * (size_t)(o->nship + 1));
            o->ship[o->nship++] = xstrdup(argv[++i]);
        }
        else if (str_eq(a, "--keep"))                    o->keep_stage = 1;
        else if (str_eq(a, "--balance"))                 o->balance = 1;
        else if (str_has_prefix(a, "--")) {
            warn_msg("ignoring unknown option %s", a);
            rc = 1;
        }
    }
    return rc;
}

static void list_tasks(taskfile_t *tf)
{
    printf("%s%s%s — %d task%s\n\n", c_bold(), tf->path, c_reset(),
           tf->ntasks, tf->ntasks == 1 ? "" : "s");

    for (int i = 0; i < tf->ntasks; i++) {
        task_t *t = &tf->tasks[i];
        printf("%-24s %s%-11s%s", t->name, c_dim(), t->strategy, c_reset());
        if (t->description) printf(" %s", t->description);
        printf("\n");

        if (t->nsteps) {
            for (int s = 0; s < t->nsteps; s++)
                printf("  %s%-22s %s%s\n", c_dim(), t->steps[s].name,
                       t->steps[s].cmd, c_reset());
        } else if (t->cmd) {
            printf("  %s%s%s\n", c_dim(), t->cmd, c_reset());
        } else if (t->script) {
            printf("  %s%s%s\n", c_dim(), t->script, c_reset());
        }
    }
    printf("\nRun one with:  shard run <name>\n");
}

int cmd_tasks(cluster_t *c, int argc, char **argv)
{
    const char *file = NULL;
    run_opts o;
    run_opts_default(&o);
    parse_run_opts(argc, argv, &o, &file);

    taskfile_t *tf = tasks_load(file);
    if (!tf) return 1;

    list_tasks(tf);
    tasks_free(tf);
    return 0;
}

int cmd_run(cluster_t *c, int argc, char **argv)
{
    run_opts o;
    run_opts_default(&o);

    const char *file = NULL;
    const char *name = NULL;
    for (int i = 0; i < argc; i++)
        if (!str_has_prefix(argv[i], "--") &&
            !(i > 0 && (str_eq(argv[i - 1], "--on") ||
                        str_eq(argv[i - 1], "--jobs") ||
                        str_eq(argv[i - 1], "--timeout") ||
                        str_eq(argv[i - 1], "--retry") ||
                        str_eq(argv[i - 1], "--with") ||
                        str_eq(argv[i - 1], "--ship") ||
                        str_eq(argv[i - 1], "--tasks"))))
            name = argv[i];

    int cli_on = 0;
    for (int i = 0; i < argc; i++) if (str_eq(argv[i], "--on")) cli_on = 1;
    parse_run_opts(argc, argv, &o, &file);

    taskfile_t *tf = tasks_load(file);
    if (!tf) return 1;

    if (!name) {
        list_tasks(tf);
        tasks_free(tf);
        return 1;
    }

    task_t *t = tasks_get(tf, name);
    if (!t) {
        warn_msg("no task called \"%s\" in %s", name, tf->path);
        printf("\n");
        list_tasks(tf);
        tasks_free(tf);
        return 1;
    }

    /* The task's own settings are defaults; anything typed on the command
     * line wins, because that is the thing you decided most recently. */
    if (t->on && !cli_on)   o.on = t->on;
    if (t->timeout && !o.timeout) o.timeout = t->timeout;
    if (t->jobs && !o.jobs)       o.jobs = t->jobs;
    if (t->retry && !o.retry)     o.retry = t->retry;
    if (t->nwith && !o.nship) { o.ship = t->with; o.nship = t->nwith; }
    o.count = t->count;
    o.start = t->start;
    o.label = t->name;

    printf("%s%s%s", c_bold(), t->name, c_reset());
    if (t->description) printf(" — %s", t->description);
    printf("\n");

    int rc;
    if (str_eq(t->strategy, "pipeline")) {
        rc = strategy_steps(c, t->steps, t->nsteps, 1, &o);
    } else if (str_eq(t->strategy, "steps") || str_eq(t->strategy, "parallel") ||
               (str_eq(t->strategy, "distribute") && t->nsteps > 0)) {
        rc = strategy_steps(c, t->steps, t->nsteps, 0, &o);
    } else if (str_eq(t->strategy, "map")) {
        if (!t->cmd) { warn_msg("task \"%s\" has no cmd", t->name); rc = 1; }
        else rc = strategy_map(c, t->cmd, &o);
    } else if (str_eq(t->strategy, "distribute")) {
        if (!t->script) { warn_msg("task \"%s\" has no script", t->name); rc = 1; }
        else rc = strategy_script(c, t->script, 1, &o);
    } else if (str_eq(t->strategy, "script")) {
        if (!t->script) { warn_msg("task \"%s\" has no script", t->name); rc = 1; }
        else rc = strategy_script(c, t->script, 0, &o);
    } else if (str_eq(t->strategy, "broadcast")) {
        if (!t->cmd) { warn_msg("task \"%s\" has no cmd", t->name); rc = 1; }
        else rc = strategy_broadcast(c, t->cmd, &o);
    } else {
        warn_msg("task \"%s\": unknown strategy \"%s\"", t->name, t->strategy);
        rc = 1;
    }

    tasks_free(tf);
    return rc;
}
