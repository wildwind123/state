#ifndef DIRSTAT_H
#define DIRSTAT_H

#include <stdint.h>
#include <stdio.h>

#include "util.h"

enum ds_type { DS_FILE = 0, DS_DIR = 1, DS_OTHER = 2 };
enum ds_flag {
    DS_F_ERROR = 1,   /* could not read (e.g. permission denied) */
    DS_F_SKIPPED = 2, /* not descended: virtual fs or other fs with -x */
};

typedef struct ds_node {
    char *name;                /* basename; the root node holds the full path */
    uint64_t size;             /* bytes on disk, including everything below */
    uint64_t items;            /* number of entries below (directories only) */
    struct ds_node **children; /* sorted by size, largest first */
    uint32_t nchildren;
    uint32_t cap;
    uint8_t type;
    uint8_t flags;
} ds_node;

typedef struct {
    char *root;          /* absolute path that was scanned */
    int64_t scanned_at;  /* unix time */
    uint64_t duration_ms;
    uint64_t errors;
    ds_node *tree;
} ds_scan;

typedef struct {
    int one_fs; /* do not cross into other filesystems */
    void (*progress)(uint64_t items, uint64_t bytes, void *ud);
    void *progress_ud;
} ds_scan_opts;

/* Summary of one cache file, read from its header only. */
typedef struct {
    char *root;
    char *file;
    int64_t scanned_at;
    uint64_t duration_ms;
    uint64_t errors;
    uint64_t size;
    uint64_t items;
} ds_cache_entry;

typedef struct {
    int depth;     /* levels below the start node to print */
    uint32_t top;  /* children per directory, 0 = all */
} ds_tree_opts;

typedef struct {
    int depth;        /* levels below the start node to include */
    uint32_t limit;   /* children per directory, 0 = all */
    double min_ratio; /* fold children smaller than this share of the start node */
} ds_json_opts;

/* scan.c (POSIX) / scan_win32.c */
int ds_scan_path(const char *path, const ds_scan_opts *opts, ds_scan *out, char *err, size_t errlen);

/* node.c */
ds_node *ds_node_new(const char *name, uint8_t type);
void ds_node_add(ds_node *p, ds_node *c);
void ds_node_finish(ds_node *dir); /* sort children by size and trim memory */
void ds_node_free(ds_node *n);
void ds_scan_free(ds_scan *s);
int ds_path_covers(const char *root, const char *path);
ds_node *ds_find(const ds_scan *s, const char *path);

/* cache.c */
void ds_cache_dir(char *out, size_t len);
int ds_cache_save(const ds_scan *s, char *file_out, size_t file_len, char *err, size_t errlen);
int ds_cache_load(const char *file, ds_scan *out);
int ds_cache_list(ds_cache_entry **out, size_t *count);
/* Finds the most recent scan that contains path. Returns 0 if found. */
int ds_cache_find(const char *path, ds_cache_entry *out);
void ds_cache_entry_free(ds_cache_entry *e);
void ds_cache_list_free(ds_cache_entry *list, size_t count);

/* tree.c */
void ds_print_tree(FILE *out, const ds_scan *s, const ds_node *n, const char *path,
                   const ds_tree_opts *o);

/* json.c */
void ds_json_scan(sbuf *b, const ds_scan *s, const ds_node *n, const char *path,
                  const ds_json_opts *o);
void ds_json_cache_list(sbuf *b, const ds_cache_entry *list, size_t count);

#endif
