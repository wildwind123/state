#ifndef STAT_UTIL_H
#define STAT_UTIL_H

#include <stddef.h>
#include <stdint.h>

#include "compat.h"

/* Growable string buffer. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} sbuf;

void sb_init(sbuf *b);
void sb_free(sbuf *b);
void sb_append(sbuf *b, const char *s, size_t n);
void sb_puts(sbuf *b, const char *s);
void sb_printf(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Appends s as a quoted JSON string; invalid UTF-8 bytes become U+FFFD. */
void sb_json_str(sbuf *b, const char *s);

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

void fmt_size(uint64_t bytes, char *out, size_t outlen);
void fmt_time(int64_t t, char *out, size_t outlen);
void fmt_ago(int64_t t, char *out, size_t outlen);

/* Makes path absolute and canonical; works for paths that no longer exist.
 * Implemented per platform in compat.c. */
int normalize_path(const char *in, char *out, size_t outlen);
uint64_t now_ms(void);

#endif
