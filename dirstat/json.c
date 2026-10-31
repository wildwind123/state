#include "dirstat.h"

#include <inttypes.h>

static const char *type_name(uint8_t t)
{
    switch (t) {
    case DS_DIR: return "dir";
    case DS_FILE: return "file";
    default: return "other";
    }
}

static void json_node(sbuf *b, const ds_node *n, const char *name, int depth,
                      const ds_json_opts *o, uint64_t min_size)
{
    sb_puts(b, "{\"name\":");
    sb_json_str(b, name);
    sb_printf(b, ",\"type\":\"%s\",\"size\":%" PRIu64 ",\"items\":%" PRIu64, type_name(n->type),
              n->size, n->items);
    if (n->flags & DS_F_ERROR)
        sb_puts(b, ",\"error\":true");
    if (n->flags & DS_F_SKIPPED)
        sb_puts(b, ",\"skipped\":true");

    if (n->type == DS_DIR && n->nchildren) {
        if (depth <= 0) {
            sb_puts(b, ",\"truncated\":true}");
            return;
        }
        sb_puts(b, ",\"children\":[");
        uint32_t shown = 0, rest_count = 0;
        uint64_t rest_size = 0, rest_items = 0;
        for (uint32_t i = 0; i < n->nchildren; i++) {
            const ds_node *c = n->children[i];
            if ((o->limit && shown >= o->limit) || c->size < min_size) {
                rest_count++;
                rest_size += c->size;
                rest_items += c->items + 1;
                continue;
            }
            if (shown++)
                sb_puts(b, ",");
            json_node(b, c, c->name, depth - 1, o, min_size);
        }
        if (rest_count) {
            sb_printf(b,
                      "%s{\"name\":\"%" PRIu32 " more items\",\"type\":\"rest\",\"size\":%" PRIu64
                      ",\"items\":%" PRIu64 ",\"count\":%" PRIu32 "}",
                      shown ? "," : "", rest_count, rest_size, rest_items, rest_count);
        }
        sb_puts(b, "]");
    }
    sb_puts(b, "}");
}

void ds_json_scan(sbuf *b, const ds_scan *s, const ds_node *n, const char *path,
                  const ds_json_opts *o)
{
    uint64_t min_size = o->min_ratio > 0 ? (uint64_t)((double)n->size * o->min_ratio) : 0;
    sb_puts(b, "{\"root\":");
    sb_json_str(b, s->root);
    sb_puts(b, ",\"path\":");
    sb_json_str(b, path);
    sb_printf(b,
              ",\"scanned_at\":%" PRId64 ",\"duration_ms\":%" PRIu64 ",\"errors\":%" PRIu64
              ",\"tree\":",
              s->scanned_at, s->duration_ms, s->errors);
    json_node(b, n, path, o->depth, o, min_size);
    sb_puts(b, "}");
}

void ds_json_cache_list(sbuf *b, const ds_cache_entry *list, size_t count)
{
    sb_puts(b, "{\"scans\":[");
    for (size_t i = 0; i < count; i++) {
        const ds_cache_entry *e = &list[i];
        sb_puts(b, i ? ",{\"root\":" : "{\"root\":");
        sb_json_str(b, e->root);
        sb_printf(b,
                  ",\"scanned_at\":%" PRId64 ",\"duration_ms\":%" PRIu64 ",\"errors\":%" PRIu64
                  ",\"size\":%" PRIu64 ",\"items\":%" PRIu64 "}",
                  e->scanned_at, e->duration_ms, e->errors, e->size, e->items);
    }
    sb_puts(b, "]}");
}
