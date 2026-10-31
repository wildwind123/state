#define _GNU_SOURCE
#include "sysinfo.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

void si_cpu_sample_free(si_cpu_sample *s)
{
    free(s->core);
    memset(s, 0, sizeof *s);
}

double si_cpu_usage(const si_cpu_times *a, const si_cpu_times *b)
{
    if (b->total <= a->total || b->busy < a->busy)
        return 0.0;
    double u = 100.0 * (double)(b->busy - a->busy) / (double)(b->total - a->total);
    return u > 100.0 ? 100.0 : u;
}

void si_cpu_free(si_cpu *cpu)
{
    free(cpu->core_usage);
    cpu->core_usage = NULL;
}

void si_cpu_copy(si_cpu *dst, const si_cpu *src)
{
    *dst = *src;
    dst->core_usage = xcalloc((size_t)(src->ncpu ? src->ncpu : 1), sizeof *dst->core_usage);
    if (src->core_usage)
        memcpy(dst->core_usage, src->core_usage, (size_t)src->ncpu * sizeof *dst->core_usage);
}

void si_disks_free(si_disk *d, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(d[i].device);
        free(d[i].mount);
        free(d[i].fstype);
    }
    free(d);
}

void si_gpu_init(si_gpu *g, const char *vendor, const char *source)
{
    memset(g, 0, sizeof *g);
    snprintf(g->vendor, sizeof g->vendor, "%s", vendor);
    snprintf(g->source, sizeof g->source, "%s", source);
    g->util = g->mem_used = g->mem_total = g->temp_c = g->power_w = g->freq_mhz = -1;
}

void si_gpu_push(si_gpu **out, size_t *count, const si_gpu *g)
{
    *out = xrealloc(*out, (*count + 1) * sizeof **out);
    (*out)[(*count)++] = *g;
}

static double field(const char *s)
{
    while (*s == ' ')
        s++;
    if (!isdigit((unsigned char)*s))
        return -1; /* "[N/A]", "[Not Supported]" */
    return strtod(s, NULL);
}

static int have_nvidia_smi(void)
{
#ifdef _WIN32
    wchar_t found[MAX_PATH];
    return SearchPathW(NULL, L"nvidia-smi.exe", NULL, MAX_PATH, found, NULL) > 0;
#else
    return access("/usr/bin/nvidia-smi", X_OK) == 0 || access("/bin/nvidia-smi", X_OK) == 0 ||
           access("/usr/local/bin/nvidia-smi", X_OK) == 0;
#endif
}

/* NVIDIA's own tool is the one source that works the same on Linux and Windows. */
int si_nvidia_read(si_gpu **out, size_t *count)
{
    if (!have_nvidia_smi())
        return 0;
#ifdef _WIN32
#define NULLDEV "nul"
#else
#define NULLDEV "/dev/null"
#endif
    FILE *p = popen("nvidia-smi --query-gpu=name,utilization.gpu,memory.used,memory.total,"
                    "temperature.gpu,power.draw,clocks.gr --format=csv,noheader,nounits 2>" NULLDEV,
                    "r");
    if (!p)
        return 0;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, p)) {
        char *cols[7] = { 0 };
        int nc = 0;
        char *save, *tok = strtok_r(line, ",\r\n", &save);
        while (tok && nc < 7) {
            cols[nc++] = tok;
            tok = strtok_r(NULL, ",\r\n", &save);
        }
        if (nc < 4)
            continue;
        si_gpu g;
        si_gpu_init(&g, "NVIDIA", "nvidia-smi");
        snprintf(g.name, sizeof g.name, "%s", cols[0]);
        g.util = field(cols[1]);
        double mu = field(cols[2]), mt = field(cols[3]);
        g.mem_used = mu >= 0 ? mu * 1024 * 1024 : -1;
        g.mem_total = mt >= 0 ? mt * 1024 * 1024 : -1;
        if (nc > 4) g.temp_c = field(cols[4]);
        if (nc > 5) g.power_w = field(cols[5]);
        if (nc > 6) g.freq_mhz = field(cols[6]);
        si_gpu_push(out, count, &g);
        n++;
    }
    pclose(p);
    return n;
}
