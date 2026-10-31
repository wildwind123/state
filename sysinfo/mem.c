#include "sysinfo.h"

#include <string.h>

int si_mem_read(si_mem *m)
{
    memset(m, 0, sizeof *m);
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return -1;
    char key[64];
    unsigned long long kb;
    uint64_t swap_free = 0;
    int have_avail = 0;
    uint64_t mfree = 0;
    while (fscanf(f, "%63s %llu kB\n", key, &kb) == 2) {
        uint64_t v = (uint64_t)kb * 1024;
        if (strcmp(key, "MemTotal:") == 0) m->total = v;
        else if (strcmp(key, "MemAvailable:") == 0) { m->available = v; have_avail = 1; }
        else if (strcmp(key, "MemFree:") == 0) mfree = v;
        else if (strcmp(key, "Buffers:") == 0) m->buffers = v;
        else if (strcmp(key, "Cached:") == 0) m->cached = v;
        else if (strcmp(key, "SwapTotal:") == 0) m->swap_total = v;
        else if (strcmp(key, "SwapFree:") == 0) swap_free = v;
    }
    fclose(f);
    if (!have_avail) /* kernels before 3.14 */
        m->available = mfree + m->buffers + m->cached;
    if (m->available > m->total)
        m->available = m->total;
    m->used = m->total - m->available;
    m->swap_used = m->swap_total > swap_free ? m->swap_total - swap_free : 0;
    return m->total ? 0 : -1;
}
