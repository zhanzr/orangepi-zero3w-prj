/*
  coremark_multi.c - multi-core CoreMark verification for the Orange Pi
  Zero 3W (Allwinner A733: 6x Cortex-A55 = cpu0-5, little cluster,
  2x Cortex-A76 = cpu6-7, big cluster).

  Verifies the multi-core features of the chip by running independent,
  complete CoreMark instances pinned to each core. This is NOT a
  parallel/tailored CoreMark: every run is a standard, unmodified
  single-core CoreMark process - eight of them simply exercise the
  eight cores, verifying the multi-core features of the chip.

  Modes:
    --round-robin   run one instance per core, one core at a time;
                    each core gets its own score
    --parallel      run one instance on every core simultaneously;
                    each core gets its own score + combined throughput
    --loops N       repeat the selected phase(s) N times (default 2)
    --cpus 0,1,2    cores to test (default: all 8)
    --secs T        target seconds per run -> compile-time-matched
                    iterations (default 10)

  Workers are independent OS processes (this binary re-execs itself in
  --worker mode, pinned with sched_setaffinity), so each CoreMark has
  its own address space and globals - zero cross-contamination.

  Results: appended to results.csv in the working directory, plus a
  per-loop console summary.

  Frequency note: scores are only comparable at fixed clocks - run
  with the performance governor on all cpus (make run-perf) so the
  A55 cluster sits at 1794 MHz and the A76 cluster at 2002 MHz.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <sys/syscall.h>
#include <time.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sched.h>
#include <sys/syscall.h>

#define MAX_CPUS 8
#define OUT_FMT  "/dev/shm/cm%d.out"
/* rough single-core CoreMark/s for the fast cluster (A76 @ 2.0 GHz,
 * measured 15328 - see README); used only to size --secs iterations.
 * The slower A55 cluster (cpu0-5) then runs ~2.1x that long per round. */
#define EST_CM_PER_S 15000.0

static int  g_cpus[MAX_CPUS];
static int  g_ncpus;
static int  g_loops = 2;
static unsigned g_iters = 160000;    /* ~10.4 s on A76, ~22.3 s on A55 */
static char g_self[512];

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double cpu_temp_c(void)
{
    /* thermal_zone0 = cpub_thermal_zone (A76 big cluster) - the hotter
     * of the two SoC clusters under load here */
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    int v = -1000;

    if (f)
    {
        if (fscanf(f, "%d", &v) != 1)
            v = -1000;
        fclose(f);
    }
    return v / 1000.0;
}

/* avoid the glibc cpu_set_t macros (they break under -Dmain=...):
 * issue the raw sched_setaffinity syscall with a plain bitmask */
static void pin_to_cpu(int cpu)
{
    unsigned long mask = 1UL << cpu;

    if (syscall(SYS_sched_setaffinity, 0, sizeof(mask), &mask) != 0)
    {
        fprintf(stderr, "worker: sched_setaffinity(cpu %d): %s\n",
                cpu, strerror(errno));
        exit(3);
    }
}

/* ---- worker mode: pin this process, run one full CoreMark ---- */
static void worker_mode(int cpu)
{
    extern int coremark_entry(int argc, char **argv);
    char *argv[2] = { "coremark", NULL };

    pin_to_cpu(cpu);
    fflush(stdout);
    coremark_entry(1, argv);
    fflush(stdout);
}

/* ---- spawn one worker pinned to `cpu`, stdout -> outfile ---- */
static pid_t spawn_worker(int cpu, const char *outfile)
{
    pid_t pid = fork();

    if (pid < 0)
    {
        perror("fork");
        return -1;
    }
    if (pid == 0)
    {
        char cpu_str[16];
        FILE *out = fopen(outfile, "w");

        if (!out)
            _exit(4);
        dup2(fileno(out), STDOUT_FILENO);
        fclose(out);

        snprintf(cpu_str, sizeof(cpu_str), "%d", cpu);
        pin_to_cpu(cpu);
        execl(g_self, g_self, "--worker", "--cpu", cpu_str, (char *)NULL);
        _exit(5);
    }
    return pid;
}

