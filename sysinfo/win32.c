/* Windows: host, CPU, memory and drives. GPUs are in gpu_win32.c. */
#include "sysinfo.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winternl.h>
#include <psapi.h>

#include "compat.h"

/* ---------- host ---------- */

void si_host_info(char *host, size_t hostlen, char *kernel, size_t kernellen)
{
    wchar_t w[256];
    DWORD n = 256;
    if (GetComputerNameExW(ComputerNameDnsHostname, w, &n)) {
        char *u = u8_from_wide(w);
        snprintf(host, hostlen, "%s", u);
        free(u);
    } else {
        snprintf(host, hostlen, "unknown");
    }

    /* GetVersionEx reports 6.2 to unmanifested programs; RtlGetVersion tells the truth. */
    typedef LONG(WINAPI * rtl_get_version_fn)(OSVERSIONINFOW *);
    rtl_get_version_fn rtl_get_version =
        (rtl_get_version_fn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW vi = { .dwOSVersionInfoSize = sizeof vi };
    if (rtl_get_version && rtl_get_version(&vi) == 0)
        snprintf(kernel, kernellen, "Windows %lu.%lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion,
                 vi.dwBuildNumber);
    else
        snprintf(kernel, kernellen, "Windows");
}

/* ---------- CPU ---------- */

typedef struct {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime; /* includes idle time */
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG InterruptCount;
} proc_perf_info;

typedef LONG(WINAPI *nt_query_fn)(int, void *, ULONG, ULONG *);
#define SystemProcessorPerformanceInformation 8
#define SystemPageFileInformation 18

static nt_query_fn nt_query(void)
{
    static nt_query_fn fn;
    if (!fn)
        fn = (nt_query_fn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                 "NtQuerySystemInformation");
    return fn;
}

static uint64_t ft64(FILETIME f)
{
    return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

int si_cpu_sample_read(si_cpu_sample *s)
{
    memset(s, 0, sizeof *s);
    FILETIME idle, kern, user;
    if (!GetSystemTimes(&idle, &kern, &user))
        return -1;
    s->all.total = ft64(kern) + ft64(user);
    s->all.busy = s->all.total - ft64(idle);

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    ULONG n = si.dwNumberOfProcessors;
    nt_query_fn q = nt_query();
    if (!q || n == 0)
        return 0;
    proc_perf_info *p = xcalloc(n, sizeof *p);
    ULONG ret = 0;
    if (q(SystemProcessorPerformanceInformation, p, n * (ULONG)sizeof *p, &ret) == 0) {
        s->ncpu = (int)(ret / sizeof *p);
        s->core = xcalloc((size_t)(s->ncpu ? s->ncpu : 1), sizeof *s->core);
        for (int i = 0; i < s->ncpu; i++) {
            uint64_t total = (uint64_t)p[i].KernelTime.QuadPart + (uint64_t)p[i].UserTime.QuadPart;
            s->core[i].total = total;
            s->core[i].busy = total - (uint64_t)p[i].IdleTime.QuadPart;
        }
    }
    free(p);
    return 0;
}

static void read_model(char *out, size_t len)
{
    wchar_t buf[256];
    DWORD size = sizeof buf;
    out[0] = '\0';
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString", RRF_RT_REG_SZ, NULL, buf, &size) != ERROR_SUCCESS)
        return;
    char *u = u8_from_wide(buf);
    char *p = u;
    while (*p == ' ')
        p++;
    snprintf(out, len, "%s", p);
    free(u);
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
    cpu->has_load = 0;
    cpu->uptime_s = GetTickCount64() / 1000;
}

/* ---------- memory ---------- */

typedef struct {
    ULONG NextEntryOffset;
    ULONG TotalSize; /* pages */
    ULONG TotalInUse;
    ULONG PeakUsage;
    UNICODE_STRING PageFileName;
} pagefile_info;

int si_mem_read(si_mem *m)
{
    memset(m, 0, sizeof *m);
    MEMORYSTATUSEX ms = { .dwLength = sizeof ms };
    if (!GlobalMemoryStatusEx(&ms))
        return -1;
    m->total = ms.ullTotalPhys;
    m->available = ms.ullAvailPhys;
    m->used = m->total - m->available;

    PERFORMANCE_INFORMATION pi = { .cb = sizeof pi };
    if (GetPerformanceInfo(&pi, sizeof pi))
        m->cached = (uint64_t)pi.SystemCache * pi.PageSize;

    /* Page files are the Windows equivalent of swap. */
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    unsigned char buf[8192];
    ULONG ret = 0;
    nt_query_fn q = nt_query();
    if (q && q(SystemPageFileInformation, buf, sizeof buf, &ret) == 0 && ret) {
        unsigned char *p = buf;
        for (;;) {
            pagefile_info *f = (pagefile_info *)p;
            m->swap_total += (uint64_t)f->TotalSize * si.dwPageSize;
            m->swap_used += (uint64_t)f->TotalInUse * si.dwPageSize;
            if (!f->NextEntryOffset || p + f->NextEntryOffset >= buf + ret)
                break;
            p += f->NextEntryOffset;
        }
    }
    return 0;
}

/* ---------- drives ---------- */

static const char *drive_kind(UINT type)
{
    switch (type) {
    case DRIVE_FIXED: return "Local disk";
    case DRIVE_REMOVABLE: return "Removable";
    case DRIVE_REMOTE: return "Network";
    case DRIVE_CDROM: return "CD/DVD";
    case DRIVE_RAMDISK: return "RAM disk";
    default: return "Drive";
    }
}

int si_disks_read(si_disk **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    wchar_t drives[1024];
    DWORD n = GetLogicalDriveStringsW(1023, drives);
    if (n == 0 || n > 1023)
        return -1;
    /* No "insert a disk" dialogs for empty card readers. */
    UINT old = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    size_t cap = 0;
    for (wchar_t *d = drives; *d; d += wcslen(d) + 1) {
        UINT type = GetDriveTypeW(d);
        ULARGE_INTEGER avail, total, tfree;
        if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN ||
            !GetDiskFreeSpaceExW(d, &avail, &total, &tfree) || total.QuadPart == 0)
            continue;
        wchar_t label[MAX_PATH + 1] = L"", fs[MAX_PATH + 1] = L"";
        GetVolumeInformationW(d, label, MAX_PATH + 1, NULL, NULL, NULL, fs, MAX_PATH + 1);

        if (*count == cap) {
            cap = cap ? cap * 2 : 8;
            *out = xrealloc(*out, cap * sizeof **out);
        }
        si_disk *disk = &(*out)[(*count)++];
        disk->mount = u8_from_wide(d);
        for (char *c = disk->mount; *c; c++)
            if (*c == '\\')
                *c = '/';
        disk->device = label[0] ? u8_from_wide(label) : xstrdup(drive_kind(type));
        disk->fstype = fs[0] ? u8_from_wide(fs) : xstrdup("?");
        disk->total = total.QuadPart;
        disk->used = total.QuadPart - tfree.QuadPart;
        disk->free = avail.QuadPart;
    }
    SetErrorMode(old);
    return 0;
}
