/* `shard tui` — the dashboard for people who live in a terminal.
 *
 * Same data as the web UI, drawn with escape sequences and read with
 * termios. Writing it by hand rather than linking a curses library keeps the
 * promise the rest of the program makes: one binary, nothing to install.
 * A frame is built in memory and written in one go, so nothing flickers.
 */

#include "shard.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAX_RUNS 200
#define MAX_JOBS 64

typedef struct {
    char label[64];
    char node[64];
    char status[16];
    char logfile[80];
    double duration;
    int exit_code;
} tjob_t;

typedef struct {
    char   id[40];
    char   command[160];
    char   status[16];
    char   started_at[24];
    double duration;
    int    items_total, items_done, items_failed;
    tjob_t jobs[MAX_JOBS];
    int    njobs;
} trun_t;

typedef struct {
    cluster_t *c;

    health_t  *health;
    int        nhealth;
    double     health_at;

    trun_t     runs[MAX_RUNS];
    int        nruns;
    double     runs_at;

    int        pane;          /* 0 = runs list, 1 = one run in detail */
    int        sel_run;
    int        sel_job;
    int        log_scroll;    /* lines from the bottom; 0 = following */
    char      *log_text;
    char       message[120];
    double     message_at;

    int        rows, cols;
    int        quit;
} tui_t;

/* ------------------------------------------------------------- terminal */

static struct termios saved_term;
static int term_saved = 0;

static void term_restore(void)
{
    if (!term_saved) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_term);
    /* Cursor back, alternate screen off. */
    if (write(STDOUT_FILENO, "\033[?25h\033[?1049l", 14) < 0) { /* leaving anyway */ }
    term_saved = 0;
}

static void on_signal(int sig)
{
    term_restore();
    _exit(sig == SIGINT ? 130 : 143);
}

static int term_setup(void)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &saved_term) != 0) return -1;

    struct termios raw = saved_term;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return -1;
    term_saved = 1;

    atexit(term_restore);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    if (write(STDOUT_FILENO, "\033[?1049h\033[?25l", 14) < 0) return -1;
    return 0;
}

static void term_size(tui_t *t)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        t->rows = ws.ws_row;
        t->cols = ws.ws_col;
    } else {
        t->rows = 24;
        t->cols = 80;
    }
    if (t->cols > 400) t->cols = 400;
}

/* ---------------------------------------------------------------- colors */

#define FG_DIM    "\033[38;5;245m"
#define FG_GREEN  "\033[38;5;71m"
#define FG_RED    "\033[38;5;167m"
#define FG_AMBER  "\033[38;5;179m"
#define FG_BLUE   "\033[38;5;110m"
#define BOLD      "\033[1m"
#define REV       "\033[7m"
#define OFF       "\033[0m"

static const char *status_color(const char *status)
{
    if (str_eq(status, "completed")) return FG_GREEN;
    if (str_eq(status, "running"))   return FG_AMBER;
    if (str_eq(status, "failed") || str_eq(status, "timeout")) return FG_RED;
    if (str_eq(status, "interrupted")) return FG_RED;
    return FG_DIM;
}

/* Counts what a terminal shows rather than what a string holds: escape
 * sequences take no space, and a UTF-8 sequence is one column. */
static int visible_width(const char *s)
{
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '\033') {
            while (*p && *p != 'm') p++;
            if (!*p) break;
            continue;
        }
        if ((*p & 0xC0) != 0x80) w++;     /* skip UTF-8 continuation bytes */
    }
    return w;
}

/* Adds a line, cut to the screen width and cleared to the end of the row. */
static void row(tui_t *t, strbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *line = NULL;
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    line = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(line, (size_t)n + 1, fmt, ap);
    va_end(ap);

    if (visible_width(line) > t->cols) {
        /* Cut on a character boundary, keeping whole escape sequences. */
        int w = 0;
        char *p = line;
        while (*p && w < t->cols - 1) {
            if (*p == '\033') {
                while (*p && *p != 'm') p++;
                if (*p) p++;
                continue;
            }
            do { p++; } while (((unsigned char)*p & 0xC0) == 0x80);
            w++;
        }
        *p = '\0';
    }

    sb_puts(b, line);
    sb_puts(b, "\033[K\r\n");
    free(line);
}

