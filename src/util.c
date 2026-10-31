#define _GNU_SOURCE
#include "util.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void oom(void)
{
    fputs("stat: out of memory\n", stderr);
    abort();
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        oom();
    return p;
}

void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p)
        oom();
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        oom();
    return p;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void sb_init(sbuf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void sb_free(sbuf *b)
{
    free(b->data);
    sb_init(b);
}

static void sb_reserve(sbuf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra + 1)
        cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void sb_append(sbuf *b, const char *s, size_t n)
{
    sb_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void sb_puts(sbuf *b, const char *s)
{
    sb_append(b, s, strlen(s));
}

void sb_printf(sbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    sb_reserve(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

/* Length of a valid UTF-8 multi-byte sequence at p, or 0 if invalid. */
static int utf8_len(const unsigned char *p)
{
    int n;
    uint32_t cp, min;
    if ((p[0] & 0xE0) == 0xC0) {
        n = 2; cp = p[0] & 0x1F; min = 0x80;
    } else if ((p[0] & 0xF0) == 0xE0) {
        n = 3; cp = p[0] & 0x0F; min = 0x800;
    } else if ((p[0] & 0xF8) == 0xF0) {
        n = 4; cp = p[0] & 0x07; min = 0x10000;
    } else {
        return 0;
    }
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;
    return n;
}

void sb_json_str(sbuf *b, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    sb_append(b, "\"", 1);
    while (*p) {
        unsigned c = *p;
        if (c == '"' || c == '\\') {
            char e[2] = { '\\', (char)c };
            sb_append(b, e, 2);
            p++;
        } else if (c < 0x20) {
            sb_printf(b, "\\u%04x", c);
            p++;
        } else if (c < 0x80) {
            sb_append(b, (const char *)p, 1);
            p++;
        } else {
            int n = utf8_len(p);
            if (n > 0) {
                sb_append(b, (const char *)p, (size_t)n);
                p += n;
            } else {
                sb_puts(b, "\\ufffd");
                p++;
            }
        }
    }
    sb_append(b, "\"", 1);
}

void fmt_size(uint64_t bytes, char *out, size_t outlen)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB" };
    if (bytes < 1024) {
        snprintf(out, outlen, "%" PRIu64 " B", bytes);
        return;
    }
    double v = (double)bytes;
    int i = 0;
    while (v >= 1024.0 && i < 6) {
        v /= 1024.0;
        i++;
    }
    snprintf(out, outlen, "%.1f %s", v, units[i]);
}

void fmt_time(int64_t t, char *out, size_t outlen)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(out, outlen, "%Y-%m-%d %H:%M:%S", &tm);
}

void fmt_ago(int64_t t, char *out, size_t outlen)
{
    int64_t d = (int64_t)time(NULL) - t;
    if (d < 0)
        d = 0;
    if (d < 60)
        snprintf(out, outlen, "%" PRId64 " seconds ago", d);
    else if (d < 3600)
        snprintf(out, outlen, "%" PRId64 " minutes ago", d / 60);
    else if (d < 86400)
        snprintf(out, outlen, "%" PRId64 " hours ago", d / 3600);
    else
        snprintf(out, outlen, "%" PRId64 " days ago", d / 86400);
}

/* Lexically resolves ".", ".." and duplicate slashes in an absolute path. */
static void clean_path(const char *in, char *out, size_t outlen)
{
    size_t o = 0;
    const char *p = in;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *s = p;
        while (*p && *p != '/')
            p++;
        size_t n = (size_t)(p - s);
        if (n == 1 && s[0] == '.')
            continue;
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 0 && out[o - 1] != '/')
                o--;
            if (o > 0)
                o--;
            continue;
        }
        if (o + 1 + n + 1 > outlen)
            break;
        out[o++] = '/';
        memcpy(out + o, s, n);
        o += n;
    }
    if (o == 0)
        out[o++] = '/';
    out[o] = '\0';
}

int normalize_path(const char *in, char *out, size_t outlen)
{
    char buf[PATH_MAX];
    if (realpath(in, buf)) {
        if (strlen(buf) + 1 > outlen) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(out, buf);
        return 0;
    }
    if (in[0] == '/') {
        clean_path(in, out, outlen);
        return 0;
    }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd))
        return -1;
    char joined[PATH_MAX * 2];
    snprintf(joined, sizeof joined, "%s/%s", cwd, in);
    clean_path(joined, out, outlen);
    return 0;
}

uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
