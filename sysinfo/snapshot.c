#define _GNU_SOURCE
#include "sysinfo.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void si_snapshot_base(si_snapshot *s)
{
    memset(s, 0, sizeof *s);
    si_host_info(s->hostname, sizeof s->hostname, s->kernel, sizeof s->kernel);
    s->time = (int64_t)time(NULL);
    si_mem_read(&s->mem);
    si_disks_read(&s->disks, &s->ndisks);
}

void si_snapshot_free(si_snapshot *s)
{
    si_cpu_free(&s->cpu);
    si_disks_free(s->disks, s->ndisks);
    free(s->gpus);
    memset(s, 0, sizeof *s);
}

/* ---------- JSON ---------- */

static void num_or_null(sbuf *b, const char *key, double v)
{
    if (v < 0)
        sb_printf(b, ",\"%s\":null", key);
    else
        sb_printf(b, ",\"%s\":%.1f", key, v);
}

void si_snapshot_json(sbuf *b, const si_snapshot *s)
{
    const si_cpu *c = &s->cpu;
    const si_mem *m = &s->mem;
    sb_puts(b, "{\"version\":\"" STAT_VERSION "\",\"hostname\":");
    sb_json_str(b, s->hostname);
    sb_puts(b, ",\"kernel\":");
    sb_json_str(b, s->kernel);
    sb_printf(b, ",\"time\":%" PRId64 ",\"uptime_s\":%" PRIu64, s->time, c->uptime_s);

    sb_puts(b, ",\"cpu\":{\"model\":");
    sb_json_str(b, c->model);
    sb_printf(b, ",\"cores\":%d,\"usage\":%.1f,\"load\":", c->ncpu, c->usage);
    if (c->has_load)
        sb_printf(b, "[%.2f,%.2f,%.2f]", c->load[0], c->load[1], c->load[2]);
    else
        sb_puts(b, "null");
    sb_puts(b, ",\"core_usage\":[");
    for (int i = 0; i < c->ncpu; i++)
        sb_printf(b, "%s%.1f", i ? "," : "", c->core_usage ? c->core_usage[i] : 0.0);
    sb_puts(b, "]}");

    sb_printf(b,
              ",\"memory\":{\"total\":%" PRIu64 ",\"available\":%" PRIu64 ",\"used\":%" PRIu64
              ",\"buffers\":%" PRIu64 ",\"cached\":%" PRIu64 ",\"swap_total\":%" PRIu64
              ",\"swap_used\":%" PRIu64 "}",
              m->total, m->available, m->used, m->buffers, m->cached, m->swap_total, m->swap_used);

    sb_puts(b, ",\"disks\":[");
    for (size_t i = 0; i < s->ndisks; i++) {
        const si_disk *d = &s->disks[i];
        sb_puts(b, i ? ",{\"device\":" : "{\"device\":");
        sb_json_str(b, d->device);
        sb_puts(b, ",\"mount\":");
        sb_json_str(b, d->mount);
        sb_puts(b, ",\"fstype\":");
        sb_json_str(b, d->fstype);
        sb_printf(b, ",\"total\":%" PRIu64 ",\"used\":%" PRIu64 ",\"free\":%" PRIu64 "}", d->total,
                  d->used, d->free);
    }
    sb_puts(b, "]");

    sb_puts(b, ",\"gpus\":[");
    for (size_t i = 0; i < s->ngpus; i++) {
        const si_gpu *g = &s->gpus[i];
        sb_puts(b, i ? ",{\"name\":" : "{\"name\":");
        sb_json_str(b, g->name);
        sb_puts(b, ",\"vendor\":");
        sb_json_str(b, g->vendor);
        sb_puts(b, ",\"source\":");
        sb_json_str(b, g->source);
        num_or_null(b, "util", g->util);
        num_or_null(b, "mem_used", g->mem_used);
        num_or_null(b, "mem_total", g->mem_total);
        num_or_null(b, "temp_c", g->temp_c);
        num_or_null(b, "power_w", g->power_w);
        num_or_null(b, "freq_mhz", g->freq_mhz);
        sb_puts(b, "}");
    }
    sb_puts(b, "]}");
}

/* ---------- terminal ---------- */

static void bar(FILE *out, double pct, int width)
{
    int fill = (int)(pct / 100.0 * width + 0.5);
    if (fill > width)
        fill = width;
    fputc('[', out);
    for (int i = 0; i < width; i++)
        fputc(i < fill ? '#' : ' ', out);
    fputc(']', out);
}

static double pct_of(uint64_t part, uint64_t whole)
{
    return whole ? 100.0 * (double)part / (double)whole : 0.0;
}