/* Cuts a plain string to a column count, ending with … when it had to. */
static void shorten(const char *in, char *out, size_t n, int width)
{
    int w = 0;
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (w >= width - 1 && p[1]) {
            if (o + 4 < n) { memcpy(out + o, "\xe2\x80\xa6", 3); o += 3; }
            break;
        }
        if (o + 5 >= n) break;
        out[o++] = (char)*p;
        if ((*p & 0xC0) != 0x80) w++;
    }
    out[o] = '\0';
}

static void bar(char *out, size_t n, int pct, int width)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = pct * width / 100;
    out[0] = '\0';
    for (int i = 0; i < width; i++) {
        size_t len = strlen(out);
        if (len + 4 >= n) break;
        strcat(out, i < filled ? "\xe2\x96\x93" : "\xe2\x96\x91");
    }
}

/* ------------------------------------------------------------------ data */

/* task.json is JSON shard wrote itself, so a scan for the keys it always
 * contains is enough — see json_peek_str. */
static void parse_run(const char *text, trun_t *run)
{
    memset(run, 0, sizeof *run);

    char *v;
    if ((v = json_peek_str(text, "id")))         { snprintf(run->id, sizeof run->id, "%s", v); free(v); }
    if ((v = json_peek_str(text, "command")))    { snprintf(run->command, sizeof run->command, "%s", v); free(v); }
    if ((v = json_peek_str(text, "status")))     { snprintf(run->status, sizeof run->status, "%s", v); free(v); }
    if ((v = json_peek_str(text, "started_at"))) { snprintf(run->started_at, sizeof run->started_at, "%s", v); free(v); }
    run->duration     = json_peek_num(text, "duration");

    /* "running" is what the file said when it was last written; whether it is
     * still true is a question for the operating system. */
    if (str_eq(run->status, "running")) {
        int pid = (int)json_peek_num(text, "pid");
        if (pid <= 0 || kill((pid_t)pid, 0) != 0)
            snprintf(run->status, sizeof run->status, "interrupted");
    }
    run->items_total  = (int)json_peek_num(text, "items_total");
    run->items_done   = (int)json_peek_num(text, "items_done");
    run->items_failed = (int)json_peek_num(text, "items_failed");

    /* Every job object starts with its label, which makes them easy to walk. */
    const char *p = strstr(text, "\"jobs\"");
    while (p && run->njobs < MAX_JOBS) {
        p = strstr(p, "{\"label\"");
        if (!p) break;

        tjob_t *j = &run->jobs[run->njobs];
        memset(j, 0, sizeof *j);
        if ((v = json_peek_str(p, "label")))  { snprintf(j->label, sizeof j->label, "%s", v); free(v); }
        if ((v = json_peek_str(p, "node")))   { snprintf(j->node, sizeof j->node, "%s", v); free(v); }
        if ((v = json_peek_str(p, "status"))) { snprintf(j->status, sizeof j->status, "%s", v); free(v); }
        if ((v = json_peek_str(p, "log")))    { snprintf(j->logfile, sizeof j->logfile, "%s", v); free(v); }
        j->duration  = json_peek_num(p, "duration");
        j->exit_code = (int)json_peek_num(p, "exit");
        run->njobs++;
        p++;
    }
}

static void load_runs(tui_t *t)
{
    char **ids;
    int n = task_list(t->c, &ids, MAX_RUNS);
    t->nruns = 0;

    for (int i = 0; i < n; i++) {
        size_t len;
        char *meta = task_meta_read(t->c, ids[i], &len);
        if (meta) {
            parse_run(meta, &t->runs[t->nruns]);
            if (!t->runs[t->nruns].id[0])
                snprintf(t->runs[t->nruns].id, sizeof t->runs[t->nruns].id, "%s", ids[i]);
            t->nruns++;
            free(meta);
        }
        free(ids[i]);
    }
    free(ids);
    t->runs_at = now_seconds();

    if (t->sel_run >= t->nruns) t->sel_run = t->nruns ? t->nruns - 1 : 0;
}

/* The health file is written by `shard cluster health` and by the web UI, so
 * the three of them agree about what they last saw. */
