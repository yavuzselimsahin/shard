/* Node health.
 *
 * One small shell snippet is sent to every node in parallel. It answers with
 * a single line, which is all a health check needs: reachable or not, how
 * many CPUs, the load, and how much memory is in use. No agent involved —
 * this is the same SSH path an ordinary job takes.
 */

#include "shard.h"

#include <stdlib.h>
#include <string.h>

const char *SHARD_PROBE_CMD =
    "n=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1); "
    "l=$(uptime | tr ',' ' ' | "
    "awk '{for(i=1;i<=NF;i++) if($i ~ /average/) {print $(i+1); exit}}'); "
    "[ -z \"$l\" ] && l=0; "
    "if [ -r /proc/meminfo ]; then "
    "t=$(awk '/^MemTotal/{printf \"%d\", $2/1024}' /proc/meminfo); "
    "a=$(awk '/^MemAvailable/{printf \"%d\", $2/1024}' /proc/meminfo); "
    "[ -z \"$a\" ] && a=$(awk '/^MemFree/{printf \"%d\", $2/1024}' /proc/meminfo); "
    "u=$((t-a)); "
    "else "
    "t=$(( $(sysctl -n hw.memsize 2>/dev/null || echo 0) / 1048576 )); "
    "p=$(vm_stat 2>/dev/null | awk '/page size of/{print $8}'); "
    "[ -z \"$p\" ] && p=4096; "
    "f=$(vm_stat 2>/dev/null | awk '/Pages free/{print $3}' | tr -d '.'); "
    "i=$(vm_stat 2>/dev/null | awk '/Pages inactive/{print $3}' | tr -d '.'); "
    "[ -z \"$f\" ] && f=0; [ -z \"$i\" ] && i=0; "
    "u=$(( t - (f+i)*p/1048576 )); "
    "fi; "
    "echo \"SHARD_PROBE $n $l $t $u\"";

/* The probe's own line is the answer; anything else the node printed (an
 * SSH warning, a motd) is kept as the error text when it never arrives. */
static void parse_probe(const char *out, health_t *h)
{
    const char *p = out ? strstr(out, "SHARD_PROBE") : NULL;
    if (!p) {
        h->online = 0;
        if (out) {
            snprintf(h->error, sizeof h->error, "%s", out);
            char *nl = strchr(h->error, '\n');
            if (nl) *nl = '\0';
        }
        return;
    }

    int   ncpu = 1;
    double load = 0;
    long  total = 0, used = 0;
    if (sscanf(p, "SHARD_PROBE %d %lf %ld %ld", &ncpu, &load, &total, &used) < 1)
        return;

    h->online = 1;
    h->ncpu = ncpu > 0 ? ncpu : 1;
    h->load1 = load;
    h->ram_mb = total;
    h->ram_used_mb = used;

    int cpu_pct = (int)(load / h->ncpu * 100.0 + 0.5);
    h->cpu_pct = cpu_pct < 0 ? 0 : (cpu_pct > 100 ? 100 : cpu_pct);
    h->ram_pct = total > 0 ? (int)((double)used / (double)total * 100.0 + 0.5) : 0;
    if (h->ram_pct < 0)   h->ram_pct = 0;
    if (h->ram_pct > 100) h->ram_pct = 100;
}

int health_check(cluster_t *c, node_t **nodes, int n, health_t *out)
{
    runner_t r;
    runner_init(&r);
    r.quiet = 1;
    r.capture_output = 1;
    r.timeout = 20;

    for (int i = 0; i < n; i++)
        runner_add(&r, nodes[i], nodes[i]->name, SHARD_PROBE_CMD);

    runner_run(&r);

    int online = 0;
    for (int i = 0; i < n; i++) {
        health_t *h = &out[i];
        memset(h, 0, sizeof *h);
        snprintf(h->name, sizeof h->name, "%s", nodes[i]->name);

        job_t *j = &r.jobs[i];
        h->ms = (j->end - j->start) * 1000.0;

        if (j->state == JOB_TIMEOUT) {
            snprintf(h->error, sizeof h->error, "timed out");
        } else {
            parse_probe(j->capture.data, h);
            if (!h->online && !h->error[0])
                snprintf(h->error, sizeof h->error, "no answer (exit %d)",
                         j->exit_code);
        }
        if (h->online) online++;
    }

    runner_free(&r);
    return online;
}

char *health_state_path(cluster_t *c)
{
    char *home = home_dir();
    char *dir = xasprintf("%s/.shard/state", home);
    free(home);
    mkdir_p(dir);
    char *path = xasprintf("%s/%s-health.json", dir, c->name);
    free(dir);
    return path;
}

void health_write(cluster_t *c, health_t *h, int n)
{
    char *path = health_state_path(c);
    FILE *f = fopen(path, "w");
    if (!f) { free(path); return; }

    strbuf b;
    sb_init(&b);
    sb_printf(&b, "{\n  \"checked\": %.0f,\n  \"nodes\": {", now_seconds());
    for (int i = 0; i < n; i++) {
        sb_puts(&b, i ? ",\n    \"" : "\n    \"");
        json_escape(&b, h[i].name);
        sb_printf(&b, "\": {\"status\": \"%s\"", h[i].online ? "online" : "offline");
        sb_printf(&b, ", \"cpu_pct\": %d, \"ram_pct\": %d", h[i].cpu_pct, h[i].ram_pct);
        sb_printf(&b, ", \"ncpu\": %d, \"ram_mb\": %ld", h[i].ncpu, h[i].ram_mb);
        sb_printf(&b, ", \"load1\": %.2f, \"ms\": %.0f", h[i].load1, h[i].ms);
        sb_puts(&b, ", \"error\": \"");
        json_escape(&b, h[i].error);
        sb_puts(&b, "\"}");
    }
    sb_puts(&b, "\n  }\n}\n");

    fwrite(b.data, 1, b.len, f);
    fclose(f);
    sb_free(&b);
    free(path);
}
