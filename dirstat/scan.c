#define _GNU_SOURCE
#include "dirstat.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

#define REPORT_EVERY 4096

typedef struct {
    dev_t dev;
    ino_t ino;
    int used;
} ino_key;

typedef struct {
    const ds_scan_opts *opts;
    ino_key *seen; /* hard-linked files already counted */
    size_t seen_cap;
    size_t seen_len;
    uint64_t items;
    uint64_t bytes;
    uint64_t errors;
} scan_ctx;

static size_t hash_key(dev_t dev, ino_t ino)
{
    uint64_t h = (uint64_t)ino * 0x9E3779B97F4A7C15ull ^ (uint64_t)dev * 0xC2B2AE3D27D4EB4Full;
    return (size_t)(h ^ (h >> 31));
}

static void seen_insert(ino_key *tab, size_t cap, dev_t dev, ino_t ino)
{
    size_t i = hash_key(dev, ino) & (cap - 1);
    while (tab[i].used)
        i = (i + 1) & (cap - 1);
    tab[i].dev = dev;
    tab[i].ino = ino;
    tab[i].used = 1;
}

/* Returns 1 if (dev, ino) was seen before, otherwise records it. */
static int seen_before(scan_ctx *c, dev_t dev, ino_t ino)
{
    if (c->seen_cap) {
        size_t i = hash_key(dev, ino) & (c->seen_cap - 1);
        while (c->seen[i].used) {
            if (c->seen[i].dev == dev && c->seen[i].ino == ino)
                return 1;
            i = (i + 1) & (c->seen_cap - 1);
        }
    }
    if ((c->seen_len + 1) * 2 > c->seen_cap) {
        size_t cap = c->seen_cap ? c->seen_cap * 2 : 1024;
        ino_key *tab = xcalloc(cap, sizeof *tab);
        for (size_t i = 0; i < c->seen_cap; i++)
            if (c->seen[i].used)
                seen_insert(tab, cap, c->seen[i].dev, c->seen[i].ino);
        free(c->seen);
        c->seen = tab;
        c->seen_cap = cap;
    }
    seen_insert(c->seen, c->seen_cap, dev, ino);
    c->seen_len++;
    return 0;
}

/* Pseudo filesystems whose sizes are meaningless for disk usage. */
static int is_virtual_fs(int fd)
{
    static const unsigned long magic[] = {
        0x9fa0,     /* proc */
        0x62656572, /* sysfs */
        0x1cd1,     /* devpts */
        0x27e0eb,   /* cgroup */
        0x63677270, /* cgroup2 */
        0x64626720, /* debugfs */
        0x73636673, /* securityfs */
        0x74726163, /* tracefs */
        0x6165676c, /* pstore */
        0xcafe4a11, /* bpf */
        0x62656570, /* configfs */
        0x65735543, /* fusectl */
        0x19800202, /* mqueue */
        0x42494e4d, /* binfmt_misc */
        0x958458f6, /* hugetlbfs */
        0x0187,     /* autofs */
    };
    struct statfs sf;
    if (fstatfs(fd, &sf) != 0)
        return 0;
    for (size_t i = 0; i < sizeof magic / sizeof magic[0]; i++)
        if ((unsigned long)sf.f_type == magic[i])
            return 1;
    return 0;
}

static ds_node *node_new(const char *name, uint8_t type)
{
    ds_node *n = xcalloc(1, sizeof *n);
    n->name = xstrdup(name);
    n->type = type;
    return n;
}

static void node_add(ds_node *p, ds_node *c)
{
    if (p->nchildren == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 4;
        p->children = xrealloc(p->children, p->cap * sizeof *p->children);
    }
    p->children[p->nchildren++] = c;
}

static int cmp_size_desc(const void *a, const void *b)
{
    const ds_node *x = *(const ds_node *const *)a;
    const ds_node *y = *(const ds_node *const *)b;
    if (x->size != y->size)
        return x->size < y->size ? 1 : -1;
    return strcmp(x->name, y->name);
}

static void report(scan_ctx *c)
{
    if (c->opts->progress && c->items % REPORT_EVERY == 0)
        c->opts->progress(c->items, c->bytes, c->opts->progress_ud);
}