static void fmt_uptime(uint64_t s, char *out, size_t len)
{
    uint64_t d = s / 86400, h = s % 86400 / 3600, m = s % 3600 / 60;
    if (d)
        snprintf(out, len, "%" PRIu64 "d %" PRIu64 "h %" PRIu64 "m", d, h, m);
    else
        snprintf(out, len, "%" PRIu64 "h %" PRIu64 "m", h, m);
}

void si_snapshot_print(FILE *out, const si_snapshot *s)
{
    const si_cpu *c = &s->cpu;
    const si_mem *m = &s->mem;
    char a[32], b[32], t[32], when[64];

    fmt_uptime(c->uptime_s, a, sizeof a);
    fmt_time(s->time, when, sizeof when);
    fprintf(out, "%s · %s · up %s · %s\n\n", s->hostname, s->kernel, a, when);

    /* CPU */
    fprintf(out, "CPU      %s · %d cores", c->model[0] ? c->model : "unknown", c->ncpu);
    if (c->has_load)
        fprintf(out, " · load %.2f %.2f %.2f", c->load[0], c->load[1], c->load[2]);
    fputc('\n', out);
    fputs("         ", out);
    bar(out, c->usage, 30);
    fprintf(out, " %5.1f%%\n", c->usage);
    for (int i = 0; i < c->ncpu; i++) {
        if (i % 4 == 0)
            fputs("         ", out);
        fprintf(out, "%3d ", i);
        bar(out, c->core_usage[i], 10);
        fprintf(out, " %3.0f%%  ", c->core_usage[i]);
        if (i % 4 == 3 || i == c->ncpu - 1)
            fputc('\n', out);
    }

    /* Memory */
    double mp = pct_of(m->used, m->total);
    fmt_size(m->used, a, sizeof a);
    fmt_size(m->total, b, sizeof b);
    fmt_size(m->available, t, sizeof t);
    fputs("\nMemory   ", out);
    bar(out, mp, 30);
    fprintf(out, " %5.1f%%  %s used of %s · %s available\n", mp, a, b, t);
    if (m->swap_total) {
        double sp = pct_of(m->swap_used, m->swap_total);
        fmt_size(m->swap_used, a, sizeof a);
        fmt_size(m->swap_total, b, sizeof b);
        fputs("Swap     ", out);
        bar(out, sp, 30);
        fprintf(out, " %5.1f%%  %s used of %s\n", sp, a, b);
    } else {
        fputs("Swap     none\n", out);
    }

    /* Disks */
    fputs("\nDisks\n", out);
    int mw = 5;
    for (size_t i = 0; i < s->ndisks; i++) {
        int l = (int)strlen(s->disks[i].mount);
        if (l > mw)
            mw = l;
    }
    if (mw > 40)
        mw = 40;
    fprintf(out, "  %-*s  %-8s %10s %10s %10s  %-12s %5s\n", mw, "MOUNT", "TYPE", "SIZE", "USED",
            "FREE", "", "USE%");
    for (size_t i = 0; i < s->ndisks; i++) {
        const si_disk *d = &s->disks[i];
        /* like df: share of the space usable by normal users */
        double dp = pct_of(d->used, d->used + d->free);
        fmt_size(d->total, a, sizeof a);
        fmt_size(d->used, b, sizeof b);
        fmt_size(d->free, t, sizeof t);
        fprintf(out, "  %-*s  %-8s %10s %10s %10s  ", mw, d->mount, d->fstype, a, b, t);
        bar(out, dp, 10);
        fprintf(out, " %4.0f%%\n", dp);
    }
    if (!s->ndisks)
        fputs("  none found\n", out);

    /* GPU */
    fputs("\nGPU\n", out);
    if (!s->ngpus)
        fputs("  none detected\n", out);
    for (size_t i = 0; i < s->ngpus; i++) {
        const si_gpu *g = &s->gpus[i];
        fprintf(out, "  %s\n         ", g->name);
        if (g->util >= 0) {
            bar(out, g->util, 30);
            fprintf(out, " %5.1f%%", g->util);
        } else {
            fputs("usage not available", out);
        }
        if (g->mem_total > 0) {
            fmt_size((uint64_t)(g->mem_used > 0 ? g->mem_used : 0), a, sizeof a);
            fmt_size((uint64_t)g->mem_total, b, sizeof b);
            fprintf(out, "  memory %s of %s", a, b);
        }
        if (g->temp_c >= 0)
            fprintf(out, " · %.0f °C", g->temp_c);
        if (g->power_w >= 0)
            fprintf(out, " · %.0f W", g->power_w);
        if (g->freq_mhz >= 0)
            fprintf(out, " · %.0f MHz", g->freq_mhz);
        fputc('\n', out);
    }
}
