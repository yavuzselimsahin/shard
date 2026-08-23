/* The job runner.
 *
 * A job is one command on one node. The runner starts up to max_parallel of
 * them, keeps their output flowing into per-job log files, and reports each
 * one as it finishes. Remote jobs are ordinary `ssh` child processes: no
 * agent, no daemon, nothing installed on the far end.
 */

#include "shard.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void runner_init(runner_t *r)
{
    memset(r, 0, sizeof *r);
    r->max_parallel = 0;      /* 0 means "all of them at once" */
}

/* Log file names come from the label, so they stay readable in ~/.shard. */
static char *sanitize(const char *s)
{
    char *out = xmalloc(strlen(s) + 1);
    size_t o = 0;
    for (const char *p = s; *p; p++) {
        int plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || *p == '_' || *p == '.';
        if (plain) {
            out[o++] = *p;
        } else if (o > 0 && out[o - 1] != '-') {
            /* One dash for a run of anything else, so the middle dot in
             * "laptop·build" does not become two. */
            out[o++] = '-';
        }
    }
    while (o > 0 && out[o - 1] == '-') o--;
    out[o] = '\0';
    return out;
}

int runner_add(runner_t *r, node_t *n, const char *label, const char *cmd)
{
    r->jobs = xrealloc(r->jobs, sizeof(job_t) * (size_t)(r->njobs + 1));
    job_t *j = &r->jobs[r->njobs++];
    memset(j, 0, sizeof *j);
    j->node  = n;
    j->label = xstrdup(label ? label : n->name);
    j->cmd   = xstrdup(cmd);
    j->fd    = -1;
    j->in_fd = -1;
    j->state = JOB_PENDING;
    j->exit_code = -1;
    j->log_name = sanitize(j->label);
    return r->njobs - 1;
}

char **node_argv(const node_t *n, const char *command)
{
    char *cmd = n->workdir
        ? xasprintf("cd %s && %s", n->workdir, command)
        : xstrdup(command);

    char **argv = xmalloc(sizeof(char *) * 20);
    int a = 0;

    if (n->is_local) {
        argv[a++] = xstrdup("/bin/sh");
        argv[a++] = xstrdup("-c");
        argv[a++] = cmd;
        argv[a] = NULL;
        return argv;
    }

    argv[a++] = xstrdup("ssh");
    argv[a++] = xstrdup("-o"); argv[a++] = xstrdup("BatchMode=yes");
    argv[a++] = xstrdup("-o"); argv[a++] = xstrdup("StrictHostKeyChecking=accept-new");
    argv[a++] = xstrdup("-o"); argv[a++] = xstrdup("ConnectTimeout=10");
    argv[a++] = xstrdup("-o"); argv[a++] = xstrdup("LogLevel=ERROR");
    if (n->key) {
        argv[a++] = xstrdup("-i");
        argv[a++] = xstrdup(n->key);
    }
    if (n->port) {
        argv[a++] = xstrdup("-p");
        argv[a++] = xasprintf("%d", n->port);
    }
    argv[a++] = n->user ? xasprintf("%s@%s", n->user, n->host) : xstrdup(n->host);
    argv[a++] = cmd;
    argv[a] = NULL;
    return argv;
}

char **job_argv(const job_t *j)
{
    return node_argv(j->node, j->cmd);
}

