/* The strategies: the four shapes work can take.
 *
 * Each one turns a request — a command, a script, an index range, a list of
 * steps — into jobs for the runner. `shard exec` and friends call these after
 * parsing the command line; `shard run` calls them after reading tasks.toml.
 * Neither knows how the other got here.
 */

#include "shard.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static node_t **measure_online(cluster_t *c, node_t **nodes, int n,
                               int **weights_out, int *nlive, int quiet);

void run_opts_default(run_opts *o)
{
    memset(o, 0, sizeof *o);
    o->on = "all";
}

void shard_show_plan(runner_t *r)
{
    for (int i = 0; i < r->njobs; i++) {
        char **argv = job_argv(&r->jobs[i]);
        printf("[%s]", r->jobs[i].label);
        for (int a = 0; argv[a]; a++) printf(" %s", argv[a]);
        printf("\n");
        argv_free(argv);
    }
}

void shard_summary(runner_t *r, int failed)
{
    double first = 0, last = 0, total = 0;
    int ok = 0, skipped = 0;
    for (int i = 0; i < r->njobs; i++) {
        job_t *j = &r->jobs[i];
        if (j->state == JOB_OK) ok++;
        if (j->state == JOB_SKIPPED) skipped++;
        if (j->start <= 0) continue;
        if (first == 0 || j->start < first) first = j->start;
        if (j->end > last) last = j->end;
        total += j->end - j->start;
    }

    char wall_s[32], total_s[32];
    double wall = last - first;
    fmt_duration(wall, wall_s, sizeof wall_s);
    fmt_duration(total, total_s, sizeof total_s);

    if (r->items_total > 0) {
        /* Workers are an implementation detail; what was asked for was items. */
        printf("\n%d/%d items completed in %s",
               r->items_done - r->items_failed, r->items_total, wall_s);
        if (r->items_failed) printf(", %d failed", r->items_failed);
        printf("\n");
    } else {
        printf("\n%d/%d %s in %s", ok, r->njobs,
               failed ? "succeeded" : "completed", wall_s);
        if (failed)  printf(", %d failed", failed);
        if (skipped) printf(", %d not run", skipped);
        printf("\n");
    }

    /* One job at a time is a pipeline: comparing it with running the same
     * steps one after another would be comparing it with itself. */
    if (r->njobs > 1 && total > 0.01 && r->max_parallel != 1) {
        int speedup = (int)((1.0 - wall / total) * 100.0 + 0.5);
        if (speedup < 0) speedup = 0;
        printf("%sTotal machine time: %s (%d%% faster than one after another)%s\n",
               c_dim(), total_s, speedup, c_reset());
    }
    if (r->task_id)
        printf("%sLogs: shard logs %s%s\n", c_dim(), r->task_id, c_reset());
}

/* Gives the runner its log directory, runs it, and reports. Strategies that
 * need the directory earlier (they write chunk files into it) create it
 * themselves and pass it in. */
static int finish(cluster_t *c, runner_t *r, const run_opts *o,
                  char *task_dir, char *task_id)
{
    if (o->dry_run) {
        shard_show_plan(r);
        free(task_dir);
        free(task_id);
        return 0;
    }

    if (!task_dir && !o->no_log) {
        task_dir = task_dir_create(c, &task_id);
    }
    r->task_dir = task_dir;
    r->task_id = task_id;
    r->max_parallel = o->jobs;
    r->timeout = o->timeout;
    r->stream = o->stream;
    r->retries = o->retry;
    r->ship = o->ship;
    r->nship = o->nship;
    r->keep_stage = o->keep_stage;

    int failed = runner_run(r);
    shard_summary(r, failed);

    free(task_dir);
    free(task_id);
    return failed ? 1 : 0;
}


/* ------------------------------------------------------- live dispatch */

/* Work handed out as it is asked for.
 *
 * Each worker runs a small read-and-run loop and says so after every item;
 * the dispatcher answers with the next one. Nothing is decided in advance, so
 * a machine that turns out to be slow simply asks for less. Two items are
 * kept in front of each worker, so it never waits for the network between
 * one item and the next. */
static const char *DISPATCH_MARKER = "__shard_item__";

/* Every item runs in a subshell, so an item that calls `exit` ends itself
 * rather than the worker reading the queue.
 *
 * The worker always ends successfully: whether the items it ran worked out is
 * counted here, not there. A worker that exits non-zero has therefore really
 * gone wrong — a dropped connection, a shell that died — which is what makes
 * retrying the worker itself meaningful. */
static const char *WORKER_LOOP =
    "while IFS= read -r _shard_cmd; do "
    "[ -z \"$_shard_cmd\" ] && continue; "
    "( eval \"$_shard_cmd\" ) </dev/null; "
    "echo \"__shard_item__ $?\"; "
    "done";

#define DISPATCH_DEPTH 2                 /* items kept in front of a worker */
#define DISPATCH_QUEUE (DISPATCH_DEPTH + 2)

typedef struct {
    const char  *tmpl;      /* map: the command, with {i} still in it */
    char       **lines;     /* distribute: one command per item */
    int          nitems;
    int          start;     /* map: the number the first item gets */
    int          next;      /* the next item never handed out */

    int         *attempts;  /* per item */
    int         *last_node; /* per item: which machine ran it last, or -1 */
    int         *retry_q;   /* items waiting to be tried again */
    int          retry_n;
    int          max_attempts;

    int         *held;      /* njobs * DISPATCH_QUEUE ring of items in flight */
    int         *held_n;
    int         *per_job;
    int          njobs;

    node_t     **nodes;
    int          nnodes;

    double       last_print;
    double       began;
    runner_t    *runner;
} dispatch_t;