/* ---- parse one worker output file ---- */
static int parse_result(const char *outfile, double *score, int *crc_ok,
                        unsigned long *iters)
{
    FILE *f = fopen(outfile, "r");
    char line[512];
    int found_score = 0, found_valid = 0, found_errors = 0;

    *score = 0.0;
    *crc_ok = 0;
    *iters = 0;
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f))
    {
        double s;
        unsigned long n;

        /* the linux port prints "Iterations/Sec : X" instead of the
         * standard "CoreMark 1.0 : X / Iterations : N" line */
        if (sscanf(line, "Iterations/Sec : %lf", &s) == 1)
        {
            *score = s;
            found_score = 1;
        }
        if (sscanf(line, "Iterations : %lu", &n) == 1 && !*iters)
            *iters = n;
        if (strstr(line, "crcfinal"))
            found_valid = 1;
        if (strstr(line, "Errors detected"))
            found_errors = 1;
        if (strstr(line, "Must execute for at least 10 secs"))
            found_errors = 1;
    }
    fclose(f);
    *crc_ok = (found_valid && !found_errors);
    return found_score ? 0 : -1;
}

/* ---- CSV recording ---- */
static void record_csv(const char *csv, const char *mode, int loop, int cpu,
                       unsigned long iters, double elapsed, double score,
                       int crc_ok)
{
    int first = (access(csv, F_OK) != 0);
    FILE *f = fopen(csv, "a");

    if (!f)
    {
        perror(csv);
        return;
    }
    if (first)
        fprintf(f, "unix_time,mode,loop,cpu,iterations,elapsed_s,"
                   "coremark_s,crc_valid\n");
    fprintf(f, "%ld,%s,%d,%d,%lu,%.3f,%.2f,%d\n",
            (long)time(NULL), mode, loop, cpu, iters, elapsed,
            score, crc_ok);
    fclose(f);
}

static double run_one(int cpu, const char *outfile)
{
    pid_t pid;
    double t0 = now_sec(), t1;
    int st;

    pid = spawn_worker(cpu, outfile);
    if (pid < 0)
        return -1;
    waitpid(pid, &st, 0);
    t1 = now_sec();
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
    {
        fprintf(stderr, "worker cpu%d exited abnormally (%d)\n", cpu, st);
        return -1;
    }
    return t1 - t0;
}

/* ---- round-robin phase ---- */
static void round_robin(int loops, const char *csv)
{
    int loop, c;

    printf("== round robin: one core at a time (%d loop(s)) ==\n", loops);
    for (loop = 1; loop <= loops; loop++)
    {
        printf("-- loop %d/%d --\n", loop, loops);
        for (c = 0; c < g_ncpus; c++)
        {
            char outfile[64];
            double elapsed, score = 0.0;
            int crc_ok = 0;
            unsigned long iters = 0;

            snprintf(outfile, sizeof(outfile), OUT_FMT, g_cpus[c]);
            elapsed = run_one(g_cpus[c], outfile);
            if (elapsed < 0)
                continue;
            if (parse_result(outfile, &score, &crc_ok, &iters) != 0)
            {
                fprintf(stderr, "cpu%d: no CoreMark result\n", g_cpus[c]);
                continue;
            }
            record_csv(csv, "rr", loop, g_cpus[c], iters, elapsed,
                       score, crc_ok);
            printf("  cpu%d: %.2f CoreMark/s (%s, %lu iters, %.2f s, "
                   "SoC %.1f C)\n",
                   g_cpus[c], score, crc_ok ? "CRC ok" : "CRC FAIL",
                   iters, elapsed, cpu_temp_c());
            unlink(outfile);
        }
    }
}