void argv_free(char **argv)
{
    if (!argv) return;
    for (int i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}

static void job_start(runner_t *r, job_t *j)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) die("pipe: %s", strerror(errno));

    if (r->task_dir) {
        if (!j->log_path)
            j->log_path = xasprintf("%s/%s.log", r->task_dir, j->log_name);
        /* A retry adds to the log rather than hiding what went wrong. */
        j->log = fopen(j->log_path, j->attempt ? "a" : "w");
        if (j->log && j->attempt)
            fprintf(j->log, "\n--- attempt %d ---\n", j->attempt + 1);
    }

    /* Live dispatch keeps the job's stdin open and writes work into it as the
     * job asks for more; otherwise stdin is a file, or nothing. */
    int in_fd = -1, in_write = -1;
    if (r->marker) {
        int inp[2];
        if (pipe(inp) != 0) die("pipe: %s", strerror(errno));
        in_fd = inp[0];
        in_write = inp[1];
    } else if (j->stdin_path) {
        in_fd = open(j->stdin_path, O_RDONLY);
        if (in_fd < 0) warn_msg("cannot open %s: %s", j->stdin_path, strerror(errno));
    }
    if (in_fd < 0) in_fd = open("/dev/null", O_RDONLY);

    char **argv = job_argv(j);
    pid_t pid = fork();
    if (pid < 0) die("fork: %s", strerror(errno));

    if (pid == 0) {
        /* Its own process group, so a timeout can take down ssh and whatever
         * it spawned rather than just the parent process. */
        setpgid(0, 0);
        close(pipefd[0]);
        if (in_write >= 0) close(in_write);
        dup2(in_fd, STDIN_FILENO);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        if (in_fd > 2) close(in_fd);
        execvp(argv[0], argv);
        fprintf(stderr, "shard: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    close(pipefd[1]);
    if (in_fd > 2) close(in_fd);
    argv_free(argv);

    /* Non-blocking, so drain() can read until the pipe is empty. */
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    j->pid   = pid;
    j->fd    = pipefd[0];
    j->in_fd = in_write;
    j->state = JOB_RUNNING;
    j->start = now_seconds();

    /* The job is up and has nothing to do yet: this is where a dispatcher
     * hands it its first piece of work. */
    if (r->on_event) r->on_event(r, j, NULL, r->event_ctx);
}

void job_send(job_t *j, const char *line)
{
    if (j->in_fd < 0) return;
    size_t len = strlen(line);
    if (write(j->in_fd, line, len) < 0) return;
    if (write(j->in_fd, "\n", 1) < 0) return;
}

void job_end_input(job_t *j)
{
    if (j->in_fd < 0) return;
    close(j->in_fd);
    j->in_fd = -1;
}

static const char *state_word(job_state s)
{
    switch (s) {
    case JOB_OK:      return "completed";
    case JOB_FAILED:  return "failed";
    case JOB_TIMEOUT: return "timed out";
    case JOB_RUNNING: return "running";
    case JOB_SKIPPED: return "skipped";
    default:          return "pending";
    }
}

static void report(runner_t *r, job_t *j, int label_w)
{
    if (r->quiet) return;

    /* A progress line may be sitting on this row; take it off first. */
    if (color_enabled()) printf("\r\033[K");

    char dur[32];
    fmt_duration(j->end - j->start, dur, sizeof dur);

    const char *text = j->show ? j->show : j->cmd;
    char cmd[41];
    snprintf(cmd, sizeof cmd, "%s", text);
    if (strlen(text) > 40) memcpy(cmd + 37, "...", 4);

    const char *mark, *color;
    if (j->state == JOB_OK)      { mark = "\xe2\x9c\x93"; color = c_green(); }
    else if (j->state == JOB_TIMEOUT) { mark = "\xe2\x8f\xb1"; color = c_yellow(); }
    else if (j->state == JOB_SKIPPED) { mark = "\xe2\x80\x93"; color = c_dim(); }
    else                         { mark = "\xe2\x9c\x97"; color = c_red(); }

    printf("[%-*s] %-40s %s%s %s%s", label_w, j->label, cmd,
           color, mark, state_word(j->state), c_reset());
    if (j->state == JOB_FAILED && j->exit_code > 0)
        printf(" (exit %d)", j->exit_code);
    printf(" %s(%s)%s", c_dim(), dur, c_reset());
    if (j->state != JOB_OK && j->state != JOB_SKIPPED && j->attempt < r->retries)
        printf(" %s\xe2\x86\xba trying again%s", c_yellow(), c_reset());
    printf("\n");
    fflush(stdout);
}

/* Reads whatever is available, writes it to the log, and — in stream mode —
 * echoes complete lines with the node's name in front. */
static void drain(runner_t *r, job_t *j, strbuf *pending, int label_w)
{
    char buf[4096];
    for (;;) {
        ssize_t n = read(j->fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;                       /* EAGAIN: nothing more for now */
        }
        if (n == 0) {                     /* EOF */
            close(j->fd);
            j->fd = -1;
            if (pending->len) {
                if (r->marker && j->log) {
                    fwrite(pending->data, 1, pending->len, j->log);
                    fputc('\n', j->log);
                }
                if (r->stream)
                    printf("[%-*s] %s\n", label_w, j->label, pending->data);
                pending->len = 0;
                pending->data[0] = '\0';
            }
            return;
        }

        j->bytes += (size_t)n;
        if (r->capture_output) sb_add(&j->capture, buf, (size_t)n);

        /* Without a marker there is nothing to look for in the output, so it
         * goes straight to the log in whatever size it arrived. */
        if (!r->marker) {
            if (j->log) { fwrite(buf, 1, (size_t)n, j->log); fflush(j->log); }
            if (!r->stream) continue;
        }

        sb_add(pending, buf, (size_t)n);
        char *start = pending->data;
        char *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';

            if (r->marker && str_has_prefix(start, r->marker)) {
                /* An event, not output: it never reaches the log. */
                const char *rest = start + strlen(r->marker);
                while (*rest == ' ') rest++;
                if (r->on_event) r->on_event(r, j, rest, r->event_ctx);
            } else {
                if (r->marker && j->log) {
                    fwrite(start, 1, strlen(start), j->log);
                    fputc('\n', j->log);
                    /* Flushed as it arrives: `tail -f`, the dashboard and the
                     * TUI all read this file while the job is still going. */
                    fflush(j->log);
                }
                if (r->stream) printf("[%-*s] %s\n", label_w, j->label, start);
            }
            start = nl + 1;
        }
        size_t left = pending->len - (size_t)(start - pending->data);
        memmove(pending->data, start, left + 1);
        pending->len = left;
        if (r->stream) fflush(stdout);
    }
}