static int node_index(dispatch_t *d, const node_t *n)
{
    for (int i = 0; i < d->nnodes; i++) if (d->nodes[i] == n) return i;
    return -1;
}

static char *item_command(dispatch_t *d, int k)
{
    if (d->lines) return xstrdup(d->lines[k]);

    char *num = xasprintf("%d", d->start + k);
    char *one = str_replace(d->tmpl, "{i}", num);
    char *cmd = str_replace(one, "{index}", num);
    free(one);
    free(num);
    return cmd;
}

/* One line, rewritten in place, so a thousand items do not scroll a thousand
 * lines past you. It is only drawn on a terminal. */
static void show_progress(dispatch_t *d, int force)
{
    runner_t *r = d->runner;
    if (r->quiet || r->stream || !color_enabled()) return;

    double now = now_seconds();
    if (!force && now - d->last_print < 0.2) return;
    d->last_print = now;

    double per = r->items_done > 0 ? (now - d->began) / r->items_done : 0;
    int left = d->nitems - r->items_done;
    char eta[32] = "";
    if (per > 0 && left > 0) {
        char t[32];
        fmt_duration(per * left, t, sizeof t);
        snprintf(eta, sizeof eta, " \xc2\xb7 about %s left", t);
    }

    printf("\r\033[K  %s%d/%d items", c_dim(), r->items_done, d->nitems);
    if (r->items_failed)  printf(" \xc2\xb7 %d failed", r->items_failed);
    if (r->items_retried) printf(" \xc2\xb7 %d retried", r->items_retried);
    printf("%s%s", eta, c_reset());
    fflush(stdout);
}

/* Prefers an item that failed somewhere else: the same machine failing it
 * twice teaches nothing, and a broken machine is exactly what retrying is
 * for. Falls back to any waiting item. */
static int take_retry(dispatch_t *d, const node_t *node)
{
    if (d->retry_n == 0) return -1;

    int here = node_index(d, node);
    int pick = 0;
    for (int i = 0; i < d->retry_n; i++)
        if (d->last_node[d->retry_q[i]] != here) { pick = i; break; }

    int item = d->retry_q[pick];
    memmove(&d->retry_q[pick], &d->retry_q[pick + 1],
            sizeof(int) * (size_t)(d->retry_n - pick - 1));
    d->retry_n--;
    return item;
}

static void queue_retry(dispatch_t *d, int item)
{
    d->retry_q[d->retry_n++] = item;
}

static int total_held(dispatch_t *d)
{
    int n = 0;
    for (int i = 0; i < d->njobs; i++) n += d->held_n[i];
    return n;
}

/* Nothing left to hand out, nothing waiting to be retried and nothing still
 * running: closing every worker's stdin is how they learn to stop. */
static void maybe_finish(dispatch_t *d)
{
    if (d->next < d->nitems || d->retry_n > 0 || total_held(d) > 0) return;
    for (int i = 0; i < d->njobs; i++) job_end_input(&d->runner->jobs[i]);
}

static void dispatch_feed(dispatch_t *d, job_t *j, int howmany)
{
    int idx = (int)(j - d->runner->jobs);

    for (int k = 0; k < howmany; k++) {
        if (d->held_n[idx] >= DISPATCH_QUEUE) break;

        int item = take_retry(d, j->node);
        if (item < 0) {
            if (d->next >= d->nitems) break;
            item = d->next++;
        }

        char *cmd = item_command(d, item);
        job_send(j, cmd);
        free(cmd);

        d->held[idx * DISPATCH_QUEUE + d->held_n[idx]++] = item;
    }
    maybe_finish(d);
}

/* A worker only asks for work when it finishes something, so an item that
 * comes back into the queue while every other worker is idle would sit there
 * for ever. After anything is requeued, the idle workers are offered it. */
static void dispatch_kick(dispatch_t *d)
{
    runner_t *r = d->runner;
    for (int i = 0; i < r->njobs && d->retry_n > 0; i++) {
        job_t *j = &r->jobs[i];
        if (j->state != JOB_RUNNING || j->in_fd < 0) continue;
        if (d->held_n[i] > 0) continue;
        dispatch_feed(d, j, DISPATCH_DEPTH);
    }
}

static void dispatch_event(runner_t *r, job_t *j, const char *line, void *ctx)
{
    dispatch_t *d = ctx;
    int idx = (int)(j - r->jobs);

    if (!line) {                       /* the worker just started */
        dispatch_feed(d, j, DISPATCH_DEPTH);
        return;
    }

    if (d->held_n[idx] == 0) return;   /* a marker we never asked for */

    /* Workers answer in the order they were given work. */
    int item = d->held[idx * DISPATCH_QUEUE];
    memmove(&d->held[idx * DISPATCH_QUEUE], &d->held[idx * DISPATCH_QUEUE + 1],
            sizeof(int) * (size_t)(d->held_n[idx] - 1));
    d->held_n[idx]--;

    d->attempts[item]++;
    d->last_node[item] = node_index(d, j->node);
    d->per_job[idx]++;

    if (atoi(line) == 0) {
        r->items_done++;
    } else if (d->attempts[item] < d->max_attempts) {
        queue_retry(d, item);
        r->items_retried++;
    } else {
        r->items_done++;
        r->items_failed++;
    }

    dispatch_feed(d, j, 1);
    if (d->retry_n > 0) dispatch_kick(d);
    show_progress(d, 0);
}

