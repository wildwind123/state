#define _GNU_SOURCE
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dirstat.h"
#include "server.h"
#include "sysinfo.h"
#include "util.h"

#define DEFAULT_ADDR "127.0.0.1:8084"

static void usage(FILE *out)
{
    fputs("Usage:\n"
          "  stat [-j, --json]                 system overview: CPU, memory, disks, GPU\n"
          "  stat -s, --server [HOST:PORT]     run the HTTP API and web UI (default " DEFAULT_ADDR ")\n"
          "  stat dirstat -s, --scan [PATH]    scan disk usage under PATH and cache the result\n"
          "  stat dirstat -t, --tree [PATH]    show the cached result as a tree (alias: --three)\n"
          "  stat dirstat -l, --list           list cached scans\n"
          "\n"
          "dirstat options:\n"
          "  -d, --depth N            tree levels to show (default 2, 0 = unlimited)\n"
          "  -n, --top N              biggest entries per directory (default 10, 0 = all)\n"
          "  -x, --one-file-system    do not descend into other filesystems while scanning\n"
          "  -j, --json               print JSON (same as the API) instead of a tree\n"
          "  -q, --quiet              no progress or summary output\n"
          "\n"
          "Cache directory: $STAT_CACHE_DIR, else $XDG_CACHE_HOME/stat/dirstat,\n"
          "else ~/.cache/stat/dirstat\n",
          out);
}

static void cli_progress(uint64_t items, uint64_t bytes, void *ud)
{
    (void)ud;
    char sz[32];
    fmt_size(bytes, sz, sizeof sz);
    fprintf(stderr, "\rScanning... %" PRIu64 " items, %s   ", items, sz);
}

static int show(const ds_scan *s, const ds_node *n, const char *path, const ds_tree_opts *to,
                int json)
{
    if (json) {
        ds_json_opts jo = { .depth = to->depth, .limit = to->top };
        sbuf b;
        sb_init(&b);
        ds_json_scan(&b, s, n, path, &jo);
        puts(b.data);
        sb_free(&b);
    } else {
        ds_print_tree(stdout, s, n, path, to);
    }
    return 0;
}

static int do_scan(const char *path, const ds_scan_opts *so, const ds_tree_opts *to, int json,
                   int quiet)
{
    ds_scan s;
    char err[512], file[PATH_MAX];
    if (ds_scan_path(path, so, &s, err, sizeof err) != 0) {
        if (so->progress)
            fputs("\r\033[K", stderr);
        fprintf(stderr, "stat: %s\n", err);
        return 1;
    }
    if (so->progress)
        fputs("\r\033[K", stderr);
    int rc = 0;
    if (ds_cache_save(&s, file, sizeof file, err, sizeof err) != 0) {
        fprintf(stderr, "stat: %s\n", err);
        rc = 1;
    }
    if (json) {
        show(&s, s.tree, s.root, to, 1);
    } else if (!quiet) {
        char sz[32];
        fmt_size(s.tree->size, sz, sizeof sz);
        printf("Scanned %s: %s in %" PRIu64 " items, %.1f s", s.root, sz, s.tree->items,
               (double)s.duration_ms / 1000.0);
        if (s.errors)
            printf(", %" PRIu64 " unreadable entries (try sudo)", s.errors);
        printf("\n");
        if (rc == 0)
            printf("Cached to %s\n", file);
        printf("\n");
        ds_tree_opts brief = { .depth = 1, .top = to->top };
        ds_print_tree(stdout, &s, s.tree, s.root, &brief);
    }
    ds_scan_free(&s);
    return rc;
}

static int do_tree(const char *arg, const ds_tree_opts *to, int json)
{
    char path[PATH_MAX];
    if (normalize_path(arg, path, sizeof path) != 0) {
        perror(arg);
        return 1;
    }
    ds_cache_entry e;
    if (ds_cache_find(path, &e) != 0) {
        fprintf(stderr, "stat: no cached scan covers %s\nRun: stat dirstat --scan %s\n", path, path);
        return 1;
    }
    ds_scan s;
    if (ds_cache_load(e.file, &s) != 0) {
        fprintf(stderr, "stat: cannot read cache file %s\n", e.file);
        ds_cache_entry_free(&e);
        return 1;
    }
    ds_cache_entry_free(&e);
    int rc = 0;
    ds_node *n = ds_find(&s, path);
    if (!n) {
        char when[64];
        fmt_time(s.scanned_at, when, sizeof when);
        fprintf(stderr, "stat: %s did not exist when %s was scanned (%s)\n", path, s.root, when);
        rc = 1;
    } else {
        show(&s, n, path, to, json);
    }
    ds_scan_free(&s);
    return rc;
}

