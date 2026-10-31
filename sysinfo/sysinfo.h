#ifndef SYSINFO_H
#define SYSINFO_H

#include <stdint.h>
#include <stdio.h>

#include "util.h"

/* Raw jiffy counters from /proc/stat; usage needs two samples. */
typedef struct {
    uint64_t busy;
    uint64_t total;
} si_cpu_times;

typedef struct {
    int ncpu;
    si_cpu_times all;
    si_cpu_times *core; /* ncpu entries */
} si_cpu_sample;

typedef struct {
    char model[128];
    int ncpu;
    double usage;       /* percent, all cores */
    double *core_usage; /* ncpu entries, percent */
    double load[3];
    uint64_t uptime_s;
} si_cpu;

typedef struct {
    uint64_t total;
    uint64_t available;
    uint64_t used; /* total - available */
    uint64_t buffers;
    uint64_t cached;
    uint64_t swap_total;
    uint64_t swap_used;
} si_mem;

typedef struct {
    char *device;
    char *mount;
    char *fstype;
    uint64_t total;
    uint64_t used;
    uint64_t free; /* available to unprivileged users, like df */
} si_disk;

/* Unknown numeric fields are negative. */
typedef struct {
    char name[128];
    char vendor[16];
    char source[16]; /* where the numbers came from: nvidia-smi, sysfs */
    double util;     /* percent */
    double mem_used; /* bytes */
    double mem_total;
    double temp_c;
    double power_w;
    double freq_mhz;
} si_gpu;

typedef struct {
    char hostname[128];
    char kernel[128];
    int64_t time;
    si_cpu cpu;
    si_mem mem;
    si_disk *disks;
    size_t ndisks;
    si_gpu *gpus;
    size_t ngpus;
} si_snapshot;

/* cpu.c */
int si_cpu_sample_read(si_cpu_sample *s);
void si_cpu_sample_free(si_cpu_sample *s);
/* Fills cpu from two samples taken some time apart (plus model, load, uptime). */
void si_cpu_compute(const si_cpu_sample *prev, const si_cpu_sample *cur, si_cpu *cpu);
void si_cpu_free(si_cpu *cpu);
void si_cpu_copy(si_cpu *dst, const si_cpu *src);

/* mem.c */
int si_mem_read(si_mem *m);

/* disk.c */
int si_disks_read(si_disk **out, size_t *count);
void si_disks_free(si_disk *d, size_t count);

/* gpu.c */
int si_gpus_read(si_gpu **out, size_t *count);

/* snapshot.c: host info + memory + disks; cpu and gpus are filled by the caller. */
void si_snapshot_base(si_snapshot *s);
void si_snapshot_free(si_snapshot *s);
void si_snapshot_json(sbuf *b, const si_snapshot *s);
void si_snapshot_print(FILE *out, const si_snapshot *s);

#endif