/* A worker that died was holding work nobody has heard about. It goes back in
 * the queue, so the machine failing takes nothing down with it. */
static void dispatch_job_end(runner_t *r, job_t *j, void *ctx)
{
    dispatch_t *d = ctx;
    int idx = (int)(j - r->jobs);

    for (int k = 0; k < d->held_n[idx]; k++) {
        int item = d->held[idx * DISPATCH_QUEUE + k];
        d->last_node[item] = node_index(d, j->node);
        queue_retry(d, item);
    }
    d->held_n[idx] = 0;
    dispatch_kick(d);

    if (j->state == JOB_OK) { maybe_finish(d); return; }

    /* The other workers pick the items up; if this was the last one standing,
     * they are counted as failed rather than lost. */
    int alive = 0;
    for (int i = 0; i < r->njobs; i++)
        if (i != idx && r->jobs[i].state == JOB_RUNNING) alive++;

    if (alive == 0 && j->attempt >= r->retries) {
        while (d->retry_n > 0) {
            take_retry(d, j->node);
            r->items_done++;
            r->items_failed++;
        }
        maybe_finish(d);
    }
}

static dispatch_t *dispatch_attach(runner_t *r, const char *tmpl, char **lines,
                                   int nitems, int start, node_t **nodes,
                                   int nnodes, int max_attempts)
{
    dispatch_t *d = xmalloc(sizeof *d);
    memset(d, 0, sizeof *d);
    d->tmpl = tmpl;
    d->lines = lines;
    d->nitems = nitems;
    d->start = start;
    d->njobs = r->njobs;
    d->nodes = nodes;
    d->nnodes = nnodes;
    d->max_attempts = max_attempts > 0 ? max_attempts : 1;
    d->runner = r;
    d->began = now_seconds();

    size_t items = (size_t)(nitems > 0 ? nitems : 1);
    d->attempts  = xmalloc(sizeof(int) * items);
    d->last_node = xmalloc(sizeof(int) * items);
    d->retry_q   = xmalloc(sizeof(int) * items);
    memset(d->attempts, 0, sizeof(int) * items);
    for (size_t i = 0; i < items; i++) d->last_node[i] = -1;

    size_t jobs = (size_t)(r->njobs > 0 ? r->njobs : 1);
    d->per_job = xmalloc(sizeof(int) * jobs);
    d->held_n  = xmalloc(sizeof(int) * jobs);
    d->held    = xmalloc(sizeof(int) * jobs * DISPATCH_QUEUE);
    memset(d->per_job, 0, sizeof(int) * jobs);
    memset(d->held_n, 0, sizeof(int) * jobs);

    r->marker = DISPATCH_MARKER;
    r->on_event = dispatch_event;
    r->after_job = dispatch_job_end;
    r->event_ctx = d;
    r->items_total = nitems;

    /* The job's command becomes the read-and-run loop, but what a reader
     * wants to see is the work it is doing. */
    for (int i = 0; i < r->njobs; i++) {
        free(r->jobs[i].cmd);
        r->jobs[i].cmd = xstrdup(WORKER_LOOP);
        free(r->jobs[i].show);
        r->jobs[i].show = xstrdup(tmpl ? tmpl : "one line at a time");
    }
    return d;
}

/* What each machine ended up doing — the point of dispatching live is that
 * this is not the same as what was planned. */
static void dispatch_report(dispatch_t *d, runner_t *r, node_t **nodes, int n)
{
    for (int i = 0; i < n; i++) {
        int workers = 0, items = 0;
        for (int k = 0; k < r->njobs; k++) {
            if (r->jobs[k].node != nodes[i]) continue;
            workers++;
            items += d->per_job[k];
        }
        if (!workers) continue;
        printf("  %s%-16s%s %d worker%s \xc2\xb7 %d item%s\n",
               c_dim(), nodes[i]->name, c_reset(),
               workers, workers == 1 ? " " : "s", items, items == 1 ? "" : "s");
    }
    if (r->items_retried)
        printf("  %s%d item%s tried again%s\n", c_yellow(), r->items_retried,
               r->items_retried == 1 ? "" : "s", c_reset());
    if (r->items_failed)
        printf("  %s%d of %d items failed%s\n",
               c_red(), r->items_failed, d->nitems, c_reset());
}

/* --dry-run on dispatched work: the interesting part is the items, not the
 * loop each worker runs. */
static void show_dispatch_plan(dispatch_t *d, int total)
{
    for (int k = 0; k < d->runner->njobs; k++)
        printf("  %s%s%s\n", c_dim(), d->runner->jobs[k].label, c_reset());

    printf("\n");
    for (int k = 0; k < total; k++) {
        if (k >= 3 && k < total - 1) {
            if (k == 3) printf("  %s… %d more%s\n", c_dim(), total - 4, c_reset());
            continue;
        }
        char *cmd = item_command(d, k);
        printf("  %s\n", cmd);
        free(cmd);
    }
}

static void dispatch_free(dispatch_t *d)
{
    free(d->attempts);
    free(d->last_node);
    free(d->retry_q);
    free(d->per_job);
    free(d->held_n);
    free(d->held);
    free(d);
}

/* ------------------------------------------------------------- broadcast */