static void job_finish(runner_t *r, job_t *j, int label_w)
{
    job_end_input(j);
    int status = 0;
    waitpid(j->pid, &status, 0);
    j->end = now_seconds();

    if (j->state != JOB_TIMEOUT) {
        if (WIFEXITED(status)) {
            j->exit_code = WEXITSTATUS(status);
            j->state = j->exit_code == 0 ? JOB_OK : JOB_FAILED;
        } else {
            j->exit_code = 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            j->state = JOB_FAILED;
        }
    }

    if (j->log) { fclose(j->log); j->log = NULL; }
    report(r, j, label_w);
}

/* Runs one child to completion, its stdin optionally from a file, its output
 * discarded. Used to ship files and to clean them up afterwards. */
static int stage_run(char **argv, const char *in_path)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int in = in_path ? open(in_path, O_RDONLY) : open("/dev/null", O_RDONLY);
        if (in >= 0) { dup2(in, STDIN_FILENO); close(in); }
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDOUT_FILENO); close(dn); }
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* Ships the files in r->ship to every remote worker before it runs: pack them
 * once, unpack them into a per-run directory on each machine, and point the
 * work at that directory. This is what lets a job run on a machine that has
 * never seen the code — it only lends its CPU. Local nodes already have the
 * files in the working directory, so they are left alone. The saved workdirs
 * are restored and the copies removed by stage_teardown. */