static void load_health(tui_t *t)
{
    free(t->health);
    t->health = xmalloc(sizeof(health_t) * (size_t)(t->c->nnodes ? t->c->nnodes : 1));
    t->nhealth = t->c->nnodes;
    memset(t->health, 0, sizeof(health_t) * (size_t)(t->c->nnodes ? t->c->nnodes : 1));

    char *path = health_state_path(t->c);
    size_t len;
    char *text = read_file(path, &len);
    free(path);

    for (int i = 0; i < t->c->nnodes; i++) {
        health_t *h = &t->health[i];
        snprintf(h->name, sizeof h->name, "%s", t->c->nodes[i].name);
        h->ms = -1;                    /* nothing measured yet */
        if (!text) continue;

        char *key = xasprintf("\"%s\"", t->c->nodes[i].name);
        const char *at = strstr(text, key);
        free(key);
        if (!at) continue;
        h->ms = 0;

        char *st = json_peek_str(at, "status");
        h->online = str_eq(st, "online");
        free(st);
        h->cpu_pct = (int)json_peek_num(at, "cpu_pct");
        h->ram_pct = (int)json_peek_num(at, "ram_pct");
        h->ncpu    = (int)json_peek_num(at, "ncpu");
        h->ram_mb  = (long)json_peek_num(at, "ram_mb");
    }
    free(text);
    t->health_at = now_seconds();
}

static void load_log(tui_t *t)
{
    free(t->log_text);
    t->log_text = NULL;
    if (t->sel_run >= t->nruns) return;

    trun_t *r = &t->runs[t->sel_run];
    if (t->sel_job >= r->njobs) return;

    char *path = xasprintf("%s/%s/%s", t->c->log_dir, r->id, r->jobs[t->sel_job].logfile);
    size_t len;
    t->log_text = read_file(path, &len);
    free(path);
}

/* ------------------------------------------------------------------ draw */

static void draw_header(tui_t *t, strbuf *b)
{
    int online = 0;
    for (int i = 0; i < t->nhealth; i++) if (t->health[i].online) online++;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char clock[16];
    strftime(clock, sizeof clock, "%H:%M:%S", &tm);

    char left[256];
    snprintf(left, sizeof left, " %sshard%s  %s  %s%d/%d online%s",
             BOLD, OFF, t->c->name, online == t->c->nnodes ? FG_GREEN : FG_AMBER,
             online, t->c->nnodes, OFF);

    int pad = t->cols - visible_width(left) - (int)strlen(clock) - 1;
    if (pad < 1) pad = 1;
    row(t, b, "%s%*s%s%s%s", left, pad, "", FG_DIM, clock, OFF);

    strbuf rule;
    sb_init(&rule);
    for (int i = 0; i < t->cols; i++) sb_puts(&rule, "\xe2\x94\x80");
    row(t, b, "%s%s%s", FG_DIM, rule.data, OFF);
    sb_free(&rule);
}

static void draw_nodes(tui_t *t, strbuf *b)
{
    row(t, b, " %sNODES%s", FG_DIM, OFF);

    for (int i = 0; i < t->c->nnodes; i++) {
        node_t *n = &t->c->nodes[i];
        health_t *h = i < t->nhealth ? &t->health[i] : NULL;
        int online = h && h->online;

        if (!online) {
            row(t, b, " %s\xe2\x97\x8b %-16s %s%s", FG_DIM, n->name,
                h && h->ms >= 0 ? "offline" : "not checked yet \xc2\xb7 press h", OFF);
            continue;
        }

        char cbar[64], rbar[64];
        bar(cbar, sizeof cbar, h->cpu_pct, 8);
        bar(rbar, sizeof rbar, h->ram_pct, 8);
        row(t, b, " %s\xe2\x97\x8f%s %-16s %s%s %3d%% cpu  %s %3d%% ram  %s%d cpu%s",
            FG_GREEN, OFF, n->name, FG_BLUE, cbar, h->cpu_pct, rbar, h->ram_pct,
            FG_DIM, h->ncpu, OFF);
    }
    row(t, b, "");
}

static void run_counts(trun_t *r, char *out, size_t n)
{
    if (r->items_total > 0) {
        snprintf(out, n, "%d/%d items", r->items_done, r->items_total);
        return;
    }
    int done = 0;
    for (int i = 0; i < r->njobs; i++)
        if (!str_eq(r->jobs[i].status, "running") && !str_eq(r->jobs[i].status, "pending"))
            done++;
    snprintf(out, n, "%d/%d jobs", done, r->njobs);
}

