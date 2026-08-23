/* `shard queue` — a batch queue for people who have more jobs than patience.
 *
 * shard runs a command and returns; there is no daemon. The queue is the
 * unattended counterpart: drop several jobs in it, say which matter more, and
 * `shard queue run` works through them highest-priority first, one at a time,
 * so the cluster is never oversubscribed. Each job is a small file under
 * ~/.shard/queue, so the queue survives the program exiting and can be looked
 * at with `ls` and `cat`.
 */

#include "shard.h"

#include <dirent.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char   *id;
    int     priority;
    double  added;
    char   *added_str;
    char   *name;
    char   *cwd;
    char  **argv;
    int     argc;
} qjob;

static char *queue_dir(void)
{
    char *home = home_dir();
    char *dir = xasprintf("%s/.shard/queue", home);
    free(home);
    mkdir_p(dir);
    return dir;
}

static void qjob_free(qjob *j)
{
    free(j->id);
    free(j->added_str);
    free(j->name);
    free(j->cwd);
    for (int i = 0; i < j->argc; i++) free(j->argv[i]);
    free(j->argv);
}

/* One job file: a few "key value" header lines, then one "argv <token>" line
 * per argument, so a command with spaces in it survives the round trip. */
static qjob *qjob_read(const char *dir, const char *id)
{
    char *path = xasprintf("%s/%s", dir, id);
    size_t len;
    char *text = read_file(path, &len);
    free(path);
    if (!text) return NULL;

    qjob *j = xmalloc(sizeof *j);
    memset(j, 0, sizeof *j);
    j->id = xstrdup(id);

    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (str_has_prefix(line, "priority "))   j->priority = atoi(line + 9);
        else if (str_has_prefix(line, "added ")) j->added = strtod(line + 6, NULL);
        else if (str_has_prefix(line, "when "))  j->added_str = xstrdup(line + 5);
        else if (str_has_prefix(line, "name "))  j->name = xstrdup(line + 5);
        else if (str_has_prefix(line, "cwd "))   j->cwd = xstrdup(line + 4);
        else if (str_has_prefix(line, "argv ")) {
            j->argv = xrealloc(j->argv, sizeof(char *) * (size_t)(j->argc + 1));
            j->argv[j->argc++] = xstrdup(line + 5);
        }
    }
    free(text);

    if (j->argc == 0) { qjob_free(j); free(j); return NULL; }
    return j;
}

/* Highest priority first; oldest first within a priority, so a queue of equal
 * jobs is plain FIFO. */
static int qjob_cmp(const void *a, const void *b)
{
    const qjob *x = *(qjob *const *)a, *y = *(qjob *const *)b;
    if (x->priority != y->priority) return y->priority - x->priority;
    if (x->added < y->added) return -1;
    if (x->added > y->added) return 1;
    return strcmp(x->id, y->id);
}

static qjob **queue_load(int *nout)
{
    char *dir = queue_dir();
    DIR *d = opendir(dir);
    qjob **jobs = NULL;
    int n = 0;

    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            size_t l = strlen(e->d_name);
            if (l < 5 || strcmp(e->d_name + l - 4, ".job") != 0) continue;
            qjob *j = qjob_read(dir, e->d_name);
            if (j) {
                jobs = xrealloc(jobs, sizeof(qjob *) * (size_t)(n + 1));
                jobs[n++] = j;
            }
        }
        closedir(d);
    }
    free(dir);

    qsort(jobs, (size_t)n, sizeof(qjob *), qjob_cmp);
    *nout = n;
    return jobs;
}

static void queue_free(qjob **jobs, int n)
{
    for (int i = 0; i < n; i++) { qjob_free(jobs[i]); free(jobs[i]); }
    free(jobs);
}

static char *summary(qjob *j)
{
    strbuf b;
    sb_init(&b);
    for (int i = 0; i < j->argc; i++) {
        if (i) sb_puts(&b, " ");
        sb_puts(&b, j->argv[i]);
    }
    return b.data;
}

/* ---------------------------------------------------------------- add */