static void stage_setup(runner_t *r, node_t ***staged_out, char ***saved_out,
                        int *n_out, char **dir_out)
{
    *staged_out = NULL; *saved_out = NULL; *n_out = 0; *dir_out = NULL;
    if (r->nship == 0) return;

    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    char *tar = xasprintf("%s/shard-ship-%d.tar", tmp, (int)getpid());

    char **targv = xmalloc(sizeof(char *) * (size_t)(r->nship + 5));
    int a = 0;
    targv[a++] = xstrdup("tar");
    targv[a++] = xstrdup("cf");
    targv[a++] = xstrdup(tar);
    targv[a++] = xstrdup("--");
    for (int i = 0; i < r->nship; i++) targv[a++] = xstrdup(r->ship[i]);
    targv[a] = NULL;
    int rc = stage_run(targv, NULL);
    argv_free(targv);

    if (rc != 0) {
        warn_msg("could not pack the files to ship (tar exit %d)", rc);
        free(tar);
        return;
    }

    /* Absolute via $HOME, so it resolves the same however the shell was
     * started and whatever workdir a node had configured. */
    char *dir = xasprintf("$HOME/.shard/stage/%d", (int)getpid());

    node_t **staged = NULL;
    char   **saved = NULL;
    int      n = 0;

    for (int i = 0; i < r->njobs; i++) {
        node_t *nd = r->jobs[i].node;
        if (nd->is_local) continue;

        int seen = 0;
        for (int k = 0; k < n; k++) if (staged[k] == nd) seen = 1;
        if (seen) continue;

        char *cmd = xasprintf("mkdir -p %s && tar xf - -C %s", dir, dir);
        char **av = node_argv(nd, cmd);      /* uses the node's old workdir */
        int prc = stage_run(av, tar);
        argv_free(av);
        free(cmd);

        staged = xrealloc(staged, sizeof(node_t *) * (size_t)(n + 1));
        saved  = xrealloc(saved, sizeof(char *) * (size_t)(n + 1));
        staged[n] = nd;
        saved[n] = nd->workdir;              /* keep the old one to restore */
        nd->workdir = xstrdup(dir);          /* the job now runs in the copy */
        n++;

        if (prc != 0)
            warn_msg("could not ship files to %s (exit %d)", nd->name, prc);
        else if (!r->quiet)
            printf("  %s\xe2\x86\x91 shipped %d file%s to %s%s\n", c_dim(),
                   r->nship, r->nship == 1 ? "" : "s", nd->name, c_reset());
    }

    unlink(tar);
    free(tar);
    *staged_out = staged;
    *saved_out = saved;
    *n_out = n;
    *dir_out = dir;
}

static void stage_teardown(runner_t *r, node_t **staged, char **saved,
                           int n, char *dir)
{
    for (int i = 0; i < n; i++) {
        node_t *nd = staged[i];
        free(nd->workdir);
        nd->workdir = saved[i];              /* put the old workdir back */

        if (!r->keep_stage && dir) {
            char *cmd = xasprintf("rm -rf %s", dir);
            char **av = node_argv(nd, cmd);
            stage_run(av, NULL);
            argv_free(av);
            free(cmd);
        }
    }
    free(staged);
    free(saved);
    free(dir);
}

static int is_over(job_state s)
{
    return s == JOB_OK || s == JOB_FAILED || s == JOB_TIMEOUT || s == JOB_SKIPPED;
}

