#define _GNU_SOURCE
/* Linux: /proc/stat, /proc/cpuinfo, /proc/loadavg */
#include "sysinfo.h"

#include <stdlib.h>
#include <string.h>

static int parse_cpu_line(const char *line, si_cpu_times *t)
{
    unsigned long long v[10] = { 0 };
    const char *p = strchr(line, ' ');
    if (!p)
        return -1;
    int n = sscanf(p, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2],
                   &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    if (n < 4)
        return -1;
    /* user nice system idle iowait irq softirq steal; guest time is already in user. */
    uint64_t idle = v[3] + v[4];
    uint64_t total = 0;
    for (int i = 0; i < 8 && i < n; i++)
        total += v[i];
    t->total = total;
    t->busy = total - idle;
    return 0;
}

int si_cpu_sample_read(si_cpu_sample *s)
{
    memset(s, 0, sizeof *s);
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return -1;
    char line[512];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "cpu", 3) != 0)
            break;
        if (line[3] == ' ') {
            parse_cpu_line(line, &s->all);
            continue;
        }
        if (s->ncpu == cap) {
            cap = cap ? cap * 2 : 16;
            s->core = xrealloc(s->core, (size_t)cap * sizeof *s->core);
        }
        if (parse_cpu_line(line, &s->core[s->ncpu]) == 0)
            s->ncpu++;
    }
    fclose(f);
    return s->all.total ? 0 : -1;
}

static void read_model(char *out, size_t len)
{
    out[0] = '\0';
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        /* x86: "model name", ARM: "Model" or "Hardware" */
        if (strncmp(line, "model name", 10) == 0 || strncmp(line, "Model", 5) == 0 ||
            strncmp(line, "Hardware", 8) == 0) {
            char *v = strchr(line, ':');
            if (!v)
                continue;
            v++;
            while (*v == ' ' || *v == '\t')
                v++;
            v[strcspn(v, "\n")] = '\0';
            snprintf(out, len, "%s", v);
            if (strncmp(line, "model name", 10) == 0)
                break;
        }
    }
    fclose(f);
}

void si_cpu_compute(const si_cpu_sample *prev, const si_cpu_sample *cur, si_cpu *cpu)
{
    memset(cpu, 0, sizeof *cpu);
    read_model(cpu->model, sizeof cpu->model);
    cpu->ncpu = cur->ncpu;
    cpu->usage = si_cpu_usage(&prev->all, &cur->all);
    cpu->core_usage = xcalloc((size_t)(cur->ncpu ? cur->ncpu : 1), sizeof *cpu->core_usage);
    for (int i = 0; i < cur->ncpu && i < prev->ncpu; i++)
        cpu->core_usage[i] = si_cpu_usage(&prev->core[i], &cur->core[i]);

    FILE *f = fopen("/proc/loadavg", "r");
    if (f) {
        cpu->has_load = fscanf(f, "%lf %lf %lf", &cpu->load[0], &cpu->load[1], &cpu->load[2]) == 3;
        fclose(f);
    }
    f = fopen("/proc/uptime", "r");
    if (f) {
        double up;
        if (fscanf(f, "%lf", &up) == 1)
            cpu->uptime_s = (uint64_t)up;
        fclose(f);
    }
}