int strategy_broadcast(cluster_t *c, const char *cmd, const run_opts *o)
{
    node_t **nodes;
    int n = cluster_select(c, o->on, &nodes);
    if (n == 0) {
        warn_msg("no nodes matched \"%s\"", o->on);
        free(nodes);
        return 1;
    }

    runner_t r;
    runner_init(&r);
    r.task_cmd = o->label ? o->label : cmd;
    r.task_selector = o->on;
    r.task_strategy = "broadcast";
    for (int k = 0; k < n; k++) runner_add(&r, nodes[k], nodes[k]->name, cmd);

    int rc = finish(c, &r, o, NULL, NULL);
    runner_free(&r);
    free(nodes);
    return rc;
}

/* ---------------------------------------------------------------- script */

/* Blank lines and whole-line comments carry no work, so they are dropped
 * before the file is shared out. */
static char **split_work_lines(const char *text, int *count)
{
    char **lines = NULL;
    int n = 0;
    const char *p = text;

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char *line = xstrndup(p, len);
        char *t = str_trim(line);
        if (*t && *t != '#') {
            lines = xrealloc(lines, sizeof(char *) * (size_t)(n + 1));
            lines[n++] = xstrdup(t);
        }
        free(line);
        if (!nl) break;
        p = nl + 1;
    }
    *count = n;
    return lines;
}

/* Shares fixed before the run starts: --static, kept because it needs no
 * coordination and leaves a readable chunk file per machine. */
static int distribute_static(cluster_t *c, const char *path, node_t **nodes,
                             int n, const run_opts *o, char **lines, int nlines)
{

    /* Shares are proportional to each node's cpu count, so an 8-core machine
     * takes four times what a 2-core one does. */
    int total_weight = 0;
    for (int i = 0; i < n; i++) total_weight += node_weight(nodes[i]);

    int *share = xmalloc(sizeof(int) * (size_t)n);
    int handed = 0;
    for (int i = 0; i < n; i++) {
        share[i] = nlines * node_weight(nodes[i]) / total_weight;
        if (share[i] < 1) share[i] = 1;
        handed += share[i];
    }
    for (int i = 0; handed > nlines && i < n; i++) {
        while (share[i] > 1 && handed > nlines) { share[i]--; handed--; }
    }
    for (int i = 0; handed < nlines; i = (i + 1) % n) { share[i]++; handed++; }

    char *task_id = NULL;
    char *task_dir = task_dir_create(c, &task_id);
    if (!task_dir) { free(share); return 1; }

    printf("%s: %d command%s across %d node%s\n", path, nlines,
           nlines == 1 ? "" : "s", n, n == 1 ? "" : "s");

    runner_t r;
    runner_init(&r);
    r.task_cmd = o->label ? o->label : path;
    r.task_selector = o->on;
    r.task_strategy = "distribute";

    int at = 0;
    for (int i = 0; i < n; i++) {
        char *chunk_path = xasprintf("%s/%s.chunk.sh", task_dir, nodes[i]->name);
        FILE *f = fopen(chunk_path, "w");
        if (!f) { warn_msg("cannot write %s", chunk_path); free(chunk_path); continue; }

        fprintf(f, "set -e\n");
        for (int k = 0; k < share[i] && at < nlines; k++, at++)
            fprintf(f, "%s\n", lines[at]);
        fclose(f);

        printf("  %s%-16s%s lines %d-%d (%d)\n", c_dim(), nodes[i]->name,
               c_reset(), at - share[i] + 1, at, share[i]);

        int idx = runner_add(&r, nodes[i], nodes[i]->name, "sh -s");
        r.jobs[idx].stdin_path = chunk_path;
    }
    printf("\n");

    int rc = finish(c, &r, o, task_dir, task_id);

    runner_free(&r);
    free(share);
    return rc;
}

/* Lines handed out one at a time, so a machine that finishes early asks for
 * more instead of waiting for the others. One worker per machine keeps the
 * load on each of them the same as it was. */
static int distribute_dynamic(cluster_t *c, const char *path, node_t **nodes,
                              int n, const run_opts *o, char **lines, int nlines)
{
    runner_t r;
    runner_init(&r);
    r.task_cmd = o->label ? o->label : path;
    r.task_selector = o->on;
    r.task_strategy = "distribute";

    for (int i = 0; i < n; i++) runner_add(&r, nodes[i], nodes[i]->name, "");

    dispatch_t *d = dispatch_attach(&r, NULL, lines, nlines, 0,
                                    nodes, n, o->retry + 1);
    for (int i = 0; i < r.njobs; i++) {
        free(r.jobs[i].show);
        r.jobs[i].show = xstrdup(path);
    }

    printf("%s: %d command%s across %d node%s, handed out as they finish\n",
           path, nlines, nlines == 1 ? "" : "s", n, n == 1 ? "" : "s");

    if (o->dry_run) {
        show_dispatch_plan(d, nlines);
        dispatch_free(d);
        runner_free(&r);
        return 0;
    }

    char *task_id = NULL, *task_dir = NULL;
    if (!o->no_log) task_dir = task_dir_create(c, &task_id);
    r.task_dir = task_dir;
    r.task_id = task_id;
    r.max_parallel = o->jobs;
    r.timeout = o->timeout;
    r.stream = o->stream;
    r.retries = o->retry;
    r.ship = o->ship;
    r.nship = o->nship;
    r.keep_stage = o->keep_stage;

    int failed = runner_run(&r);
    printf("\n");
    dispatch_report(d, &r, nodes, n);
    shard_summary(&r, failed);

    dispatch_free(d);
    runner_free(&r);
    free(task_dir);
    free(task_id);
    return failed || r.items_failed ? 1 : 0;
}

