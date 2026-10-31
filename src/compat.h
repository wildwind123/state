#ifndef STAT_COMPAT_H
#define STAT_COMPAT_H

/*
 * Small portability layer for Linux and Windows (MinGW-w64).
 * All paths are UTF-8. On Windows, paths use '/' separators ("C:/Users").
 */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <wchar.h>

/* MinGW defines PATH_MAX as 260, but Windows paths can be much longer with \\?\ prefixes. */
#ifdef _WIN32
#undef PATH_MAX
#define PATH_MAX 4096
#endif

/* Converts argv to UTF-8 and sets the console to UTF-8 on Windows. */
void platform_init(int *argc, char ***argv);

FILE *u8_fopen(const char *path, const char *mode);
int u8_mkdir(const char *path); /* 0 if created or already a directory */
int u8_rename(const char *from, const char *to); /* replaces an existing target */
int u8_unlink(const char *path);

typedef struct {
    int is_dir;
    int64_t size;
    int64_t mtime_ns;
} u8_statbuf;

int u8_stat(const char *path, u8_statbuf *st);

typedef struct u8_dir u8_dir;
u8_dir *u8_opendir(const char *path);
const char *u8_readdir(u8_dir *d); /* skips "." and ".."; NULL at the end */
void u8_closedir(u8_dir *d);

/* Environment variable as UTF-8; returns 0 if set and non-empty. */
int u8_getenv(const char *name, char *out, size_t len);

void sleep_ms(unsigned ms);
long get_pid(void);
void local_time(int64_t t, struct tm *out);
int stderr_is_tty(void);

/* Path prefix comparison; case-insensitive on Windows. */
int path_ncmp(const char *a, const char *b, size_t n);

#ifdef _WIN32
char *u8_from_wide(const wchar_t *w); /* malloc'ed */
wchar_t *wide_from_u8(const char *s); /* malloc'ed */
#endif

#endif
