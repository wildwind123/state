#include "dirstat.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define BAR_WIDTH 20

static void print_bar(FILE *out, double pct)
{
    int fill = (int)(pct / 100.0 * BAR_WIDTH + 0.5);
    fputc('[', out);
    for (int i = 0; i < BAR_WIDTH; i++)
        fputc(i < fill ? '#' : ' ', out);
    fputc(']', out);
}

static void print_row(FILE *out, const ds_node *n, uint64_t parent_size, const char *prefix,
                      const char *branch)
{
    char sz[32];
    double pct = parent_size ? 100.0 * (double)n->size / (double)parent_size : 100.0;
    fmt_size(n->size, sz, sizeof sz);
    fprintf(out, "%10s %5.1f%% ", sz, pct);
    print_bar(out, pct);
    size_t nl = strlen(n->name);
    int slash = n->type == DS_DIR && nl && n->name[nl - 1] != '/';
    fprintf(out, "  %s%s%s%s", prefix, branch, n->name, slash ? "/" : "");
    if (n->flags & DS_F_ERROR)
        fputs("  [unreadable]", out);
    if (n->flags & DS_F_SKIPPED)
        fputs("  [skipped]", out);
    fputc('\n', out);
}

static void print_children(FILE *out, const ds_node *n, const char *prefix, int depth_left,
                           const ds_tree_opts *o)
{
    uint32_t shown = n->nchildren;
    if (o->top && shown > o->top)
        shown = o->top;
    uint64_t rest_size = 0;
    for (uint32_t i = shown; i < n->nchildren; i++)
        rest_size += n->children[i]->size;
    int has_rest = shown < n->nchildren;

    for (uint32_t i = 0; i < shown; i++) {
        const ds_node *c = n->children[i];
        int last = i == shown - 1 && !has_rest;
        print_row(out, c, n->size, prefix, last ? "└── " : "├── ");
        if (c->type == DS_DIR && c->nchildren && depth_left > 1) {
            size_t pl = strlen(prefix);
            char *np = xmalloc(pl + 8);
            memcpy(np, prefix, pl);
            strcpy(np + pl, last ? "    " : "│   ");
            print_children(out, c, np, depth_left - 1, o);
            free(np);
        }
    }
    if (has_rest) {
        char sz[32];
        double pct = n->size ? 100.0 * (double)rest_size / (double)n->size : 0.0;
        fmt_size(rest_size, sz, sizeof sz);
        fprintf(out, "%10s %5.1f%% ", sz, pct);
        print_bar(out, pct);
        fprintf(out, "  %s└── … %" PRIu32 " more\n", prefix, n->nchildren - shown);
    }
}

void ds_print_tree(FILE *out, const ds_scan *s, const ds_node *n, const char *path,
                   const ds_tree_opts *o)
{
    char when[64], ago[64], sz[32];
    fmt_time(s->scanned_at, when, sizeof when);
    fmt_ago(s->scanned_at, ago, sizeof ago);
    fmt_size(n->size, sz, sizeof sz);

    fprintf(out, "Path:    %s\n", path);
    fprintf(out, "Scanned: %s (%s)", when, ago);
    if (strcmp(s->root, path) != 0)
        fprintf(out, ", as part of the scan of %s", s->root);
    fputc('\n', out);
    fprintf(out, "Total:   %s in %" PRIu64 " items\n\n", sz, n->items);

    ds_node shown = *n;
    shown.name = (char *)path;
    print_row(out, &shown, n->size, "", "");
    if (o->depth > 0)
        print_children(out, n, "", o->depth, o);
}