static int do_add(int argc, char **argv)
{
    int priority = 0;
    const char *name = NULL;
    int sep = -1;

    for (int i = 0; i < argc; i++) {
        if (str_eq(argv[i], "--priority") && i + 1 < argc) priority = atoi(argv[++i]);
        else if (str_eq(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (str_eq(argv[i], "--")) { sep = i; break; }
    }

    if (sep < 0 || sep + 1 >= argc) {
        warn_msg("usage: shard queue add [--priority N] [--name \"...\"] -- <command>");
        return 1;
    }
    if (str_eq(argv[sep + 1], "queue")) {
        warn_msg("a queue cannot run the queue");
        return 1;
    }

    char *dir = queue_dir();
    char *stamp = now_stamp();
    char *id = xasprintf("%s-%d.job", stamp, (int)getpid() % 10000);
    char *path = xasprintf("%s/%s", dir, id);
    free(stamp);
    free(dir);

    FILE *f = fopen(path, "w");
    if (!f) { warn_msg("cannot write %s", path); free(path); free(id); return 1; }

    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char when[32];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);

    char *cwd = getcwd(NULL, 0);

    fprintf(f, "priority %d\n", priority);
    fprintf(f, "added %.3f\n", now_seconds());
    fprintf(f, "when %s\n", when);
    if (name) fprintf(f, "name %s\n", name);
    if (cwd)  fprintf(f, "cwd %s\n", cwd);
    for (int i = sep + 1; i < argc; i++) fprintf(f, "argv %s\n", argv[i]);
    fclose(f);
    free(cwd);

    printf("Queued %s%s%s (priority %d).\n", c_bold(), id, c_reset(), priority);
    printf("Run the queue with:  shard queue run\n");
    free(path);
    free(id);
    return 0;
}

/* --------------------------------------------------------------- list */

static int do_list(void)
{
    int n;
    qjob **jobs = queue_load(&n);

    if (n == 0) {
        printf("The queue is empty.\n");
        queue_free(jobs, n);
        return 0;
    }

    printf("%d job%s waiting, in the order they will run:\n\n", n, n == 1 ? "" : "s");
    printf("%-4s %-8s %-19s %s\n", "PRI", "", "ADDED", "COMMAND");
    for (int i = 0; i < n; i++) {
        char *cmd = summary(jobs[i]);
        char shortcmd[57];
        snprintf(shortcmd, sizeof shortcmd, "%s", cmd);
        if (strlen(cmd) > 56) memcpy(shortcmd + 53, "...", 4);
        printf("%s%-4d%s %-8s %-19s %s\n", c_bold(), jobs[i]->priority, c_reset(),
               jobs[i]->name ? jobs[i]->name : "",
               jobs[i]->added_str ? jobs[i]->added_str : "", shortcmd);
        free(cmd);
    }
    queue_free(jobs, n);
    return 0;
}

static int do_remove(const char *id)
{
    char *dir = queue_dir();
    /* Accept the id with or without the .job suffix. */
    char *path = str_has_prefix(id + (strlen(id) > 4 ? strlen(id) - 4 : 0), ".job")
        ? xasprintf("%s/%s", dir, id)
        : xasprintf("%s/%s.job", dir, id);
    free(dir);

    if (unlink(path) != 0) {
        warn_msg("no queued job called %s", id);
        free(path);
        return 1;
    }
    printf("Removed %s from the queue.\n", id);
    free(path);
    return 0;
}

static int do_clear(void)
{
    int n;
    qjob **jobs = queue_load(&n);
    char *dir = queue_dir();
    for (int i = 0; i < n; i++) {
        char *path = xasprintf("%s/%s", dir, jobs[i]->id);
        unlink(path);
        free(path);
    }
    free(dir);
    printf("Cleared %d job%s.\n", n, n == 1 ? "" : "s");
    queue_free(jobs, n);
    return 0;
}

/* ---------------------------------------------------------------- run */

static volatile sig_atomic_t stop_after_current = 0;
static void on_int(int sig) { (void)sig; stop_after_current = 1; }

static int do_run(cluster_t *c, int argc, char **argv)
{
    int keep_going = 1;                 /* keep running even if a job fails */
    for (int i = 0; i < argc; i++)
        if (str_eq(argv[i], "--stop-on-fail")) keep_going = 0;

    /* Ctrl-C stops the queue between jobs rather than in the middle of one. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_int;
    sigaction(SIGINT, &sa, NULL);

    char *start_cwd = getcwd(NULL, 0);
    int ran = 0, failed = 0;

    for (;;) {
        /* Re-read every time, so a higher-priority job added mid-run — from
         * another terminal — is picked up next, not after everything else. */
        int n;
        qjob **jobs = queue_load(&n);
        if (n == 0) { queue_free(jobs, n); break; }

        qjob *j = jobs[0];
        char *cmd = summary(j);
        printf("\n%s\xe2\x96\xb6 [priority %d] %s%s%s\n", c_bold(), j->priority,
               c_reset(), j->name ? j->name : cmd, c_reset());
        if (j->name) printf("%s  %s%s\n", c_dim(), cmd, c_reset());
        free(cmd);

        /* Run where the job was queued, so relative paths and --with files
         * resolve the way they did when it was added. */
        if (j->cwd && chdir(j->cwd) != 0)
            warn_msg("cannot enter %s, running in the current directory", j->cwd);

        int rc = shard_dispatch(c, j->argc, j->argv);
        ran++;
        if (rc != 0) failed++;

        if (start_cwd) { if (chdir(start_cwd) != 0) { /* best effort */ } }

        /* Remove only after it ran, so a crash leaves the job in the queue. */
        char *dir = queue_dir();
        char *path = xasprintf("%s/%s", dir, j->id);
        unlink(path);
        free(path);
        free(dir);

        queue_free(jobs, n);

        if (rc != 0 && !keep_going) {
            warn_msg("a job failed and --stop-on-fail is set, stopping");
            break;
        }
        if (stop_after_current) {
            printf("\n%sStopped after the current job.%s\n", c_dim(), c_reset());
            break;
        }
    }

    free(start_cwd);

    int left;
    qjob **rest = queue_load(&left);
    queue_free(rest, left);

    printf("\n%d job%s run", ran, ran == 1 ? "" : "s");
    if (failed) printf(", %d failed", failed);
    if (left)   printf(", %d still queued", left);
    printf(".\n");
    return failed ? 1 : 0;
}

/* -------------------------------------------------------------- entry */

int cmd_queue(cluster_t *c, int argc, char **argv)
{
    if (argc < 1) {
        printf("Usage:\n"
               "  shard queue add [--priority N] [--name \"...\"] -- <command>\n"
               "  shard queue list\n"
               "  shard queue run [--stop-on-fail]\n"
               "  shard queue remove <id>\n"
               "  shard queue clear\n");
        return 1;
    }

    const char *sub = argv[0];
    if (str_eq(sub, "add"))    return do_add(argc - 1, argv + 1);
    if (str_eq(sub, "list"))   return do_list();
    if (str_eq(sub, "run"))    return do_run(c, argc - 1, argv + 1);
    if (str_eq(sub, "clear"))  return do_clear();
    if (str_eq(sub, "remove") || str_eq(sub, "rm")) {
        if (argc < 2) { warn_msg("usage: shard queue remove <id>"); return 1; }
        return do_remove(argv[1]);
    }

    warn_msg("unknown subcommand: queue %s", sub);
    return 1;
}
