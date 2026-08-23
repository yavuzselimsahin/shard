/* Where runs are kept.
 *
 * One directory per task under the cluster's log_dir: a task.json with the
 * metadata, and one .log per job. Plain files, so `cat`, `grep` and `tail`
 * work on them, and the web UI can serve task.json straight through without
 * anything here having to parse JSON back.
 */

#include "shard.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double task_started_at = 0;
static char   task_started_str[32] = "";

char *log_root(cluster_t *c)
{
    char *root = xstrdup(c->log_dir);
    if (mkdir_p(root) != 0) {
        warn_msg("cannot create %s", root);
        free(root);
        return NULL;
    }
    return root;
}

char *task_dir_create(cluster_t *c, char **id_out)
{
    char *root = log_root(c);
    if (!root) return NULL;

    char *stamp = now_stamp();
    char *id = xasprintf("%s-%d", stamp, (int)getpid() % 10000);
    free(stamp);

    char *dir = xasprintf("%s/%s", root, id);
    free(root);

    if (mkdir_p(dir) != 0) {
        warn_msg("cannot create %s", dir);
        free(dir);
        free(id);
        return NULL;
    }

    task_started_at = now_seconds();
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(task_started_str, sizeof task_started_str, "%Y-%m-%d %H:%M:%S", &tm);

    if (id_out) *id_out = id;
    else free(id);
    return dir;
}

static const char *job_status(job_state s)
{
    switch (s) {
    case JOB_OK:      return "completed";
    case JOB_FAILED:  return "failed";
    case JOB_TIMEOUT: return "timeout";
    case JOB_RUNNING: return "running";
    case JOB_SKIPPED: return "skipped";
    default:          return "pending";
    }
}

void task_write_meta(runner_t *r, const char *status)
{
    if (!r->task_dir) return;

    strbuf b;
    sb_init(&b);
    double now = now_seconds();

    sb_puts(&b, "{\n  \"id\": \"");
    json_escape(&b, r->task_id ? r->task_id : "");
    sb_puts(&b, "\",\n  \"command\": \"");
    json_escape(&b, r->task_cmd ? r->task_cmd : "");
    sb_puts(&b, "\",\n  \"selector\": \"");
    json_escape(&b, r->task_selector ? r->task_selector : "");
    sb_puts(&b, "\",\n  \"strategy\": \"");
    json_escape(&b, r->task_strategy ? r->task_strategy : "broadcast");
    sb_puts(&b, "\",\n  \"status\": \"");
    json_escape(&b, status);
    sb_printf(&b, "\",\n  \"pid\": %d", (int)getpid());
    sb_printf(&b, ",\n  \"started\": %.3f", task_started_at);
    sb_puts(&b, ",\n  \"started_at\": \"");
    json_escape(&b, task_started_str);
    sb_printf(&b, "\",\n  \"duration\": %.3f", now - task_started_at);
    if (r->items_total > 0)
        sb_printf(&b, ",\n  \"items_total\": %d,\n  \"items_done\": %d,"
                      "\n  \"items_failed\": %d,\n  \"items_retried\": %d",
                  r->items_total, r->items_done, r->items_failed,
                  r->items_retried);
    sb_printf(&b, ",\n  \"jobs\": [");

    for (int i = 0; i < r->njobs; i++) {
        job_t *j = &r->jobs[i];
        double dur = j->state == JOB_RUNNING || j->state == JOB_PENDING
            ? (j->start > 0 ? now - j->start : 0)
            : j->end - j->start;

        sb_puts(&b, i ? ",\n    {" : "\n    {");
        sb_puts(&b, "\"label\": \"");
        json_escape(&b, j->label);
        sb_puts(&b, "\", \"node\": \"");
        json_escape(&b, j->node->name);
        sb_puts(&b, "\", \"host\": \"");
        json_escape(&b, j->node->host);
        sb_puts(&b, "\", \"command\": \"");
        json_escape(&b, j->show ? j->show : j->cmd);
        sb_puts(&b, "\", \"status\": \"");
        json_escape(&b, job_status(j->state));
        sb_printf(&b, "\", \"exit\": %d", j->exit_code);
        sb_printf(&b, ", \"duration\": %.3f", dur);
        sb_printf(&b, ", \"bytes\": %zu", j->bytes);
        sb_puts(&b, ", \"log\": \"");
        json_escape(&b, j->log_name ? j->log_name : "");
        sb_puts(&b, ".log\"}");
    }
    sb_puts(&b, "\n  ]\n}\n");

    /* Written through a temporary file: the UI polls this while it changes,
     * and should never see half of it. */
    char *tmp = xasprintf("%s/task.json.tmp", r->task_dir);
    char *final = xasprintf("%s/task.json", r->task_dir);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fwrite(b.data, 1, b.len, f);
        fclose(f);
        rename(tmp, final);
    }
    free(tmp);
    free(final);
    sb_free(&b);
}

static int cmp_desc(const void *a, const void *b)
{
    return strcmp(*(char *const *)b, *(char *const *)a);
}

int task_list(cluster_t *c, char ***ids_out, int limit)
{
    char *root = xstrdup(c->log_dir);
    DIR *d = opendir(root);
    free(root);
    if (!d) { *ids_out = NULL; return 0; }

    char **ids = NULL;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        ids = xrealloc(ids, sizeof(char *) * (size_t)(n + 1));
        ids[n++] = xstrdup(e->d_name);
    }
    closedir(d);

    qsort(ids, (size_t)n, sizeof(char *), cmp_desc);

    if (limit > 0 && n > limit) {
        for (int i = limit; i < n; i++) free(ids[i]);
        n = limit;
    }
    *ids_out = ids;
    return n;
}

char *task_meta_read(cluster_t *c, const char *id, size_t *len)
{
    char *path = xasprintf("%s/%s/task.json", c->log_dir, id);
    char *text = read_file(path, len);
    free(path);
    return text;
}