static int do_list(int json)
{
    ds_cache_entry *list;
    size_t n;
    if (ds_cache_list(&list, &n) != 0) {
        perror("stat: cache directory");
        return 1;
    }
    if (json) {
        sbuf b;
        sb_init(&b);
        ds_json_cache_list(&b, list, n);
        puts(b.data);
        sb_free(&b);
    } else if (n == 0) {
        puts("No cached scans. Run: stat dirstat --scan PATH");
    } else {
        printf("%-19s  %10s  %10s  %s\n", "SCANNED", "SIZE", "ITEMS", "PATH");
        for (size_t i = 0; i < n; i++) {
            char when[64], sz[32];
            fmt_time(list[i].scanned_at, when, sizeof when);
            fmt_size(list[i].size, sz, sizeof sz);
            printf("%-19s  %10s  %10" PRIu64 "  %s\n", when, sz, list[i].items, list[i].root);
        }
    }
    ds_cache_list_free(list, n);
    return 0;
}

static int cmd_dirstat(int argc, char **argv)
{
    enum { M_NONE, M_SCAN, M_TREE, M_LIST } mode = M_NONE;
    ds_scan_opts so = { 0 };
    ds_tree_opts to = { .depth = 2, .top = 10 };
    int json = 0, quiet = 0, c;
    static const struct option lo[] = {
        { "scan", no_argument, NULL, 's' },
        { "tree", no_argument, NULL, 't' },
        { "three", no_argument, NULL, 't' },
        { "list", no_argument, NULL, 'l' },
        { "depth", required_argument, NULL, 'd' },
        { "top", required_argument, NULL, 'n' },
        { "one-file-system", no_argument, NULL, 'x' },
        { "json", no_argument, NULL, 'j' },
        { "quiet", no_argument, NULL, 'q' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    while ((c = getopt_long(argc, argv, "stld:n:xjqh", lo, NULL)) != -1) {
        switch (c) {
        case 's': mode = M_SCAN; break;
        case 't': mode = M_TREE; break;
        case 'l': mode = M_LIST; break;
        case 'd': to.depth = atoi(optarg); if (to.depth <= 0) to.depth = INT_MAX; break;
        case 'n': to.top = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'x': so.one_fs = 1; break;
        case 'j': json = 1; break;
        case 'q': quiet = 1; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    const char *path = optind < argc ? argv[optind] : ".";

    switch (mode) {
    case M_SCAN:
        if (!quiet && isatty(STDERR_FILENO))
            so.progress = cli_progress;
        return do_scan(path, &so, &to, json, quiet);
    case M_TREE:
        return do_tree(path, &to, json);
    case M_LIST:
        return do_list(json);
    default:
        fputs("stat dirstat: choose --scan, --tree or --list\n\n", stderr);
        usage(stderr);
        return 2;
    }
}

static int overview(int json)
{
    si_snapshot s;
    si_cpu_sample a, b;
    si_snapshot_base(&s);
    si_cpu_sample_read(&a);
    usleep(500 * 1000); /* CPU usage is measured over this interval */
    si_cpu_sample_read(&b);
    si_cpu_compute(&a, &b, &s.cpu);
    si_cpu_sample_free(&a);
    si_cpu_sample_free(&b);
    si_gpus_read(&s.gpus, &s.ngpus);
    if (json) {
        sbuf out;
        sb_init(&out);
        si_snapshot_json(&out, &s);
        puts(out.data);
        sb_free(&out);
    } else {
        si_snapshot_print(stdout, &s);
    }
    si_snapshot_free(&s);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return overview(0);
    const char *cmd = argv[1];
    if (strcmp(cmd, "-j") == 0 || strcmp(cmd, "--json") == 0)
        return overview(1);
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "-s") == 0 || strcmp(cmd, "--server") == 0)
        return server_run(argc > 2 ? argv[2] : DEFAULT_ADDR);
    if (strcmp(cmd, "dirstat") == 0)
        return cmd_dirstat(argc - 1, argv + 1);
    fprintf(stderr, "stat: unknown command '%s'\n\n", cmd);
    usage(stderr);
    return 2;
}