static int distribute_script(cluster_t *c, const char *path, node_t **nodes,
                             int n, const run_opts *o)
{
    size_t len;
    char *text = read_file(path, &len);
    if (!text) { warn_msg("cannot read %s", path); return 1; }

    int nlines;
    char **lines = split_work_lines(text, &nlines);
    free(text);

    if (nlines == 0) {
        warn_msg("%s has no commands in it", path);
        return 1;
    }
    if (n > nlines) n = nlines;      /* no point holding idle nodes */

    int rc = o->static_split
        ? distribute_static(c, path, nodes, n, o, lines, nlines)
        : distribute_dynamic(c, path, nodes, n, o, lines, nlines);

    for (int i = 0; i < nlines; i++) free(lines[i]);
    free(lines);
    return rc;
}

int strategy_script(cluster_t *c, const char *path, int distribute,
                    const run_opts *o)
{
    if (!file_exists(path)) { warn_msg("no such file: %s", path); return 1; }

    node_t **nodes;
    int n = cluster_select(c, o->on, &nodes);
    if (n == 0) {
        warn_msg("no nodes matched \"%s\"", o->on);
        free(nodes);
        return 1;
    }

    node_t **live = NULL;
    int *weights = NULL;
    if (o->balance) {
        int m;
        live = measure_online(c, nodes, n, &weights, &m, o->dry_run);
        free(weights);
        if (m == 0) {
            warn_msg("no machines answered, nothing to run on");
            free(live); free(nodes);
            return 1;
        }
        free(nodes);
        nodes = live;
        n = m;
    }

    int rc;
    if (distribute) {
        rc = distribute_script(c, path, nodes, n, o);
    } else {
        /* Broadcast: the same script is piped to every node's shell. */
        runner_t r;
        runner_init(&r);
        r.task_cmd = o->label ? o->label : path;
        r.task_selector = o->on;
        r.task_strategy = "script";
        for (int k = 0; k < n; k++) {
            int idx = runner_add(&r, nodes[k], nodes[k]->name, "sh -s");
            r.jobs[idx].stdin_path = xstrdup(path);
        }
        rc = finish(c, &r, o, NULL, NULL);
        runner_free(&r);
    }

    free(nodes);
    return rc;
}

/* ------------------------------------------------------------------- map */

/* One worker is one connection running its own slice of the indices, and a
 * machine gets as many workers as it has cores. A thousand items therefore
 * cost a dozen SSH connections rather than a thousand. */
typedef struct {
    node_t *node;
    int     slot;
    int     first, last;
} worker_t;

/* weights[i] is how many workers node i should get; NULL means use its
 * configured cpu count. */
static worker_t *plan_workers(node_t **nodes, int nnodes, int *weights,
                              int count, int *nout)
{
    int total = 0, max_slots = 0;
    for (int i = 0; i < nnodes; i++) {
        int wgt = weights ? weights[i] : node_weight(nodes[i]);
        total += wgt;
        if (wgt > max_slots) max_slots = wgt;
    }
    if (total > count) total = count;

    worker_t *w = xmalloc(sizeof(worker_t) * (size_t)total);
    int n = 0;

    /* Slot by slot rather than node by node, so cutting the list short still
     * leaves every machine with work. */
    for (int slot = 0; slot < max_slots && n < total; slot++)
        for (int i = 0; i < nnodes && n < total; i++) {
            int wgt = weights ? weights[i] : node_weight(nodes[i]);
            if (slot < wgt) {
                w[n].node = nodes[i];
                w[n].slot = slot + 1;
                n++;
            }
        }

    *nout = n;
    return w;
}

/* Probes the machines and keeps the ones that answer, sizing each to the cores
 * it has free right now rather than the cores it has in total. A machine at
 * full load takes one worker; an idle eight-core machine takes eight; a
 * machine that does not answer is dropped from the run. The caller frees the
 * returned array and *weights. */
static node_t **measure_online(cluster_t *c, node_t **nodes, int n,
                               int **weights_out, int *nlive, int quiet)
{
    if (!quiet) { printf("measuring load…\n"); fflush(stdout); }

    health_t *h = xmalloc(sizeof(health_t) * (size_t)n);
    int online = health_check(c, nodes, n, h);

    node_t **live = xmalloc(sizeof(node_t *) * (size_t)(n ? n : 1));
    int *weights = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
    int m = 0;

    for (int i = 0; i < n; i++) {
        if (!h[i].online) {
            if (!quiet)
                printf("  %s\xe2\x97\x8b %s skipped (%s)%s\n", c_dim(),
                       nodes[i]->name, h[i].error[0] ? h[i].error : "offline",
                       c_reset());
            continue;
        }
        int cores = h[i].ncpu > 0 ? h[i].ncpu : node_weight(nodes[i]);
        /* Free cores = total minus what load is already using. */
        double busy = cores > 0 ? h[i].load1 / cores : 1.0;
        if (busy > 1.0) busy = 1.0;
        if (busy < 0.0) busy = 0.0;
        int wgt = (int)(cores * (1.0 - busy) + 0.5);
        if (wgt < 1) wgt = 1;

        live[m] = nodes[i];
        weights[m] = wgt;
        if (!quiet)
            printf("  %s\xe2\x97\x8f %s: %d of %d cores free \xe2\x86\x92 %d worker%s%s\n",
                   c_dim(), nodes[i]->name, wgt, cores, wgt, wgt == 1 ? "" : "s",
                   c_reset());
        m++;
    }
    (void)online;
    free(h);

    *weights_out = weights;
    *nlive = m;
    return live;
}