static void draw_runs(tui_t *t, strbuf *b, int height)
{
    row(t, b, " %sRUNS%s", FG_DIM, OFF);

    int first = 0;
    if (t->sel_run >= height) first = t->sel_run - height + 1;

    for (int i = first; i < t->nruns && i < first + height; i++) {
        trun_t *r = &t->runs[i];
        char counts[32], dur[32], cmd[160];
        run_counts(r, counts, sizeof counts);
        fmt_duration(r->duration, dur, sizeof dur);

        int cmd_w = t->cols - 46;
        if (cmd_w < 12) cmd_w = 12;
        shorten(r->command, cmd, sizeof cmd, cmd_w);

        /* The date is nearly always today; the time is the useful half. */
        const char *when = strlen(r->started_at) > 11 ? r->started_at + 11 : r->started_at;

        const char *sel = i == t->sel_run ? REV : "";
        row(t, b, "%s %s%-11s%s %-*s %s%12s %8s  %s%s",
            sel, status_color(r->status), r->status, OFF, cmd_w, cmd,
            FG_DIM, counts, dur, when, OFF);
    }

    if (t->nruns == 0) row(t, b, " %sno runs yet%s", FG_DIM, OFF);
}

static void draw_detail(tui_t *t, strbuf *b, int height)
{
    trun_t *r = &t->runs[t->sel_run];

    char counts[32], dur[32];
    run_counts(r, counts, sizeof counts);
    fmt_duration(r->duration, dur, sizeof dur);

    row(t, b, " %s%s%s  %s%s%s  %s  %s%s · %s%s", BOLD, r->id, OFF,
        status_color(r->status), r->status, OFF, r->command,
        FG_DIM, counts, dur, OFF);

    if (r->items_total > 0) {
        char pbar[80];
        bar(pbar, sizeof pbar, r->items_done * 100 / (r->items_total ? r->items_total : 1), 24);
        row(t, b, " %s%s%s %d%%%s%s", FG_BLUE, pbar, OFF,
            r->items_done * 100 / (r->items_total ? r->items_total : 1),
            r->items_failed ? "  " : "",
            r->items_failed ? FG_RED : "");
    }
    row(t, b, "");

    int job_rows = r->njobs < 6 ? r->njobs : 6;
    for (int i = 0; i < job_rows; i++) {
        tjob_t *j = &r->jobs[i];
        char jd[32];
        fmt_duration(j->duration, jd, sizeof jd);
        const char *sel = i == t->sel_job ? REV : "";
        row(t, b, "%s %-24s %s%-9s%s %s%8s%s", sel, j->label,
            status_color(j->status), j->status, OFF, FG_DIM, jd, OFF);
    }
    if (r->njobs > job_rows)
        row(t, b, " %s… %d more, n/p to move%s", FG_DIM, r->njobs - job_rows, OFF);
    row(t, b, "");

    /* The log of the selected job, tail first unless it has been scrolled. */
    int log_rows = height - job_rows - 5;
    if (log_rows < 3) log_rows = 3;

    if (!t->log_text || !*t->log_text) {
        row(t, b, " %s(no output yet)%s", FG_DIM, OFF);
        return;
    }

    int total = 0;
    for (char *p = t->log_text; *p; p++) if (*p == '\n') total++;

    int start = total - log_rows - t->log_scroll;
    if (start < 0) start = 0;

    int line = 0;
    char *p = t->log_text;
    while (*p && line < start) {
        char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
        line++;
    }
    for (int i = 0; i < log_rows && *p; i++) {
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char *text = xstrndup(p, len);
        row(t, b, " %s", text);
        free(text);
        if (!nl) break;
        p = nl + 1;
    }
}

static void draw_footer(tui_t *t, strbuf *b)
{
    if (t->message[0] && now_seconds() - t->message_at < 4.0) {
        row(t, b, " %s%s%s", FG_AMBER, t->message, OFF);
        return;
    }
    if (t->pane == 0)
        row(t, b, " %s[q]uit  [j/k] move  [enter] open  [h]ealth check  [r]efresh%s",
            FG_DIM, OFF);
    else
        row(t, b, " %s[esc] back  [n/p] job  [j/k] scroll log  [g/G] top/end  [q]uit%s",
            FG_DIM, OFF);
}

static void draw(tui_t *t)
{
    term_size(t);

    strbuf b;
    sb_init(&b);
    sb_puts(&b, "\033[H");            /* home, then clear as we go */

    draw_header(t, &b);

    if (t->pane == 0) {
        draw_nodes(t, &b);
        int used = 3 + t->c->nnodes + 2;
        draw_runs(t, &b, t->rows - used - 2);
    } else {
        draw_detail(t, &b, t->rows - 5);
    }

    /* Push the footer to the last row. */
    int lines = 0;
    for (size_t i = 0; i < b.len; i++) if (b.data[i] == '\n') lines++;
    for (int i = lines; i < t->rows - 1; i++) sb_puts(&b, "\033[K\r\n");

    draw_footer(t, &b);
    sb_puts(&b, "\033[J");

    if (write(STDOUT_FILENO, b.data, b.len) < 0) t->quit = 1;
    sb_free(&b);
}

