#define _GNU_SOURCE
#include "sysinfo.h"

#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void gpu_init(si_gpu *g, const char *vendor, const char *source)
{
    memset(g, 0, sizeof *g);
    snprintf(g->vendor, sizeof g->vendor, "%s", vendor);
    snprintf(g->source, sizeof g->source, "%s", source);
    g->util = g->mem_used = g->mem_total = g->temp_c = g->power_w = g->freq_mhz = -1;
}

static void push(si_gpu **out, size_t *count, const si_gpu *g)
{
    *out = xrealloc(*out, (*count + 1) * sizeof **out);
    (*out)[(*count)++] = *g;
}

static double read_num(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    double v;
    int ok = fscanf(f, "%lf", &v) == 1;
    fclose(f);
    return ok ? v : -1;
}

static int read_str(const char *path, char *out, size_t len)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int ok = fgets(out, (int)len, f) != NULL;
    fclose(f);
    if (!ok)
        return -1;
    out[strcspn(out, "\n")] = '\0';
    return 0;
}

static double field(const char *s)
{
    while (*s == ' ')
        s++;
    if (!isdigit((unsigned char)*s))
        return -1; /* "[N/A]", "[Not Supported]" */
    return strtod(s, NULL);
}

/* NVIDIA has no utilization in sysfs; its own tool is the portable source. */
static int read_nvidia(si_gpu **out, size_t *count)
{
    if (access("/usr/bin/nvidia-smi", X_OK) != 0 && access("/bin/nvidia-smi", X_OK) != 0 &&
        access("/usr/local/bin/nvidia-smi", X_OK) != 0)
        return 0;
    FILE *p = popen("nvidia-smi --query-gpu=name,utilization.gpu,memory.used,memory.total,"
                    "temperature.gpu,power.draw,clocks.gr --format=csv,noheader,nounits 2>/dev/null",
                    "r");
    if (!p)
        return 0;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, p)) {
        char *cols[7] = { 0 };
        int nc = 0;
        char *save, *tok = strtok_r(line, ",\n", &save);
        while (tok && nc < 7) {
            cols[nc++] = tok;
            tok = strtok_r(NULL, ",\n", &save);
        }
        if (nc < 4)
            continue;
        si_gpu g;
        gpu_init(&g, "NVIDIA", "nvidia-smi");
        snprintf(g.name, sizeof g.name, "%s", cols[0]);
        g.util = field(cols[1]);
        double mu = field(cols[2]), mt = field(cols[3]);
        g.mem_used = mu >= 0 ? mu * 1024 * 1024 : -1;
        g.mem_total = mt >= 0 ? mt * 1024 * 1024 : -1;
        if (nc > 4) g.temp_c = field(cols[4]);
        if (nc > 5) g.power_w = field(cols[5]);
        if (nc > 6) g.freq_mhz = field(cols[6]);
        push(out, count, &g);
        n++;
    }
    pclose(p);
    return n;
}

/* Temperature from the first hwmon sensor of a DRM device, in °C. */
static double hwmon_temp(const char *devdir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/hwmon", devdir);
    DIR *d = opendir(path);
    if (!d)
        return -1;
    double t = -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && t < 0) {
        if (strncmp(e->d_name, "hwmon", 5) != 0)
            continue;
        char f[768];
        snprintf(f, sizeof f, "%s/%s/temp1_input", path, e->d_name);
        double v = read_num(f);
        if (v >= 0)
            t = v / 1000.0;
    }
    closedir(d);
    return t;
}

/* AMD and Intel GPUs via /sys/class/drm/cardN. */
static void read_drm(si_gpu **out, size_t *count, int skip_nvidia)
{
    DIR *d = opendir("/sys/class/drm");
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        /* cardN only, not connectors like card0-HDMI-A-1 */
        const char *nm = e->d_name;
        if (strncmp(nm, "card", 4) != 0 || !nm[4])
            continue;
        int digits = 1;
        for (const char *c = nm + 4; *c; c++)
            if (!isdigit((unsigned char)*c))
                digits = 0;
        if (!digits)
            continue;

        char dev[300], path[512], vendor[32] = "";
        snprintf(dev, sizeof dev, "/sys/class/drm/%s/device", nm);
        snprintf(path, sizeof path, "%s/vendor", dev);
        if (read_str(path, vendor, sizeof vendor) != 0)
            continue;

        si_gpu g;
        if (strcmp(vendor, "0x1002") == 0) {
            gpu_init(&g, "AMD", "sysfs");
            snprintf(path, sizeof path, "%s/gpu_busy_percent", dev);
            g.util = read_num(path);
            snprintf(path, sizeof path, "%s/mem_info_vram_used", dev);
            g.mem_used = read_num(path);
            snprintf(path, sizeof path, "%s/mem_info_vram_total", dev);
            g.mem_total = read_num(path);
            snprintf(path, sizeof path, "%s/product_name", dev);
            if (read_str(path, g.name, sizeof g.name) != 0 || !g.name[0])
                snprintf(g.name, sizeof g.name, "AMD Radeon (%s)", nm);
        } else if (strcmp(vendor, "0x8086") == 0) {
            gpu_init(&g, "Intel", "sysfs");
            snprintf(g.name, sizeof g.name, "Intel Graphics (%s)", nm);
            snprintf(path, sizeof path, "/sys/class/drm/%s/gt_act_freq_mhz", nm);
            g.freq_mhz = read_num(path);
            if (g.freq_mhz < 0) {
                snprintf(path, sizeof path, "/sys/class/drm/%s/gt_cur_freq_mhz", nm);
                g.freq_mhz = read_num(path);
            }
        } else if (strcmp(vendor, "0x10de") == 0) {
            if (skip_nvidia)
                continue;
            gpu_init(&g, "NVIDIA", "sysfs");
            snprintf(g.name, sizeof g.name, "NVIDIA GPU (%s, install nvidia-smi for usage)", nm);
        } else {
            gpu_init(&g, "other", "sysfs");
            snprintf(g.name, sizeof g.name, "GPU %s (vendor %s)", nm, vendor);
        }
        g.temp_c = hwmon_temp(dev);
        push(out, count, &g);
    }
    closedir(d);
}

int si_gpus_read(si_gpu **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    int nv = read_nvidia(out, count);
    read_drm(out, count, nv > 0);
    return 0;
}
