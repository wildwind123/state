#define _GNU_SOURCE
#include "dirstat.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Cache file layout (host byte order):
 *   header: magic[8] i64 scanned_at u64 duration_ms u64 errors u32 rootlen root[rootlen]
 *   nodes, pre-order: u8 type u8 flags u16 namelen u64 size u64 items u32 nchildren name[namelen]
 */
static const char MAGIC[8] = { 'D', 'S', 'T', 'A', 'T', 0, 0, 1 };
#define MAX_CHILDREN (1u << 26)

void ds_cache_dir(char *out, size_t len)
{
    const char *e = getenv("STAT_CACHE_DIR");
    if (e && *e) {
        snprintf(out, len, "%s", e);
        return;
    }
    e = getenv("XDG_CACHE_HOME");
    if (e && *e) {
        snprintf(out, len, "%s/stat/dirstat", e);
        return;
    }
    e = getenv("HOME");
    snprintf(out, len, "%s/.cache/stat/dirstat", e && *e ? e : "/tmp");
}

static int mkdir_p(const char *path)
{
    char buf[PATH_MAX];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* One cache file per scanned root, named after the percent-encoded path. */
static void cache_file_for(const char *root, char *out, size_t len)
{
    char dir[PATH_MAX];
    char enc[256];
    size_t k = 0;
    int too_long = 0;
    ds_cache_dir(dir, sizeof dir);
    for (const unsigned char *p = (const unsigned char *)root; *p; p++) {
        if (k + 4 >= 200) {
            too_long = 1;
            break;
        }
        if (isalnum(*p) || *p == '.' || *p == '_' || *p == '-')
            enc[k++] = (char)*p;
        else
            k += (size_t)snprintf(enc + k, sizeof enc - k, "%%%02X", *p);
    }
    enc[k] = '\0';
    if (too_long) {
        uint64_t h = 0xcbf29ce484222325ull;
        for (const unsigned char *p = (const unsigned char *)root; *p; p++)
            h = (h ^ *p) * 0x100000001b3ull;
        snprintf(enc, sizeof enc, "h_%016llx", (unsigned long long)h);
    }
    snprintf(out, len, "%s/%s.dstat", dir, enc);
}

static int write_node(FILE *f, const ds_node *n)
{
    size_t nl = strlen(n->name);
    uint16_t namelen = (uint16_t)(nl > UINT16_MAX ? UINT16_MAX : nl);
    fwrite(&n->type, 1, 1, f);
    fwrite(&n->flags, 1, 1, f);
    fwrite(&namelen, sizeof namelen, 1, f);
    fwrite(&n->size, sizeof n->size, 1, f);
    fwrite(&n->items, sizeof n->items, 1, f);
    fwrite(&n->nchildren, sizeof n->nchildren, 1, f);
    fwrite(n->name, 1, namelen, f);
    for (uint32_t i = 0; i < n->nchildren; i++)
        write_node(f, n->children[i]);
    return ferror(f) ? -1 : 0;
}

int ds_cache_save(const ds_scan *s, char *file_out, size_t file_len, char *err, size_t errlen)
{
    char dir[PATH_MAX], file[PATH_MAX], tmp[PATH_MAX + 32];
    ds_cache_dir(dir, sizeof dir);
    if (mkdir_p(dir) != 0) {
        snprintf(err, errlen, "cannot create cache dir %s: %s", dir, strerror(errno));
        return -1;
    }
    cache_file_for(s->root, file, sizeof file);
    snprintf(tmp, sizeof tmp, "%s.tmp.%ld", file, (long)getpid());

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        snprintf(err, errlen, "%s: %s", tmp, strerror(errno));
        return -1;
    }
    setvbuf(f, NULL, _IOFBF, 1 << 20);
    uint32_t rootlen = (uint32_t)strlen(s->root);
    fwrite(MAGIC, 1, sizeof MAGIC, f);
    fwrite(&s->scanned_at, sizeof s->scanned_at, 1, f);
    fwrite(&s->duration_ms, sizeof s->duration_ms, 1, f);
    fwrite(&s->errors, sizeof s->errors, 1, f);
    fwrite(&rootlen, sizeof rootlen, 1, f);
    fwrite(s->root, 1, rootlen, f);
    int rc = write_node(f, s->tree);
    if (fclose(f) != 0)
        rc = -1;
    if (rc != 0 || rename(tmp, file) != 0) {
        snprintf(err, errlen, "cannot write %s: %s", file, strerror(errno));
        unlink(tmp);
        return -1;
    }
    if (file_out)
        snprintf(file_out, file_len, "%s", file);
    return 0;
}

static int read_header(FILE *f, ds_scan *s)
{
    char magic[8];
    uint32_t rootlen;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, MAGIC, 8) != 0)
        return -1;
    if (fread(&s->scanned_at, sizeof s->scanned_at, 1, f) != 1 ||
        fread(&s->duration_ms, sizeof s->duration_ms, 1, f) != 1 ||
        fread(&s->errors, sizeof s->errors, 1, f) != 1 ||
        fread(&rootlen, sizeof rootlen, 1, f) != 1 || rootlen == 0 || rootlen >= PATH_MAX)
        return -1;
    s->root = xmalloc(rootlen + 1);
    if (fread(s->root, 1, rootlen, f) != rootlen) {
        free(s->root);
        s->root = NULL;
        return -1;
    }
    s->root[rootlen] = '\0';
    return 0;
}