/* The original scheme: every worker is told its share before anything runs.
 * Kept behind --static because it needs no coordination at all, which makes
 * a run reproducible and its chunk files readable afterwards. */
static int map_static(cluster_t *c, const char *tmpl, const run_opts *o,
                      node_t **nodes, int n, worker_t *w, int nworkers)
{

    /* Every worker takes the same number of items; the remainder goes to the
     * first few, which are the ones on the roomiest machines. */
    int base = o->count / nworkers, extra = o->count % nworkers;
    int at = o->start;
    for (int k = 0; k < nworkers; k++) {
        int share = base + (k < extra ? 1 : 0);
        w[k].first = at;
        w[k].last = at + share - 1;
        at += share;
    }

    char *task_id = NULL;
    char *task_dir = task_dir_create(c, &task_id);
    if (!task_dir) return 1;

    printf("%d items across %d node%s, %d worker%s\n", o->count,
           n, n == 1 ? "" : "s", nworkers, nworkers == 1 ? "" : "s");

    runner_t r;
    runner_init(&r);
    r.task_cmd = o->label ? o->label : tmpl;
    r.task_selector = o->on;
    r.task_strategy = "map";

    for (int k = 0; k < nworkers; k++) {
        char *label = xasprintf("%s#%d", w[k].node->name, w[k].slot);
        char *path = xasprintf("%s/%s.items.sh", task_dir, label);
        for (char *p = path; *p; p++) if (*p == '#') *p = '-';

        FILE *f = fopen(path, "w");
        if (!f) { warn_msg("cannot write %s", path); free(path); free(label); continue; }

        /* A failing item does not stop the others: the point of a map is to
         * get through all of them and be told how many did not work. */
        int items = w[k].last - w[k].first + 1;
        fprintf(f, "_shard_failed=0\n");
        for (int idx = w[k].first; idx <= w[k].last; idx++) {
            char *num = xasprintf("%d", idx);
            char *one = str_replace(tmpl, "{i}", num);
            char *cmd = str_replace(one, "{index}", num);
            fprintf(f, "%s || _shard_failed=$((_shard_failed+1))\n", cmd);
            free(cmd); free(one); free(num);
        }
        fprintf(f, "[ \"$_shard_failed\" -eq 0 ] || "
                   "{ echo \"shard: $_shard_failed of %d items failed\" >&2; exit 1; }\n",
                items);
        fclose(f);

        int j = runner_add(&r, w[k].node, label, "sh -s");
        r.jobs[j].stdin_path = path;
        free(label);
    }

    /* Workers are handed out slot by slot, so a machine's items are not one
     * contiguous range. What matters per machine is how much it took. */
    for (int i = 0; i < n; i++) {
        int mine = 0, items = 0;
        for (int k = 0; k < nworkers; k++) {
            if (w[k].node != nodes[i]) continue;
            mine++;
            items += w[k].last - w[k].first + 1;
        }
        if (!mine) continue;
        printf("  %s%-16s%s %d worker%s \xc2\xb7 %d item%s\n",
               c_dim(), nodes[i]->name, c_reset(),
               mine, mine == 1 ? " " : "s", items, items == 1 ? "" : "s");
    }
    printf("\n");

    int rc = finish(c, &r, o, task_dir, task_id);
    runner_free(&r);
    return rc;
}

/* Work handed out as machines ask for it. Nothing is decided in advance, so
 * one slow item holds up nothing but itself. */
static int map_dynamic(cluster_t *c, const char *tmpl, const run_opts *o,
                       node_t **nodes, int n, worker_t *w, int nworkers)
{
    runner_t r;
    runner_init(&r);
    r.task_cmd = o->label ? o->label : tmpl;
    r.task_selector = o->on;
    r.task_strategy = "map";

    for (int k = 0; k < nworkers; k++) {
        char *label = xasprintf("%s#%d", w[k].node->name, w[k].slot);
        runner_add(&r, w[k].node, label, "");
        free(label);
    }

    dispatch_t *d = dispatch_attach(&r, tmpl, NULL, o->count, o->start,
                                    nodes, n, o->retry + 1);

    printf("%d items across %d node%s, %d worker%s, handed out as they finish\n",
           o->count, n, n == 1 ? "" : "s", nworkers, nworkers == 1 ? "" : "s");

    if (o->dry_run) {
        show_dispatch_plan(d, o->count);
        dispatch_free(d);
        runner_free(&r);
        return 0;
    }

    char *task_id = NULL, *task_dir = NULL;
    if (!o->no_log) task_dir = task_dir_create(c, &task_id);
    r.task_dir = task_dir;
    r.task_id = task_id;
    r.max_parallel = o->jobs;
    r.timeout = o->timeout;
    r.stream = o->stream;
    r.retries = o->retry;
    r.ship = o->ship;
    r.nship = o->nship;
    r.keep_stage = o->keep_stage;

    int failed = runner_run(&r);
    printf("\n");
    dispatch_report(d, &r, nodes, n);
    shard_summary(&r, failed);

    dispatch_free(d);
    runner_free(&r);
    free(task_dir);
    free(task_id);
    return failed || r.items_failed ? 1 : 0;
}