/* ---- parallel phase ---- */
static void parallel_run(int loops, const char *csv)
{
    int loop;

    printf("== parallel: all %d cores simultaneously (%d loop(s)) ==\n",
           g_ncpus, loops);
    for (loop = 1; loop <= loops; loop++)
    {
        pid_t pids[MAX_CPUS];
        char outfiles[MAX_CPUS][64];
        double scores[MAX_CPUS];
        int crc[MAX_CPUS];
        unsigned long it[MAX_CPUS];
        double t0, t1;
        int c, ok_count = 0;

        printf("-- loop %d/%d --\n", loop, loops);
        t0 = now_sec();
        for (c = 0; c < g_ncpus; c++)
        {
            snprintf(outfiles[c], sizeof(outfiles[c]), OUT_FMT, g_cpus[c]);
            unlink(outfiles[c]);
            pids[c] = spawn_worker(g_cpus[c], outfiles[c]);
        }
        for (c = 0; c < g_ncpus; c++)
            if (pids[c] > 0)
                waitpid(pids[c], NULL, 0);
        t1 = now_sec();

        for (c = 0; c < g_ncpus; c++)
        {
            scores[c] = 0.0;
            crc[c] = 0;
            it[c] = 0;
            if (pids[c] <= 0)
                continue;
            if (parse_result(outfiles[c], &scores[c], &crc[c],
                             &it[c]) != 0)
                continue;
            if (crc[c])
                ok_count++;
            record_csv(csv, "par", loop, g_cpus[c], it[c], t1 - t0,
                       scores[c], crc[c]);
            printf("  cpu%d: %.2f CoreMark/s (%s, %lu iters)\n",
                   g_cpus[c], scores[c], crc[c] ? "CRC ok" : "CRC FAIL",
                   it[c]);
            unlink(outfiles[c]);
        }
        printf("  wall time %.2f s | CRC ok %d/%d | SoC %.1f C\n",
               t1 - t0, ok_count, g_ncpus, cpu_temp_c());
    }
}

static void usage(void)
{
    fprintf(stderr,
            "usage: coremark_multi [--round-robin] [--parallel]\n"
            "                      [--loops N] [--cpus 0,1,2] [--secs T]\n"
            "       coremark_multi --worker --cpu N   (internal)\n");
}

int main(int argc, char **argv)
{
    char *cpus_arg = NULL;
    int rr = 0, par = 0, i;

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--worker") && i + 2 < argc
            && !strcmp(argv[i + 1], "--cpu"))
        {
            worker_mode(atoi(argv[i + 2]));
            return 0;
        }
        else if (!strcmp(argv[i], "--round-robin"))
            rr = 1;
        else if (!strcmp(argv[i], "--parallel"))
            par = 1;
        else if (!strcmp(argv[i], "--loops") && i + 1 < argc)
            g_loops = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cpus") && i + 1 < argc)
            cpus_arg = argv[++i];
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc)
        {
            double secs = atof(argv[++i]);

            if (secs > 1.0)
                g_iters = (unsigned)(secs * EST_CM_PER_S);
        }
        else
        {
            usage();
            return 1;
        }
    }
    if (!rr && !par)
    {
        rr = 1;
        par = 1;
    }

    g_ncpus = 0;
    if (cpus_arg)
    {
        char *tok = strtok(cpus_arg, ",");

        while (tok && g_ncpus < MAX_CPUS)
        {
            g_cpus[g_ncpus++] = atoi(tok);
            tok = strtok(NULL, ",");
        }
    }
    else
    {
        /* default: all 8 cores (cpu0-5 A55, cpu6-7 A76) */
        for (i = 0; i < 8; i++)
            g_cpus[i] = i;
        g_ncpus = 8;
    }

    {
        ssize_t n = readlink("/proc/self/exe", g_self, sizeof(g_self) - 1);

        if (n <= 0)
        {
            perror("readlink /proc/self/exe");
            return 1;
        }
        g_self[n] = 0;
    }

    printf("cores:");
    for (i = 0; i < g_ncpus; i++)
        printf(" %d", g_cpus[i]);
    printf(" | %u iterations/run (%.1f s target) | %d loop(s)\n",
           g_iters, (double)g_iters / EST_CM_PER_S, g_loops);

    if (rr)
        round_robin(g_loops, "results.csv");
    if (par)
        parallel_run(g_loops, "results.csv");
    return 0;
}
