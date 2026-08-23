/* `shard status`, `shard logs` and `shard history` — reading back what past
 * and current runs left in the log directory. */

#include "shard.h"

#include <dirent.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A task file says "running", but the process that wrote it may be gone —
 * a laptop that slept, a terminal that was closed. Ask the OS instead. */
static int task_alive(const char *meta)
{
    char *status = json_peek_str(meta, "status");
    int running = str_eq(status, "running");
    free(status);
    if (!running) return 0;

    int pid = (int)json_peek_num(meta, "pid");
    if (pid <= 0) return 0;
    return kill((pid_t)pid, 0) == 0;
}

static void print_task_line(const char *id, const char *meta)
{
    char *cmd    = json_peek_str(meta, "command");
    char *status = json_peek_str(meta, "status");
    char *when   = json_peek_str(meta, "started_at");
    double dur   = json_peek_num(meta, "duration");

    if (str_eq(status, "running") && !task_alive(meta)) {
        free(status);
        status = xstrdup("interrupted");
    }

    const char *color = c_dim();
    if (str_eq(status, "completed")) color = c_green();
    else if (str_eq(status, "failed")) color = c_red();
    else if (str_eq(status, "running")) color = c_yellow();

    char dur_s[32];
    fmt_duration(dur, dur_s, sizeof dur_s);

    char short_cmd[45];
    snprintf(short_cmd, sizeof short_cmd, "%s", cmd ? cmd : "");
    if (cmd && strlen(cmd) > 44) memcpy(short_cmd + 41, "...", 4);

    printf("%-22s %s%-11s%s %-44s %s%9s  %s%s\n",
           id, color, status ? status : "?", c_reset(), short_cmd,
           c_dim(), dur_s, when ? when : "", c_reset());

    free(cmd);
    free(status);
    free(when);
}

int cmd_status(cluster_t *c, int argc, char **argv)
{
    char **ids;
    int n = task_list(c, &ids, 50);
    int running = 0;

    for (int i = 0; i < n; i++) {
        size_t len;
        char *meta = task_meta_read(c, ids[i], &len);
        if (!meta) continue;
        if (task_alive(meta)) {
            if (!running) printf("%sRUNNING%s\n", c_bold(), c_reset());
            print_task_line(ids[i], meta);
            running++;
        }
        free(meta);
    }

    if (!running) printf("Nothing is running.\n");
    for (int i = 0; i < n; i++) free(ids[i]);
    free(ids);
    return 0;
}

int cmd_history(cluster_t *c, int argc, char **argv)
{
    int limit = 20;
    for (int i = 0; i < argc; i++)
        if ((str_eq(argv[i], "-n") || str_eq(argv[i], "--limit")) && i + 1 < argc)
            limit = atoi(argv[++i]);

    char **ids;
    int n = task_list(c, &ids, limit);
    if (n == 0) {
        printf("No runs yet. Logs are kept in %s\n", c->log_dir);
        return 0;
    }

    printf("%-22s %-11s %-44s %9s  %s\n", "TASK", "STATUS", "COMMAND",
           "DURATION", "STARTED");
    for (int i = 0; i < n; i++) {
        size_t len;
        char *meta = task_meta_read(c, ids[i], &len);
        if (meta) {
            print_task_line(ids[i], meta);
            free(meta);
        }
        free(ids[i]);
    }
    free(ids);
    return 0;
}

int cmd_logs(cluster_t *c, int argc, char **argv)
{
    const char *id = NULL, *want_node = NULL;
    for (int i = 0; i < argc; i++) {
        if (str_eq(argv[i], "--node") && i + 1 < argc) want_node = argv[++i];
        else if (!str_has_prefix(argv[i], "-")) id = argv[i];
    }

    char *resolved = NULL;
    if (!id || str_eq(id, "last")) {
        char **ids;
        int n = task_list(c, &ids, 1);
        if (n == 0) { printf("No runs yet.\n"); return 0; }
        resolved = ids[0];
        for (int i = 1; i < n; i++) free(ids[i]);
        free(ids);
        id = resolved;
    }

    char *dir = xasprintf("%s/%s", c->log_dir, id);
    DIR *d = opendir(dir);
    if (!d) {
        warn_msg("no run called %s in %s", id, c->log_dir);
        free(dir);
        free(resolved);
        return 1;
    }

    size_t mlen;
    char *meta = task_meta_read(c, id, &mlen);
    if (meta) {
        char *cmd = json_peek_str(meta, "command");
        char *status = json_peek_str(meta, "status");
        printf("%s%s%s  %s  %s\n\n", c_bold(), id, c_reset(),
               status ? status : "", cmd ? cmd : "");
        free(cmd);
        free(status);
        free(meta);
    }

    struct dirent *e;
    int shown = 0;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".log") != 0) continue;

        char *node = xstrndup(e->d_name, len - 4);
        if (want_node && !str_eq(node, want_node)) { free(node); continue; }

        char *path = xasprintf("%s/%s", dir, e->d_name);
        size_t flen;
        char *body = read_file(path, &flen);
        free(path);

        printf("%s=== %s ===%s\n", c_bold(), node, c_reset());
        if (body && flen) fwrite(body, 1, flen, stdout);
        else              printf("%s(no output)%s\n", c_dim(), c_reset());
        if (flen && body[flen - 1] != '\n') printf("\n");
        printf("\n");

        free(body);
        free(node);
        shown++;
    }
    closedir(d);

    if (!shown && want_node)
        warn_msg("%s produced no log in this run", want_node);

    free(dir);
    free(resolved);
    return 0;
}
