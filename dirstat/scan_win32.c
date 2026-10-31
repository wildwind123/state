/* Windows scanner. The POSIX one is scan.c. */
#include "dirstat.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

#include "compat.h"

#define REPORT_EVERY 4096

#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003L
#endif
#ifndef IO_REPARSE_TAG_SYMLINK
#define IO_REPARSE_TAG_SYMLINK 0xA000000CL
#endif

typedef struct {
    const ds_scan_opts *opts;
    uint64_t cluster;
    uint64_t items;
    uint64_t bytes;
    uint64_t errors;
    wchar_t *buf; /* path of the directory being scanned, grows as needed */
    size_t cap;
} scan_ctx;

static void ensure(scan_ctx *c, size_t n)
{
    if (n <= c->cap)
        return;
    c->cap = n * 2;
    c->buf = xrealloc(c->buf, c->cap * sizeof *c->buf);
}

/* Junctions and symlinks point elsewhere; following them would count data twice.
 * Other reparse points (OneDrive, dedup) are real content and are scanned. */
static int is_link(const WIN32_FIND_DATAW *fd)
{
    return (fd->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
           (fd->dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT || fd->dwReserved0 == IO_REPARSE_TAG_SYMLINK);
}

/* Bytes on disk: the size rounded up to whole clusters. Compressed, sparse and
 * cloud placeholder files ask the filesystem for the real allocation. c->buf holds the path. */
static uint64_t disk_size(scan_ctx *c, const WIN32_FIND_DATAW *fd)
{
    uint64_t size = ((uint64_t)fd->nFileSizeHigh << 32) | fd->nFileSizeLow;
    DWORD special = FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE |
                    FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE;
#ifdef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
    special |= FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
#endif
    if (fd->dwFileAttributes & special) {
        DWORD hi = 0;
        DWORD lo = GetCompressedFileSizeW(c->buf, &hi);
        if (lo != INVALID_FILE_SIZE || GetLastError() == NO_ERROR)
            size = ((uint64_t)hi << 32) | lo;
    }
    return (size + c->cluster - 1) / c->cluster * c->cluster;
}

static void report(scan_ctx *c)
{
    if (c->opts->progress && c->items % REPORT_EVERY == 0)
        c->opts->progress(c->items, c->bytes, c->opts->progress_ud);
}

/* c->buf[0..len) is the directory path, without a trailing backslash unless it is a drive root. */
static void scan_dir(scan_ctx *c, size_t len, ds_node *dir)
{
    size_t base = len;
    ensure(c, len + 3);
    if (c->buf[len - 1] != L'\\')
        c->buf[base++] = L'\\';
    c->buf[base] = L'*';
    c->buf[base + 1] = L'\0';

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(c->buf, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            dir->flags |= DS_F_ERROR;
            c->errors++;
        }
        return;
    }
    do {
        const wchar_t *nm = fd.cFileName;
        if (nm[0] == L'.' && (nm[1] == 0 || (nm[1] == L'.' && nm[2] == 0)))
            continue;
        size_t nl = wcslen(nm);
        ensure(c, base + nl + 3);
        memcpy(c->buf + base, nm, (nl + 1) * sizeof *nm);

        char *name = u8_from_wide(nm);
        ds_node *n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            n = ds_node_new(name, DS_DIR);
            if (is_link(&fd))
                n->flags |= DS_F_SKIPPED;
            else
                scan_dir(c, base + nl, n);
        } else if (is_link(&fd)) {
            n = ds_node_new(name, DS_OTHER);
        } else {
            n = ds_node_new(name, DS_FILE);
            n->size = disk_size(c, &fd);
            c->bytes += n->size;
        }
        free(name);
        ds_node_add(dir, n);
        dir->size += n->size;
        dir->items += n->items + 1;
        c->items++;
        report(c);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    ds_node_finish(dir);
}

/* "C:/x" -> "\\?\C:\x", "//srv/share/x" -> "\\?\UNC\srv\share\x" */
static wchar_t *long_path(const char *abs)
{
    wchar_t *w = wide_from_u8(abs);
    size_t n = wcslen(w);
    int unc = abs[0] == '/' && abs[1] == '/';
    const wchar_t *prefix = unc ? L"\\\\?\\UNC" : L"\\\\?\\";
    size_t pl = wcslen(prefix);
    wchar_t *out = xmalloc((pl + n + 1) * sizeof *out);
    memcpy(out, prefix, pl * sizeof *out);
    memcpy(out + pl, unc ? w + 1 : w, ((unc ? n - 1 : n) + 1) * sizeof *out);
    for (wchar_t *p = out; *p; p++)
        if (*p == L'/')
            *p = L'\\';
    free(w);
    return out;
}

static uint64_t cluster_size(const wchar_t *path)
{
    wchar_t vol[MAX_PATH + 1];
    DWORD spc, bps, nfree, ntotal;
    if (GetVolumePathNameW(path, vol, MAX_PATH + 1) && GetDiskFreeSpaceW(vol, &spc, &bps, &nfree, &ntotal) &&
        spc && bps)
        return (uint64_t)spc * bps;
    return 4096;
}

int ds_scan_path(const char *path, const ds_scan_opts *opts, ds_scan *out, char *err, size_t errlen)
{
    char abs[4096];
    memset(out, 0, sizeof *out);
    if (normalize_path(path, abs, sizeof abs) != 0) {
        snprintf(err, errlen, "%s: invalid path", path);
        return -1;
    }
    wchar_t *lp = long_path(abs);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(lp, GetFileExInfoStandard, &a)) {
        snprintf(err, errlen, "%s: not found or not accessible (error %lu)", abs, GetLastError());
        free(lp);
        return -1;
    }

    scan_ctx c = { .opts = opts, .cluster = cluster_size(lp) };
    uint64_t t0 = now_ms();
    out->scanned_at = (int64_t)time(NULL);
    ds_node *root;
    if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        root = ds_node_new(abs, DS_DIR);
        c.cap = wcslen(lp) + 1;
        c.buf = lp;
        lp = NULL;
        scan_dir(&c, wcslen(c.buf), root);
    } else {
        root = ds_node_new(abs, DS_FILE);
        WIN32_FIND_DATAW fd = { .dwFileAttributes = a.dwFileAttributes,
                                .nFileSizeHigh = a.nFileSizeHigh,
                                .nFileSizeLow = a.nFileSizeLow };
        c.buf = lp;
        lp = NULL;
        root->size = disk_size(&c, &fd);
    }
    free(c.buf);
    free(lp);
    if (opts->progress)
        opts->progress(c.items, c.bytes, opts->progress_ud);

    out->root = xstrdup(abs);
    out->tree = root;
    out->duration_ms = now_ms() - t0;
    out->errors = c.errors;
    return 0;
}
