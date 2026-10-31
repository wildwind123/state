/*
 * Windows GPUs: adapters from DXGI, usage and memory from the "GPU Engine" and
 * "GPU Adapter Memory" performance counters (the same data Task Manager shows).
 * NVIDIA cards use nvidia-smi when it is installed, for temperature and power.
 */
#define COBJMACROS
#include "sysinfo.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <initguid.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>

#include "compat.h"

/* The query lives for the whole process: utilization needs two collections. */
static struct {
    int state; /* 0 = not tried, 1 = ready, -1 = unavailable */
    PDH_HQUERY query;
    PDH_HCOUNTER engine;
    PDH_HCOUNTER dedicated;
    PDH_HCOUNTER shared;
} g_pdh;

static void pdh_init(void)
{
    g_pdh.state = -1;
    if (PdhOpenQueryW(NULL, 0, &g_pdh.query) != ERROR_SUCCESS)
        return;
    /* English names work on every display language. */
    if (PdhAddEnglishCounterW(g_pdh.query, L"\\GPU Engine(*)\\Utilization Percentage", 0,
                              &g_pdh.engine) != ERROR_SUCCESS) {
        PdhCloseQuery(g_pdh.query);
        return;
    }
    PdhAddEnglishCounterW(g_pdh.query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &g_pdh.dedicated);
    PdhAddEnglishCounterW(g_pdh.query, L"\\GPU Adapter Memory(*)\\Shared Usage", 0, &g_pdh.shared);
    PdhCollectQueryData(g_pdh.query);
    Sleep(250); /* first rate needs a second sample */
    g_pdh.state = 1;
}

typedef struct {
    char *name; /* lowercased UTF-8 instance name */
    double value;
} counter_item;

static counter_item *counter_items(PDH_HCOUNTER c, DWORD *n)
{
    *n = 0;
    if (!c)
        return NULL;
    DWORD size = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, NULL) !=
        (PDH_STATUS)PDH_MORE_DATA)
        return NULL;
    PDH_FMT_COUNTERVALUE_ITEM_W *raw = xmalloc(size);
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, raw) !=
        ERROR_SUCCESS) {
        free(raw);
        return NULL;
    }
    counter_item *items = xcalloc(count ? count : 1, sizeof *items);
    for (DWORD i = 0; i < count; i++) {
        if (raw[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
            raw[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
            continue;
        char *u = u8_from_wide(raw[i].szName);
        for (char *p = u; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        items[*n].name = u;
        items[*n].value = raw[i].FmtValue.doubleValue;
        (*n)++;
    }
    free(raw);
    return items;
}

static void free_items(counter_item *items, DWORD n)
{
    for (DWORD i = 0; i < n; i++)
        free(items[i].name);
    free(items);
}

/* Instance names contain "luid_0xHIGH_0xLOW" for the adapter they belong to. */
static void luid_key(LUID l, char *out, size_t len)
{
    snprintf(out, len, "luid_0x%08lx_0x%08lx", (unsigned long)l.HighPart, (unsigned long)l.LowPart);
}

/* Like Task Manager: add up the engines of each type, report the busiest type. */
static double adapter_util(const char *key, const counter_item *items, DWORD n)
{
    struct {
        const char *type;
        size_t len;
        double sum;
    } types[32];
    int nt = 0, found = 0;
    for (DWORD i = 0; i < n; i++) {
        if (!strstr(items[i].name, key))
            continue;
        found = 1;
        const char *t = strstr(items[i].name, "engtype_");
        t = t ? t + 8 : "";
        size_t tl = strlen(t);
        int k = 0;
        while (k < nt && !(types[k].len == tl && memcmp(types[k].type, t, tl) == 0))
            k++;
        if (k == nt) {
            if (nt == 32)
                continue;
            types[nt].type = t;
            types[nt].len = tl;
            types[nt].sum = 0;
            nt++;
        }
        types[k].sum += items[i].value;
    }
    if (!found)
        return -1;
    double best = 0;
    for (int k = 0; k < nt; k++)
        if (types[k].sum > best)
            best = types[k].sum;
    return best > 100 ? 100 : best;
}

static double adapter_sum(const char *key, const counter_item *items, DWORD n)
{
    double sum = 0;
    int found = 0;
    for (DWORD i = 0; i < n; i++) {
        if (strstr(items[i].name, key)) {
            sum += items[i].value;
            found = 1;
        }
    }
    return found ? sum : -1;
}

static const char *vendor_name(UINT id)
{
    switch (id) {
    case 0x10DE: return "NVIDIA";
    case 0x1002: return "AMD";
    case 0x8086: return "Intel";
    case 0x5143: return "Qualcomm";
    default: return "other";
    }
}

int si_gpus_read(si_gpu **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    int nvidia = si_nvidia_read(out, count);

    if (g_pdh.state == 0)
        pdh_init();
    DWORD ne = 0, nd = 0, ns = 0;
    counter_item *eng = NULL, *ded = NULL, *sha = NULL;
    if (g_pdh.state == 1 && PdhCollectQueryData(g_pdh.query) == ERROR_SUCCESS) {
        eng = counter_items(g_pdh.engine, &ne);
        ded = counter_items(g_pdh.dedicated, &nd);
        sha = counter_items(g_pdh.shared, &ns);
    }

    IDXGIFactory1 *factory = NULL;
    if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) {
        IDXGIAdapter1 *a = NULL;
        for (UINT i = 0; IDXGIFactory1_EnumAdapters1(factory, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d;
            int skip = FAILED(IDXGIAdapter1_GetDesc1(a, &d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
                       (nvidia && d.VendorId == 0x10DE);
            IDXGIAdapter1_Release(a);
            if (skip)
                continue;
            si_gpu g;
            si_gpu_init(&g, vendor_name(d.VendorId), "dxgi");
            char *name = u8_from_wide(d.Description);
            snprintf(g.name, sizeof g.name, "%s", name);
            free(name);

            char key[64];
            luid_key(d.AdapterLuid, key, sizeof key);
            if (ne)
                g.util = adapter_util(key, eng, ne);
            /* Integrated GPUs have little dedicated memory and use shared system memory. */
            int integrated = d.DedicatedVideoMemory < (SIZE_T)512 * 1024 * 1024;
            g.mem_total = (double)(integrated ? d.SharedSystemMemory : d.DedicatedVideoMemory);
            g.mem_used = integrated ? adapter_sum(key, sha, ns) : adapter_sum(key, ded, nd);
            if (g.util >= 0)
                snprintf(g.source, sizeof g.source, "dxgi+pdh");
            si_gpu_push(out, count, &g);
        }
        IDXGIFactory1_Release(factory);
    }
    free_items(eng, ne);
    free_items(ded, nd);
    free_items(sha, ns);
    return 0;
}
