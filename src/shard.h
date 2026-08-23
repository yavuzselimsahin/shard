/* shard — minimal distributed task runner.
 *
 * Everything the translation units share lives here: the small string and
 * path helpers, the TOML reader, the cluster model, and the job runner.
 */
#ifndef SHARD_H
#define SHARD_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

#define SHARD_VERSION "0.1.0"
#define SHARD_MAX_TAGS 16

/* ------------------------------------------------------------------ util */

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...);

typedef struct {
    char  *data;
    size_t len, cap;
} strbuf;

void sb_init(strbuf *b);
void sb_add(strbuf *b, const char *s, size_t n);
void sb_puts(strbuf *b, const char *s);
void sb_printf(strbuf *b, const char *fmt, ...);
void sb_free(strbuf *b);

char *str_trim(char *s);
int   str_eq(const char *a, const char *b);
int   str_has_prefix(const char *s, const char *pfx);
/* Replaces every occurrence of `what` in `s`. Result is malloc'd. */
char *str_replace(const char *s, const char *what, const char *with);

char  *expand_path(const char *p);           /* "~/x" -> "$HOME/x", malloc'd */
int    mkdir_p(const char *path);
char  *read_file(const char *path, size_t *len_out);
int    file_exists(const char *path);
char  *home_dir(void);

double now_seconds(void);
char  *now_stamp(void);                      /* "20260822-205300", malloc'd */
void   fmt_duration(double secs, char *out, size_t n);
void   json_escape(strbuf *b, const char *s);
/* Just enough JSON reading to look one key up in a document shard wrote
 * itself: enough for `status` and the web UI, no parser needed. */
char  *json_peek_str(const char *text, const char *key);
double json_peek_num(const char *text, const char *key);

void die(const char *fmt, ...);
void warn_msg(const char *fmt, ...);

int  color_enabled(void);
const char *c_green(void);
const char *c_red(void);
const char *c_dim(void);
const char *c_bold(void);
const char *c_yellow(void);
const char *c_reset(void);

/* ------------------------------------------------------------------ toml */

typedef enum { TOML_STR, TOML_INT, TOML_BOOL, TOML_ARRAY } toml_kind;

typedef struct {
    char     *key;
    toml_kind kind;
    char     *sval;      /* TOML_STR */
    long      ival;      /* TOML_INT */
    int       bval;      /* TOML_BOOL */
    char    **items;     /* TOML_ARRAY */
    int       nitems;
} toml_kv;

typedef struct {
    char    *path;       /* "cluster", "nodes", "task.step" */
    int      is_array;   /* declared as [[table]] */
    toml_kv *kv;
    int      nkv;
} toml_table;

typedef struct {
    toml_table *tables;
    int         ntables;
    char        err[256];
} toml_doc;

toml_doc      *toml_parse_file(const char *path, char *errbuf, size_t errlen);
void           toml_free(toml_doc *d);
const toml_kv *toml_get(const toml_table *t, const char *key);
const char    *toml_str(const toml_table *t, const char *key, const char *def);
long           toml_int(const toml_table *t, const char *key, long def);

/* --------------------------------------------------------------- cluster */

typedef struct {
    char *name;
    char *host;
    char *user;
    char *key;          /* ssh identity file */
    char *workdir;      /* cd here before running */
    int   port;         /* ssh port, 0 = default */
    int   cpu;
    int   ram_gb;
    char *tags[SHARD_MAX_TAGS];
    int   ntags;
    int   is_local;     /* localhost: run without ssh */
} node_t;

typedef struct {
    char   *name;
    char   *log_dir;    /* expanded */
    char   *config_path;
    node_t *nodes;
    int     nnodes;
} cluster_t;

char      *cluster_find_config(void);        /* malloc'd path or NULL */
cluster_t *cluster_load(const char *path);   /* NULL -> search; dies on error */
void       cluster_free(cluster_t *c);
node_t    *cluster_node(cluster_t *c, const char *name);
/* Selector: "all", a node name, a tag, or a comma separated list of those.
 * Fills *out with a malloc'd array of node pointers; returns the count. */
