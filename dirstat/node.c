#include "dirstat.h"

#include <stdlib.h>
#include <string.h>

#include "compat.h"

ds_node *ds_node_new(const char *name, uint8_t type)
{
    ds_node *n = xcalloc(1, sizeof *n);
    n->name = xstrdup(name);
    n->type = type;
    return n;
}

void ds_node_add(ds_node *p, ds_node *c)
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

void ds_node_finish(ds_node *dir)
{
    if (dir->nchildren > 1)
        qsort(dir->children, dir->nchildren, sizeof *dir->children, cmp_size_desc);
    if (dir->cap > dir->nchildren) {
        dir->children = xrealloc(dir->children, dir->nchildren * sizeof *dir->children);
        dir->cap = dir->nchildren;
    }
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
    size_t rl = strlen(root);
    if (path_ncmp(root, path, rl) != 0)
        return 0;
    /* "/" and "C:/" end with a separator; anything else must match a whole component. */
    return (rl && root[rl - 1] == '/') || path[rl] == '\0' || path[rl] == '/';
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
            if (strlen(c->name) == len && path_ncmp(c->name, start, len) == 0) {
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