/* ----------------------------------------------------------------- input */

static void say(tui_t *t, const char *msg)
{
    snprintf(t->message, sizeof t->message, "%s", msg);
    t->message_at = now_seconds();
}

static void run_health(tui_t *t)
{
    say(t, "checking every machine…");
    draw(t);

    node_t **nodes;
    int n = cluster_select(t->c, "all", &nodes);
    if (n > 0) {
        health_t *h = xmalloc(sizeof(health_t) * (size_t)n);
        health_check(t->c, nodes, n, h);
        health_write(t->c, h, n);
        free(h);
    }
    free(nodes);

    load_health(t);
    say(t, "health check done");
}

static void handle_key(tui_t *t, const char *key)
{
    if (str_eq(key, "q")) { t->quit = 1; return; }

    if (t->pane == 0) {
        if (str_eq(key, "j") || str_eq(key, "down")) {
            if (t->sel_run + 1 < t->nruns) t->sel_run++;
        } else if (str_eq(key, "k") || str_eq(key, "up")) {
            if (t->sel_run > 0) t->sel_run--;
        } else if (str_eq(key, "enter") || str_eq(key, "l") || str_eq(key, "right")) {
            if (t->nruns) {
                t->pane = 1;
                t->sel_job = 0;
                t->log_scroll = 0;
                load_log(t);
            }
        } else if (str_eq(key, "h")) {
            run_health(t);
        } else if (str_eq(key, "r")) {
            load_runs(t);
            load_health(t);
            say(t, "reloaded");
        }
        return;
    }

    if (str_eq(key, "esc") || str_eq(key, "left") || str_eq(key, "b")) {
        t->pane = 0;
    } else if (str_eq(key, "n")) {
        trun_t *r = &t->runs[t->sel_run];
        if (t->sel_job + 1 < r->njobs) { t->sel_job++; t->log_scroll = 0; load_log(t); }
    } else if (str_eq(key, "p")) {
        if (t->sel_job > 0) { t->sel_job--; t->log_scroll = 0; load_log(t); }
    } else if (str_eq(key, "j") || str_eq(key, "down")) {
        if (t->log_scroll > 0) t->log_scroll--;
    } else if (str_eq(key, "k") || str_eq(key, "up")) {
        t->log_scroll++;
    } else if (str_eq(key, "G")) {
        t->log_scroll = 0;
    } else if (str_eq(key, "g")) {
        t->log_scroll = 100000;
    }
}

/* Turns what arrived on stdin into one of the names handle_key knows. */
static void read_keys(tui_t *t)
{
    char buf[32];
    ssize_t n = read(STDIN_FILENO, buf, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    for (ssize_t i = 0; i < n && !t->quit; i++) {
        if (buf[i] == '\033') {
            if (i + 2 < n && buf[i + 1] == '[') {
                switch (buf[i + 2]) {
                case 'A': handle_key(t, "up");    break;
                case 'B': handle_key(t, "down");  break;
                case 'C': handle_key(t, "right"); break;
                case 'D': handle_key(t, "left");  break;
                }
                i += 2;
            } else {
                handle_key(t, "esc");
            }
            continue;
        }
        if (buf[i] == '\r' || buf[i] == '\n') { handle_key(t, "enter"); continue; }

        char key[2] = { buf[i], '\0' };
        handle_key(t, key);
    }
}

int cmd_tui(cluster_t *c, int argc, char **argv)
{
    if (term_setup() != 0) {
        warn_msg("shard tui needs a terminal. For a browser, run: shard ui");
        return 1;
    }

    tui_t t;
    memset(&t, 0, sizeof t);
    t.c = c;
    load_health(&t);
    load_runs(&t);

    while (!t.quit) {
        draw(&t);

        struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
        if (poll(&p, 1, 500) > 0 && (p.revents & POLLIN)) read_keys(&t);

        /* Runs change while you watch them; the health file only when
         * something goes and measures it. */
        if (now_seconds() - t.runs_at > 1.0) {
            load_runs(&t);
            if (t.pane == 1) load_log(&t);
        }
        if (now_seconds() - t.health_at > 5.0) load_health(&t);
    }

    term_restore();
    free(t.health);
    free(t.log_text);
    return 0;
}