int        cluster_select(cluster_t *c, const char *selector, node_t ***out);
int        cluster_init_file(const char *path);
int        cluster_remove_node(const char *path, const char *name);
int        cluster_append_node(const char *path, const node_t *n);
int        node_weight(const node_t *n);     /* cpu count, at least 1 */

/* ---------------------------------------------------------------- runner */

typedef enum {
    JOB_PENDING = 0,
    JOB_RUNNING,
    JOB_OK,
    JOB_FAILED,
    JOB_TIMEOUT,
    JOB_SKIPPED     /* an earlier job failed and the run stops in order */
} job_state;

typedef struct {
    node_t   *node;
    char     *label;        /* what to show: node name, or "chunk 2/4" */
    char     *cmd;          /* command line to run on the node */
    char     *show;         /* what to print instead of cmd, when it differs */
    char     *stdin_path;   /* optional file fed to the job's stdin */
    job_state state;
    pid_t     pid;
    int       fd;           /* read end of the job's output pipe */
    int       in_fd;        /* write end of its stdin, when work is fed live */
    FILE     *log;
    char     *log_path;
    char     *log_name;     /* basename, used by the web UI */
    int       exit_code;
    int       attempt;      /* 0 the first time, 1 on the first retry, … */
    double    start, end;
    size_t    bytes;
    strbuf    capture;      /* kept in memory only when capture_output is set */
} job_t;

struct runner;

typedef struct runner {
    job_t      *jobs;
    int         njobs;
    int         max_parallel;
    int         timeout;        /* seconds, 0 = no limit */
    int         stream;         /* prefix live output with [node] */
    int         quiet;
    int         capture_output; /* keep stdout in job->capture */
    int         stop_on_fail;   /* leave the rest unrun when one job fails */
    int         retries;        /* extra attempts a failed job may have */
    int         abort_run;      /* a hook sets this to stop starting new jobs */
    char      **ship;           /* files shipped to each remote worker first */
    int         nship;
    int         keep_stage;
    /* Live dispatch: when `marker` is set, jobs are given a writable stdin and
     * every output line starting with it is an event for `on_event` rather
     * than output. on_event is also called once, with line == NULL, as each
     * job starts. This is how a worker asks for its next piece of work. */
    const char *marker;
    void      (*on_event)(struct runner *r, job_t *j, const char *line, void *ctx);
    /* Called just before a job is started and just after it has ended —
     * where a pipeline moves files between machines, and where a dispatcher
     * takes back the work a lost job was holding. */
    void      (*before_job)(struct runner *r, job_t *j, void *ctx);
    void      (*after_job)(struct runner *r, job_t *j, void *ctx);
    void       *event_ctx;
    const char *task_id;
    const char *task_dir;
    const char *task_cmd;
    const char *task_selector;
    const char *task_strategy;
    /* Live dispatch keeps a real count of the work, which is what a progress
     * line and the dashboard want; 0 means the run has no item count. */
    int         items_total;
    int         items_done;
    int         items_failed;
    int         items_retried;
} runner_t;

void runner_init(runner_t *r);
int  runner_add(runner_t *r, node_t *n, const char *label, const char *cmd);
int  runner_run(runner_t *r);      /* returns the number of failed jobs */
void runner_free(runner_t *r);
/* Feeding a live worker: one line in, or end of input. */
void   job_send(job_t *j, const char *line);
void   job_end_input(job_t *j);
/* The ssh (or local shell) argv for one job. Caller frees with argv_free. */
char **job_argv(const job_t *j);
/* The same, for a command that is not a job: file transfers use it. */
char **node_argv(const node_t *n, const char *cmd);
void   argv_free(char **argv);

/* --------------------------------------------------------------- logging */

char *log_root(cluster_t *c);                       /* malloc'd, created */
char *task_dir_create(cluster_t *c, char **id_out); /* malloc'd */
void  task_write_meta(runner_t *r, const char *status);
/* Newest first. Returns malloc'd array of malloc'd task ids. */
int   task_list(cluster_t *c, char ***ids_out, int limit);
char *task_meta_read(cluster_t *c, const char *id, size_t *len);

/* ------------------------------------------------------------ strategies */

