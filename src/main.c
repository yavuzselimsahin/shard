/* shard — minimal distributed task runner.
 *
 * One binary: the CLI you type into, the SSH fan-out that runs the work, and
 * the web dashboard that shows what happened. Nothing is installed on the
 * machines you use; if you can `ssh` to them, shard can run work on them. */

#include "shard.h"

#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    printf(
"shard %s — run commands across the machines you already have\n"
"\n"
"Usage:\n"
"  shard cluster init                 create a cluster.toml here\n"
"  shard cluster add-node             add a machine, interactively\n"
"  shard cluster list                 show the machines in the cluster\n"
"  shard cluster health               check which machines answer\n"
"  shard cluster remove <name>        drop a machine from the file\n"
"\n"
"  shard exec \"<command>\" [--on X]     run one command on the machines\n"
"  shard exec-script <file> [--distribute]\n"
"                                     run a script, or share its lines out\n"
"  shard map \"<cmd {i}>\" --count <n>  run one command n times, spread out\n"
"  shard pipeline --step \"<cmd>\" …    steps in order, each on a machine\n"
"                 [--produces <file>] carry a step's files to the next one\n"
"\n"
"  shard tasks                        the jobs written down in tasks.toml\n"
"  shard run <task>                   run one of them\n"
"\n"
"  shard queue add -- <command>       put a job on the queue\n"
"  shard queue run                    run queued jobs, highest priority first\n"
"\n"
"  shard status                       what is running right now\n"
"  shard logs [<task-id>] [--node N]  output of a run (default: the last one)\n"
"  shard history [-n 20]              past runs\n"
"  shard ui [port]                    web dashboard (default: 8787)\n"
"  shard tui                          the same, in the terminal\n"
"\n"
"Options:\n"
"  --on <all|tag|node[,node]>  which machines to use (default: all)\n"
"  --jobs <n>                  how many to run at once (default: all of them)\n"
"  --timeout <seconds>         give up on a machine after this long\n"
"  --retry <n>                 try a failed job or item again, n more times\n"
"  --with <file>               ship this file to each worker first (repeatable)\n"
"  --balance                   measure live load, place work on the free machines\n"
"  --stream                    print output as it arrives, prefixed per node\n"
"  --dry-run                   show what would run, run nothing\n"
"  --config <file>             use this cluster file\n"
"  --tasks <file>              use this tasks file\n"
"\n"
"Examples:\n"
"  shard exec \"uname -a\" --on all\n"
"  shard exec \"make -j4\" --on build --stream\n"
"  shard exec-script tests.sh --distribute\n"
"  shard map \"python3 sim.py --seed {i}\" --count 1000 --with sim.py\n"
"  shard run build-all\n"
"  shard ui\n"
"\n"
"Configuration is a cluster.toml in the current directory, or\n"
"~/.shard/cluster.toml. Logs are kept in ~/.shard/logs.\n",
        SHARD_VERSION);
}

int main(int argc, char **argv)
{
    const char *config = NULL;

    /* Global options may appear anywhere; pull them out first. */
    int n = 0;
    char **args = xmalloc(sizeof(char *) * (size_t)argc);
    for (int i = 1; i < argc; i++) {
        if (str_eq(argv[i], "--config") && i + 1 < argc) { config = argv[++i]; continue; }
        /* --tasks is global too; the loader reads it back out of here. */
        if (str_eq(argv[i], "--tasks") && i + 1 < argc) {
            setenv("SHARD_TASKS", argv[++i], 1);
            continue;
        }
        args[n++] = argv[i];
    }

    if (n == 0 || str_eq(args[0], "help") || str_eq(args[0], "--help") ||
        str_eq(args[0], "-h")) {
        usage();
        free(args);
        return n == 0 ? 1 : 0;
    }

    if (str_eq(args[0], "version") || str_eq(args[0], "--version")) {
        printf("shard %s\n", SHARD_VERSION);
        free(args);
        return 0;
    }

    const char *cmd = args[0];
    int sub_argc = n - 1;
    char **sub_argv = args + 1;

    /* `cluster init` is the one command that runs without a cluster file. */
    int needs_cluster = !(str_eq(cmd, "cluster") && sub_argc > 0 &&
                          str_eq(sub_argv[0], "init"));

    cluster_t *c = needs_cluster ? cluster_load(config) : NULL;

    int rc = shard_dispatch(c, n, args);

    cluster_free(c);
    free(args);
    return rc;
}

/* Runs one command, argv[0] being its name. Factored out of main so the queue
 * runner can dispatch a stored command through exactly the same path. */
int shard_dispatch(cluster_t *c, int argc, char **argv)
{
    if (argc == 0) return 1;
    const char *cmd = argv[0];
    int sub_argc = argc - 1;
    char **sub_argv = argv + 1;

    if      (str_eq(cmd, "cluster"))     return cmd_cluster(c, sub_argc, sub_argv);
    else if (str_eq(cmd, "exec"))        return cmd_exec(c, sub_argc, sub_argv);
    else if (str_eq(cmd, "exec-script")) return cmd_exec_script(c, sub_argc, sub_argv);
    else if (str_eq(cmd, "map"))         return cmd_map(c, sub_argc, sub_argv);
    else if (str_eq(cmd, "pipeline"))    return cmd_pipeline(c, sub_argc, sub_argv);
    else if (str_eq(cmd, "run"))         return cmd_run(c, sub_argc, sub_argv);
    else if (str_eq(cmd, "tasks"))       return cmd_tasks(c, sub_argc, sub_argv);
    else if (str_eq(cmd, "queue"))       return cmd_queue(c, sub_argc, sub_argv);
    else if (str_eq(cmd, "status"))      return cmd_status(c, sub_argc, sub_argv);
    else if (str_eq(cmd, "logs"))        return cmd_logs(c, sub_argc, sub_argv);
    else if (str_eq(cmd, "history"))     return cmd_history(c, sub_argc, sub_argv);
    else if (str_eq(cmd, "ui"))          return cmd_ui(c, sub_argc, sub_argv);
    else if (str_eq(cmd, "tui"))         return cmd_tui(c, sub_argc, sub_argv);

    warn_msg("unknown command: %s", cmd);
    usage();
    return 1;
}