int strategy_map(cluster_t *c, const char *tmpl, const run_opts *o)
{
    if (o->count <= 0) {
        warn_msg("--count must say how many times to run the command");
        return 1;
    }
    if (!strstr(tmpl, "{i}") && !strstr(tmpl, "{index}"))
        warn_msg("the command has no {i} in it, so every run will be identical");

    node_t **nodes;
    int n = cluster_select(c, o->on, &nodes);
    if (n == 0) {
        warn_msg("no nodes matched \"%s\"", o->on);
        free(nodes);
        return 1;
    }

    node_t **use = nodes;
    int un = n;
    int *weights = NULL;
    node_t **live = NULL;

    if (o->balance) {
        live = measure_online(c, nodes, n, &weights, &un, o->dry_run);
        if (un == 0) {
            warn_msg("no machines answered, nothing to run on");
            free(live); free(weights); free(nodes);
            return 1;
        }
        use = live;
    }

    int nworkers;
    worker_t *w = plan_workers(use, un, weights, o->count, &nworkers);

    int rc = o->static_split
        ? map_static(c, tmpl, o, use, un, w, nworkers)
        : map_dynamic(c, tmpl, o, use, un, w, nworkers);

    free(w);
    free(weights);
    free(live);
    free(nodes);
    return rc;
}

/* --------------------------------------------------- carrying files along */

/* A step can name files it leaves behind. They are pulled back to this
 * machine as a tar when the step succeeds, and unpacked on the machine a
 * later step runs on, before it starts. Two hops rather than one, but it
 * needs nothing installed anywhere and works between any two machines that
 * cannot reach each other directly — which, with a laptop at home and a VM in
 * a data centre, is the normal case. */
typedef struct {
    cluster_t *c;
    step_t    *steps;
    int       *step_of_job;
    char      *stage_dir;
    char      *own_dir;         /* set when we had to make one ourselves */

    char     **tars;            /* one per staged step */
    node_t   **from;
    char     **what;
    int        nstages;

    int       *pushed_stage;    /* stage/node pairs already delivered */
    node_t   **pushed_node;
    int        npushed;

    int        failed;
} carrier_t;

/* Runs one child to completion with its input and output pointed at files.
 * Errors stay on stderr, where they belong. */