int runner_run(runner_t *r)
{
    if (r->njobs == 0) return 0;
    if (r->max_parallel <= 0) r->max_parallel = r->njobs;

    int label_w = 1;
    for (int i = 0; i < r->njobs; i++) {
        int w = (int)strlen(r->jobs[i].label);
        if (w > label_w) label_w = w;
    }

    strbuf *pending = xmalloc(sizeof(strbuf) * (size_t)r->njobs);
    for (int i = 0; i < r->njobs; i++) sb_init(&pending[i]);

    node_t **staged; char **stage_saved; int nstaged; char *stage_dir;
    stage_setup(r, &staged, &stage_saved, &nstaged, &stage_dir);

    int aborted = 0;
    double last_meta = 0;

    for (;;) {
        int running = 0, over = 0;
        for (int i = 0; i < r->njobs; i++) {
            if (r->jobs[i].state == JOB_RUNNING) running++;
            else if (is_over(r->jobs[i].state)) over++;
        }
        if (over == r->njobs) break;

        /* Start whatever is waiting, oldest first. A retry is a job that went
         * back to pending, so it is picked up here like any other. */
        for (int i = 0; i < r->njobs && running < r->max_parallel && !aborted; i++) {
            if (r->jobs[i].state != JOB_PENDING) continue;
            if (r->before_job) r->before_job(r, &r->jobs[i], r->event_ctx);
            job_start(r, &r->jobs[i]);
            running++;
            if (r->task_dir) task_write_meta(r, "running");
        }

        struct pollfd *pfd = xmalloc(sizeof(struct pollfd) * (size_t)r->njobs);
        job_t **owner = xmalloc(sizeof(job_t *) * (size_t)r->njobs);
        int np = 0;
        for (int i = 0; i < r->njobs; i++) {
            job_t *j = &r->jobs[i];
            if (j->state != JOB_RUNNING || j->fd < 0) continue;
            pfd[np].fd = j->fd;
            pfd[np].events = POLLIN;
            pfd[np].revents = 0;
            owner[np] = j;
            np++;
        }

        if (np > 0) {
            poll(pfd, (nfds_t)np, 200);
        } else {
            struct timespec ts = { 0, 20 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }

        for (int i = 0; i < np; i++) {
            if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            job_t *j = owner[i];
            drain(r, j, &pending[(int)(j - r->jobs)], label_w);
        }

        free(pfd);
        free(owner);

        double now = now_seconds();
        for (int i = 0; i < r->njobs; i++) {
            job_t *j = &r->jobs[i];
            if (j->state != JOB_RUNNING) continue;

            int timed_out = r->timeout > 0 && now - j->start > r->timeout && j->pid > 0;
            if (timed_out) {
                kill(-j->pid, SIGKILL);
                j->state = JOB_TIMEOUT;
                j->exit_code = 124;
                if (j->fd >= 0) { close(j->fd); j->fd = -1; }
            } else if (j->fd >= 0) {
                continue;                  /* still talking */
            }

            job_finish(r, j, label_w);
            if (r->after_job) r->after_job(r, j, r->event_ctx);
            if (r->abort_run) aborted = 1;

            if (j->state != JOB_OK && j->attempt < r->retries) {
                /* Another go, on the same machine: what failed is usually the
                 * command, and moving it elsewhere is the dispatcher's job. */
                j->attempt++;
                j->state = JOB_PENDING;
                j->exit_code = -1;
                pending[i].len = 0;
                if (pending[i].data) pending[i].data[0] = '\0';
                continue;
            }

            if (r->stop_on_fail && j->state != JOB_OK) aborted = 1;
        }

        if (aborted) {
            for (int i = 0; i < r->njobs; i++)
                if (r->jobs[i].state == JOB_PENDING) {
                    r->jobs[i].state = JOB_SKIPPED;
                    report(r, &r->jobs[i], label_w);
                }
        }

        if (r->task_dir && now - last_meta > 1.0) {
            task_write_meta(r, "running");
            last_meta = now;
        }
    }

    for (int i = 0; i < r->njobs; i++) sb_free(&pending[i]);
    free(pending);
    if (color_enabled() && !r->quiet) printf("\r\033[K");

    stage_teardown(r, staged, stage_saved, nstaged, stage_dir);

    int failed = 0;
    for (int i = 0; i < r->njobs; i++)
        if (r->jobs[i].state == JOB_FAILED || r->jobs[i].state == JOB_TIMEOUT)
            failed++;

    if (r->task_dir)
        task_write_meta(r, failed ? "failed" : "completed");

    return failed;
}

void runner_free(runner_t *r)
{
    for (int i = 0; i < r->njobs; i++) {
        job_t *j = &r->jobs[i];
        free(j->label);
        free(j->cmd);
        free(j->show);
        free(j->stdin_path);
        free(j->log_path);
        free(j->log_name);
        sb_free(&j->capture);
        if (j->log) fclose(j->log);
    }
    free(r->jobs);
    r->jobs = NULL;
    r->njobs = 0;
}
