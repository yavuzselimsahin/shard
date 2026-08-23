/* tasks.toml: work you run often, written down once.
 *
 * The file is a list of `[[task]]` blocks, each optionally followed by its
 * `[[task.step]]` blocks. Because the TOML reader keeps tables in file order,
 * a step belongs to the task above it — which is what TOML means anyway.
 */

#include "shard.h"

#include <stdlib.h>
#include <string.h>

static const char *TASKS_NAME = "tasks.toml";

char *tasks_find_file(void)
{
    const char *env = getenv("SHARD_TASKS");
    if (env && *env) {
        char *p = expand_path(env);
        if (file_exists(p)) return p;
        free(p);
        return NULL;
    }

    if (file_exists(TASKS_NAME)) return xstrdup(TASKS_NAME);

    char *home = home_dir();
    char *p = xasprintf("%s/.shard/%s", home, TASKS_NAME);
    free(home);
    if (file_exists(p)) return p;
    free(p);
    return NULL;
}

/* `on` may be one name or a list of them; both end up as the selector string
 * the rest of shard already understands. */
static char *selector_of(const toml_table *t)
{
    const toml_kv *kv = toml_get(t, "on");
    if (!kv) return NULL;
    if (kv->kind == TOML_STR) return xstrdup(kv->sval);
    if (kv->kind != TOML_ARRAY) return NULL;

    strbuf b;
    sb_init(&b);
    for (int i = 0; i < kv->nitems; i++) {
        if (i) sb_puts(&b, ",");
        sb_puts(&b, kv->items[i]);
    }
    return b.data;
}

/* An unstated strategy is the one implied by what the task actually holds. */
static const char *implied_strategy(const task_t *t)
{
    if (t->nsteps > 0) return "steps";
    if (t->count > 0)  return "map";
    if (t->script)     return "distribute";
    return "broadcast";
}

taskfile_t *tasks_load(const char *path)
{
    char *found = NULL;
    if (!path) {
        found = tasks_find_file();
        if (!found) {
            warn_msg("no tasks.toml here or in ~/.shard/");
            return NULL;
        }
        path = found;
    }

    char err[256];
    toml_doc *doc = toml_parse_file(path, err, sizeof err);
    if (!doc) {
        warn_msg("%s: %s", path, err[0] ? err : "parse error");
        free(found);
        return NULL;
    }

    taskfile_t *tf = xmalloc(sizeof *tf);
    memset(tf, 0, sizeof *tf);
    tf->path = xstrdup(path);

    for (int i = 0; i < doc->ntables; i++) {
        toml_table *t = &doc->tables[i];

        if (strcmp(t->path, "task") == 0) {
            tf->tasks = xrealloc(tf->tasks, sizeof(task_t) * (size_t)(tf->ntasks + 1));
            task_t *task = &tf->tasks[tf->ntasks++];
            memset(task, 0, sizeof *task);

            const char *name = toml_str(t, "name", NULL);
            task->name        = xstrdup(name ? name : "unnamed");
            const char *d     = toml_str(t, "description", NULL);
            task->description = d ? xstrdup(d) : NULL;
            const char *st    = toml_str(t, "strategy", NULL);
            task->strategy    = st ? xstrdup(st) : NULL;
            const char *cmd   = toml_str(t, "cmd", NULL);
            task->cmd         = cmd ? xstrdup(cmd) : NULL;
            const char *sc    = toml_str(t, "script", NULL);
            task->script      = sc ? xstrdup(sc) : NULL;
            task->on          = selector_of(t);
            task->count       = (int)toml_int(t, "count", 0);
            task->start       = (int)toml_int(t, "start", 0);
            task->timeout     = (int)toml_int(t, "timeout", 0);
            task->jobs        = (int)toml_int(t, "jobs", 0);
        task->retry       = (int)toml_int(t, "retry", 0);

        const toml_kv *w = toml_get(t, "with");
        if (w && w->kind == TOML_ARRAY) {
            task->nwith = w->nitems;
            task->with = xmalloc(sizeof(char *) * (size_t)w->nitems);
            for (int k = 0; k < w->nitems; k++)
                task->with[k] = xstrdup(w->items[k]);
        } else if (w && w->kind == TOML_STR) {
            task->nwith = 1;
            task->with = xmalloc(sizeof(char *));
            task->with[0] = xstrdup(w->sval);
        }
            continue;
        }

        if (strcmp(t->path, "task.step") != 0) continue;

        if (tf->ntasks == 0) {
            warn_msg("%s: a [[task.step]] appears before any [[task]], skipped", path);
            continue;
        }

        task_t *task = &tf->tasks[tf->ntasks - 1];
        task->steps = xrealloc(task->steps, sizeof(step_t) * (size_t)(task->nsteps + 1));
        step_t *step = &task->steps[task->nsteps++];
        memset(step, 0, sizeof *step);

        const char *sname = toml_str(t, "name", NULL);
        step->name = sname ? xstrdup(sname) : xasprintf("%d", task->nsteps);
        const char *scmd = toml_str(t, "cmd", NULL);
        step->cmd = xstrdup(scmd ? scmd : "true");
        step->on = selector_of(t);

        const toml_kv *prod = toml_get(t, "produces");
        if (prod && prod->kind == TOML_ARRAY) {
            step->nproduces = prod->nitems;
            step->produces = xmalloc(sizeof(char *) * (size_t)prod->nitems);
            for (int k = 0; k < prod->nitems; k++)
                step->produces[k] = xstrdup(prod->items[k]);
        } else if (prod && prod->kind == TOML_STR) {
            step->nproduces = 1;
            step->produces = xmalloc(sizeof(char *));
            step->produces[0] = xstrdup(prod->sval);
        }

        if (!scmd)
            warn_msg("%s: step \"%s\" has no cmd", path, step->name);
    }

    toml_free(doc);
    free(found);

    for (int i = 0; i < tf->ntasks; i++)
        if (!tf->tasks[i].strategy)
            tf->tasks[i].strategy = xstrdup(implied_strategy(&tf->tasks[i]));

    return tf;
}

void tasks_free(taskfile_t *tf)
{
    if (!tf) return;
    for (int i = 0; i < tf->ntasks; i++) {
        task_t *t = &tf->tasks[i];
        free(t->name); free(t->description); free(t->strategy);
        free(t->cmd); free(t->script); free(t->on);
        for (int k = 0; k < t->nwith; k++) free(t->with[k]);
        free(t->with);
        for (int s = 0; s < t->nsteps; s++) {
            free(t->steps[s].name);
            free(t->steps[s].cmd);
            free(t->steps[s].on);
            for (int k = 0; k < t->steps[s].nproduces; k++)
                free(t->steps[s].produces[k]);
            free(t->steps[s].produces);
        }
        free(t->steps);
    }
    free(tf->tasks);
    free(tf->path);
    free(tf);
}

task_t *tasks_get(taskfile_t *tf, const char *name)
{
    for (int i = 0; i < tf->ntasks; i++)
        if (str_eq(tf->tasks[i].name, name)) return &tf->tasks[i];
    return NULL;
}