/* Reads one node record without its children; nchildren is returned separately. */
static ds_node *read_record(FILE *f, uint32_t *nchildren)
{
    uint8_t type, flags;
    uint16_t namelen;
    uint64_t size, items;
    if (fread(&type, 1, 1, f) != 1 || fread(&flags, 1, 1, f) != 1 ||
        fread(&namelen, sizeof namelen, 1, f) != 1 || fread(&size, sizeof size, 1, f) != 1 ||
        fread(&items, sizeof items, 1, f) != 1 || fread(nchildren, sizeof *nchildren, 1, f) != 1 ||
        *nchildren > MAX_CHILDREN)
        return NULL;
    ds_node *n = xcalloc(1, sizeof *n);
    n->name = xmalloc((size_t)namelen + 1);
    if (fread(n->name, 1, namelen, f) != namelen) {
        free(n->name);
        free(n);
        return NULL;
    }
    n->name[namelen] = '\0';
    n->type = type;
    n->flags = flags;
    n->size = size;
    n->items = items;
    return n;
}

static ds_node *read_node(FILE *f)
{
    uint32_t nc;
    ds_node *n = read_record(f, &nc);
    if (!n)
        return NULL;
    if (nc) {
        n->children = xmalloc(nc * sizeof *n->children);
        n->cap = nc;
    }
    for (uint32_t i = 0; i < nc; i++) {
        ds_node *c = read_node(f);
        if (!c) {
            ds_node_free(n);
            return NULL;
        }
        n->children[n->nchildren++] = c;
    }
    return n;
}

int ds_cache_load(const char *file, ds_scan *out)
{
    memset(out, 0, sizeof *out);
    FILE *f = fopen(file, "rb");
    if (!f)
        return -1;
    setvbuf(f, NULL, _IOFBF, 1 << 20);
    int rc = -1;
    if (read_header(f, out) == 0) {
        out->tree = read_node(f);
        if (out->tree)
            rc = 0;
    }
    fclose(f);
    if (rc != 0)
        ds_scan_free(out);
    return rc;
}

static int read_entry(const char *file, ds_cache_entry *e)
{
    ds_scan s = { 0 };
    FILE *f = fopen(file, "rb");
    if (!f)
        return -1;
    int rc = -1;
    if (read_header(f, &s) == 0) {
        uint32_t nc;
        ds_node *root = read_record(f, &nc);
        if (root) {
            e->root = s.root;
            s.root = NULL;
            e->file = xstrdup(file);
            e->scanned_at = s.scanned_at;
            e->duration_ms = s.duration_ms;
            e->errors = s.errors;
            e->size = root->size;
            e->items = root->items;
            ds_node_free(root);
            rc = 0;
        }
    }
    free(s.root);
    fclose(f);
    return rc;
}

static int cmp_entry_time_desc(const void *a, const void *b)
{
    const ds_cache_entry *x = a, *y = b;
    return (x->scanned_at < y->scanned_at) - (x->scanned_at > y->scanned_at);
}

int ds_cache_list(ds_cache_entry **out, size_t *count)
{
    char dir[PATH_MAX];
    ds_cache_dir(dir, sizeof dir);
    *out = NULL;
    *count = 0;
    DIR *d = opendir(dir);
    if (!d)
        return errno == ENOENT ? 0 : -1;
    size_t cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t nl = strlen(de->d_name);
        if (nl < 7 || strcmp(de->d_name + nl - 6, ".dstat") != 0)
            continue;
        char file[PATH_MAX];
        snprintf(file, sizeof file, "%s/%s", dir, de->d_name);
        ds_cache_entry e;
        if (read_entry(file, &e) != 0)
            continue;
        if (*count == cap) {
            cap = cap ? cap * 2 : 8;
            *out = xrealloc(*out, cap * sizeof **out);
        }
        (*out)[(*count)++] = e;
    }
    closedir(d);
    if (*count > 1)
        qsort(*out, *count, sizeof **out, cmp_entry_time_desc);
    return 0;
}

int ds_cache_find(const char *path, ds_cache_entry *out)
{
    ds_cache_entry *list;
    size_t n;
    if (ds_cache_list(&list, &n) != 0)
        return -1;
    int found = -1;
    for (size_t i = 0; i < n; i++) {
        /* List is newest first, so the first covering scan is the freshest. */
        if (found < 0 && ds_path_covers(list[i].root, path)) {
            *out = list[i];
            found = 0;
        } else {
            ds_cache_entry_free(&list[i]);
        }
    }
    free(list);
    return found;
}

void ds_cache_entry_free(ds_cache_entry *e)
{
    free(e->root);
    free(e->file);
    e->root = e->file = NULL;
}

void ds_cache_list_free(ds_cache_entry *list, size_t count)
{
    for (size_t i = 0; i < count; i++)
        ds_cache_entry_free(&list[i]);
    free(list);
}