static int run_child(char **argv, const char *in_path, const char *out_path)
{
    pid_t pid = fork();
    if (pid < 0) { warn_msg("fork: %s", strerror(errno)); return -1; }

    if (pid == 0) {
        int in = in_path ? open(in_path, O_RDONLY) : open("/dev/null", O_RDONLY);
        if (in >= 0) { dup2(in, STDIN_FILENO); close(in); }
        if (out_path) {
            int out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (out >= 0) { dup2(out, STDOUT_FILENO); close(out); }
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static const char *carrier_dir(carrier_t *k, runner_t *r)
{
    if (k->stage_dir) return k->stage_dir;

    if (r->task_dir) {
        k->stage_dir = xstrdup(r->task_dir);
    } else {
        const char *tmp = getenv("TMPDIR");
        k->own_dir = xasprintf("%s/shard-%d", tmp && *tmp ? tmp : "/tmp",
                               (int)getpid());
        mkdir_p(k->own_dir);
        k->stage_dir = xstrdup(k->own_dir);
    }
    return k->stage_dir;
}

static void human_size(long bytes, char *out, size_t n)
{
    if (bytes < 1024)              snprintf(out, n, "%ld B", bytes);
    else if (bytes < 1024 * 1024)  snprintf(out, n, "%.1f KB", bytes / 1024.0);
    else                           snprintf(out, n, "%.1f MB", bytes / (1024.0 * 1024.0));
}

static char *quoted_list(char **paths, int n)
{
    strbuf b;
    sb_init(&b);
    for (int i = 0; i < n; i++) {
        sb_puts(&b, i ? " '" : "'");
        sb_puts(&b, paths[i]);
        sb_puts(&b, "'");
    }
    return b.data;
}

/* The same paths, written the way a person would read them. */
static char *plain_list(char **paths, int n)
{
    strbuf b;
    sb_init(&b);
    for (int i = 0; i < n; i++) {
        sb_puts(&b, i ? ", " : "");
        sb_puts(&b, paths[i]);
    }
    return b.data;
}

/* After a step: collect what it produced. */
static void carrier_after(runner_t *r, job_t *j, void *ctx)
{
    carrier_t *k = ctx;
    int idx = (int)(j - r->jobs);
    step_t *step = &k->steps[k->step_of_job[idx]];

    if (j->state != JOB_OK || step->nproduces == 0) return;

    char *list = quoted_list(step->produces, step->nproduces);
    char *shown = plain_list(step->produces, step->nproduces);
    char *cmd = xasprintf("tar cf - -- %s", list);
    char *tar = xasprintf("%s/%s.artifacts.tar", carrier_dir(k, r), step->name);

    char **argv = node_argv(j->node, cmd);
    int rc = run_child(argv, NULL, tar);
    argv_free(argv);

    if (rc != 0) {
        warn_msg("step \"%s\": could not collect %s from %s",
                 step->name, shown, j->node->name);
        k->failed = 1;
        /* The steps after this one were going to use that file. */
        r->abort_run = 1;
        free(list); free(shown); free(cmd); free(tar);
        return;
    }

    struct stat st;
    char size[32] = "";
    if (stat(tar, &st) == 0) human_size((long)st.st_size, size, sizeof size);

    k->tars = xrealloc(k->tars, sizeof(char *) * (size_t)(k->nstages + 1));
    k->from = xrealloc(k->from, sizeof(node_t *) * (size_t)(k->nstages + 1));
    k->what = xrealloc(k->what, sizeof(char *) * (size_t)(k->nstages + 1));
    k->tars[k->nstages] = tar;
    k->from[k->nstages] = j->node;
    k->what[k->nstages] = shown;
    k->nstages++;
    free(list);

    if (!r->quiet)
        printf("  %s\xe2\x86\x91 %s from %s (%s)%s\n", c_dim(), shown,
               j->node->name, size, c_reset());
    free(cmd);
}

static int already_pushed(carrier_t *k, int stage, node_t *n)
{
    for (int i = 0; i < k->npushed; i++)
        if (k->pushed_stage[i] == stage && k->pushed_node[i] == n) return 1;
    return 0;
}

/* Before a step: make sure everything produced so far is on its machine. */
static void carrier_before(runner_t *r, job_t *j, void *ctx)
{
    carrier_t *k = ctx;

    for (int i = 0; i < k->nstages; i++) {
        if (k->from[i] == j->node) continue;          /* already there */
        if (already_pushed(k, i, j->node)) continue;

        char **argv = node_argv(j->node, "tar xf -");
        int rc = run_child(argv, k->tars[i], NULL);
        argv_free(argv);

        if (rc != 0) {
            warn_msg("could not put %s onto %s", k->what[i], j->node->name);
            k->failed = 1;
            r->abort_run = 1;
            continue;
        }

        k->pushed_stage = xrealloc(k->pushed_stage,
                                   sizeof(int) * (size_t)(k->npushed + 1));
        k->pushed_node = xrealloc(k->pushed_node,
                                  sizeof(node_t *) * (size_t)(k->npushed + 1));
        k->pushed_stage[k->npushed] = i;
        k->pushed_node[k->npushed] = j->node;
        k->npushed++;

        if (!r->quiet)
            printf("  %s\xe2\x86\x93 %s onto %s%s\n", c_dim(), k->what[i],
                   j->node->name, c_reset());
    }
}

static void carrier_free(carrier_t *k)
{
    for (int i = 0; i < k->nstages; i++) {
        free(k->tars[i]);
        free(k->what[i]);
    }
    free(k->tars);
    free(k->from);
    free(k->what);
    free(k->pushed_stage);
    free(k->pushed_node);
    free(k->step_of_job);
    free(k->stage_dir);
    free(k->own_dir);
    free(k);
}

/* ----------------------------------------------------------------- steps */

/* A step names a group of machines but runs on one of them. Picking the
 * matching machine that has taken the fewest steps so far spreads a list of
 * steps over the group, instead of piling them on whichever machine happens
 * to come first in the file. */
static node_t *pick_node(cluster_t *c, const char *selector, int *used,
                         const char *step_name)
{
    node_t **match;
    int n = cluster_select(c, selector, &match);
    if (n == 0) {
        warn_msg("step \"%s\": no nodes matched \"%s\"", step_name, selector);
        free(match);
        return NULL;
    }

    node_t *chosen = match[0];
    int best = used[match[0] - c->nodes];
    for (int i = 1; i < n; i++) {
        int u = used[match[i] - c->nodes];
        if (u < best) { best = u; chosen = match[i]; }
    }
    used[chosen - c->nodes]++;

    free(match);
    return chosen;
}

int strategy_steps(cluster_t *c, step_t *steps, int nsteps, int pipeline,
                   const run_opts *o)
{
    if (nsteps == 0) { warn_msg("this task has no steps"); return 1; }

    runner_t r;
    runner_init(&r);
    r.task_cmd = o->label ? o->label : "steps";
    r.task_selector = o->on;
    r.task_strategy = pipeline ? "pipeline" : "steps";

    int unmatched = 0, produces = 0;
    int *used = xmalloc(sizeof(int) * (size_t)(c->nnodes ? c->nnodes : 1));
    int *step_of_job = xmalloc(sizeof(int) * (size_t)nsteps);
    memset(used, 0, sizeof(int) * (size_t)(c->nnodes ? c->nnodes : 1));

    for (int i = 0; i < nsteps; i++) {
        const char *sel = steps[i].on ? steps[i].on : o->on;
        node_t *n = pick_node(c, sel, used, steps[i].name);
        if (!n) { unmatched++; continue; }

        char *label = xasprintf("%s\xc2\xb7%s", n->name, steps[i].name);
        int idx = runner_add(&r, n, label, steps[i].cmd);
        step_of_job[idx] = i;
        free(label);
        produces += steps[i].nproduces;
    }
    free(used);

    if (r.njobs == 0) { free(step_of_job); runner_free(&r); return 1; }

    printf("%s: %d step%s%s\n", pipeline ? "pipeline" : "steps",
           r.njobs, r.njobs == 1 ? "" : "s",
           pipeline ? ", in order" : ", at the same time");

    /* Files only travel between steps that run one after another; steps
     * running at the same time cannot wait for each other's output. */
    carrier_t *k = NULL;
    if (produces && !pipeline) {
        warn_msg("`produces` needs steps that run in order; "
                 "these run at the same time, so nothing is carried");
    } else if (produces) {
        k = xmalloc(sizeof *k);
        memset(k, 0, sizeof *k);
        k->c = c;
        k->steps = steps;
        k->step_of_job = step_of_job;
        step_of_job = NULL;

        r.before_job = carrier_before;
        r.after_job = carrier_after;
        r.event_ctx = k;
    }
    free(step_of_job);

    run_opts local = *o;
    if (pipeline) {
        /* One at a time, and the first failure ends the run: a later step
         * usually depends on what an earlier one produced. */
        local.jobs = 1;
        r.stop_on_fail = 1;
    }

    int rc = finish(c, &r, &local, NULL, NULL);
    if (k) {
        if (k->failed) rc = 1;
        carrier_free(k);
    }
    runner_free(&r);
    return rc || unmatched ? 1 : 0;
}