/* Everything the four strategies take from the command line or from
 * tasks.toml. One struct, so a task and a typed command reach the same code. */
typedef struct {
    const char *on;         /* node selector, "all" by default */
    int   jobs;             /* how many jobs at once, 0 = all */
    int   timeout;          /* seconds per job, 0 = no limit */
    int   stream;           /* echo output as it arrives */
    int   dry_run;          /* print what would run, run nothing */
    int   no_log;           /* do not create a run directory */
    int   count;            /* map: how many items */
    int   start;            /* map: first index */
    int   static_split;     /* share the work out up front, no live dispatch */
    int   retry;            /* extra attempts a failed job or item may have */
    char **ship;            /* files to send to each worker before it runs */
    int   nship;
    int   keep_stage;       /* leave the shipped copy behind afterwards */
    int   balance;          /* measure live load and place work accordingly */
    const char *label;      /* what to record as the command of the run */
} run_opts;

typedef struct {
    char  *name;
    char  *cmd;
    char  *on;              /* selector for this step, NULL = the task's */
    char **produces;        /* files this step leaves behind, carried onward */
    int    nproduces;
} step_t;

void run_opts_default(run_opts *o);
int  strategy_broadcast(cluster_t *c, const char *cmd, const run_opts *o);
int  strategy_script(cluster_t *c, const char *path, int distribute,
                     const run_opts *o);
int  strategy_map(cluster_t *c, const char *tmpl, const run_opts *o);
/* pipeline = 0 runs the steps at the same time, 1 runs them in order and
 * stops at the first failure. */
int  strategy_steps(cluster_t *c, step_t *steps, int nsteps, int pipeline,
                    const run_opts *o);

/* Shared by every strategy and by `shard run`. */
void shard_summary(runner_t *r, int failed);
void shard_show_plan(runner_t *r);

/* ------------------------------------------------------------------ tasks */

typedef struct {
    char   *name;
    char   *description;
    char   *strategy;       /* broadcast | map | distribute | steps | pipeline */
    char   *cmd;
    char   *script;
    char   *on;
    int     count;
    int     start;
    int     timeout;
    int     jobs;
    int     retry;
    char  **with;               /* files to ship with the job */
    int     nwith;
    step_t *steps;
    int     nsteps;
} task_t;

typedef struct {
    char   *path;
    task_t *tasks;
    int     ntasks;
} taskfile_t;

char       *tasks_find_file(void);
taskfile_t *tasks_load(const char *path);   /* NULL on error, message printed */
void        tasks_free(taskfile_t *tf);
task_t     *tasks_get(taskfile_t *tf, const char *name);

/* -------------------------------------------------------------- commands */

int cmd_cluster(cluster_t *c, int argc, char **argv);
int cmd_exec(cluster_t *c, int argc, char **argv);
int cmd_exec_script(cluster_t *c, int argc, char **argv);
int cmd_map(cluster_t *c, int argc, char **argv);
int cmd_run(cluster_t *c, int argc, char **argv);
int cmd_tasks(cluster_t *c, int argc, char **argv);
int cmd_pipeline(cluster_t *c, int argc, char **argv);
int cmd_status(cluster_t *c, int argc, char **argv);
int cmd_logs(cluster_t *c, int argc, char **argv);
int cmd_history(cluster_t *c, int argc, char **argv);
int cmd_ui(cluster_t *c, int argc, char **argv);
int cmd_tui(cluster_t *c, int argc, char **argv);
int cmd_queue(cluster_t *c, int argc, char **argv);
int shard_dispatch(cluster_t *c, int argc, char **argv);

/* Health probing is shared by `cluster health` and the web UI. */
typedef struct {
    char  name[64];
    int   online;
    int   ncpu;
    double load1;
    long  ram_mb, ram_used_mb;
    int   cpu_pct, ram_pct;
    double ms;
    char  error[160];
} health_t;

int   health_check(cluster_t *c, node_t **nodes, int n, health_t *out);
char *health_state_path(cluster_t *c);
void  health_write(cluster_t *c, health_t *h, int n);

extern const char *SHARD_PROBE_CMD;

#endif /* SHARD_H */