/* Scans the directory open as fd (takes ownership of fd) into dir. */
static void scan_dir(scan_ctx *c, int fd, ds_node *dir, dev_t dev)
{
    DIR *d = fdopendir(fd);
    if (!d) {
        close(fd);
        dir->flags |= DS_F_ERROR;
        c->errors++;
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
            continue;
        struct stat st;
        if (fstatat(fd, nm, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            c->errors++;
            continue;
        }
        uint64_t sz = (uint64_t)st.st_blocks * 512;
        ds_node *n;
        if (S_ISDIR(st.st_mode)) {
            n = node_new(nm, DS_DIR);
            n->size = sz;
            c->bytes += sz;
            if (st.st_dev != dev && c->opts->one_fs) {
                n->flags |= DS_F_SKIPPED;
            } else {
                int cfd = openat(fd, nm, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (cfd < 0) {
                    n->flags |= DS_F_ERROR;
                    c->errors++;
                } else if (st.st_dev != dev && is_virtual_fs(cfd)) {
                    close(cfd);
                    n->flags |= DS_F_SKIPPED;
                } else {
                    scan_dir(c, cfd, n, st.st_dev);
                }
            }
        } else {
            /* Count hard-linked files once, like du. */
            if (st.st_nlink > 1 && seen_before(c, st.st_dev, st.st_ino))
                continue;
            n = node_new(nm, S_ISREG(st.st_mode) ? DS_FILE : DS_OTHER);
            n->size = sz;
            c->bytes += sz;
        }
        node_add(dir, n);
        dir->size += n->size;
        dir->items += n->items + 1;
        c->items++;
        report(c);
    }
    closedir(d);

    if (dir->nchildren > 1)
        qsort(dir->children, dir->nchildren, sizeof *dir->children, cmp_size_desc);
    if (dir->cap > dir->nchildren) {
        dir->children = xrealloc(dir->children, dir->nchildren * sizeof *dir->children);
        dir->cap = dir->nchildren;
    }
}

int ds_scan_path(const char *path, const ds_scan_opts *opts, ds_scan *out, char *err, size_t errlen)
{
    char abs[PATH_MAX];
    struct stat st;
    memset(out, 0, sizeof *out);
    if (normalize_path(path, abs, sizeof abs) != 0 || lstat(abs, &st) != 0) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }

    scan_ctx c = { .opts = opts };
    uint64_t t0 = now_ms();
    uint8_t type = S_ISDIR(st.st_mode) ? DS_DIR : S_ISREG(st.st_mode) ? DS_FILE : DS_OTHER;
    ds_node *root = node_new(abs, type);
    root->size = (uint64_t)st.st_blocks * 512;
    out->scanned_at = (int64_t)time(NULL);

    if (type == DS_DIR) {
        int fd = open(abs, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            snprintf(err, errlen, "%s: %s", abs, strerror(errno));
            ds_node_free(root);
            return -1;
        }
        scan_dir(&c, fd, root, st.st_dev);
    }
    free(c.seen);
    if (opts->progress)
        opts->progress(c.items, c.bytes, opts->progress_ud);

    out->root = xstrdup(abs);
    out->tree = root;
    out->duration_ms = now_ms() - t0;
    out->errors = c.errors;
    return 0;
}

void ds_node_free(ds_node *n)
{
    if (!n)
        return;
    for (uint32_t i = 0; i < n->nchildren; i++)
        ds_node_free(n->children[i]);
    free(n->children);
    free(n->name);
    free(n);
}

void ds_scan_free(ds_scan *s)
{
    ds_node_free(s->tree);
    free(s->root);
    memset(s, 0, sizeof *s);
}

int ds_path_covers(const char *root, const char *path)
{
    if (strcmp(root, "/") == 0)
        return path[0] == '/';
    size_t rl = strlen(root);
    return strncmp(root, path, rl) == 0 && (path[rl] == '\0' || path[rl] == '/');
}

ds_node *ds_find(const ds_scan *s, const char *path)
{
    if (!ds_path_covers(s->root, path))
        return NULL;
    ds_node *n = s->tree;
    const char *p = path + strlen(s->root);
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - start);
        ds_node *next = NULL;
        for (uint32_t i = 0; i < n->nchildren; i++) {
            ds_node *c = n->children[i];
            if (strlen(c->name) == len && memcmp(c->name, start, len) == 0) {
                next = c;
                break;
            }
        }
        if (!next)
            return NULL;
        n = next;
    }
    return n;
}
