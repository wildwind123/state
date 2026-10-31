#define _GNU_SOURCE
#include "compat.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* Lexically resolves ".", ".." and duplicate slashes. prefix_len bytes (e.g. "C:") are kept. */
static void clean_path(const char *in, size_t prefix_len, char *out, size_t outlen)
{
    size_t o = prefix_len < outlen ? prefix_len : 0;
    memcpy(out, in, o);
    size_t floor = o;
    const char *p = in + o;
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
            while (o > floor && out[o - 1] != '/')
                o--;
            if (o > floor)
                o--;
            continue;
        }
        if (o + 1 + n + 1 > outlen)
            break;
        out[o++] = '/';
        memcpy(out + o, s, n);
        o += n;
    }
    if (o == floor)
        out[o++] = '/';
    out[o] = '\0';
}

#ifdef _WIN32

char *u8_from_wide(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = xmalloc(n > 0 ? (size_t)n : 1);
    if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL))
        s[0] = '\0';
    return s;
}

wchar_t *wide_from_u8(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = xmalloc((n > 0 ? (size_t)n : 1) * sizeof *w);
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n))
        w[0] = L'\0';
    return w;
}

/* "C:/x" -> "\\?\C:\x" so long paths work. */
static wchar_t *win_path(const char *path)
{
    wchar_t *w = wide_from_u8(path);
    for (wchar_t *p = w; *p; p++)
        if (*p == L'/')
            *p = L'\\';
    return w;
}

void platform_init(int *argc, char ***argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    int n;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!wargv)
        return;
    char **av = xcalloc((size_t)n + 1, sizeof *av);
    for (int i = 0; i < n; i++)
        av[i] = u8_from_wide(wargv[i]);
    LocalFree(wargv);
    *argc = n;
    *argv = av; /* lives until exit */
}

FILE *u8_fopen(const char *path, const char *mode)
{
    wchar_t *wp = win_path(path), *wm = wide_from_u8(mode);
    FILE *f = _wfopen(wp, wm);
    free(wp);
    free(wm);
    return f;
}

int u8_mkdir(const char *path)
{
    u8_statbuf st;
    if (u8_stat(path, &st) == 0)
        return st.is_dir ? 0 : -1;
    wchar_t *w = win_path(path);
    int ok = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    free(w);
    return ok ? 0 : -1;
}

int u8_rename(const char *from, const char *to)
{
    wchar_t *a = win_path(from), *b = win_path(to);
    int ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
    free(a);
    free(b);
    return ok ? 0 : -1;
}

int u8_unlink(const char *path)
{
    wchar_t *w = win_path(path);
    int ok = DeleteFileW(w);
    free(w);
    return ok ? 0 : -1;
}

int u8_stat(const char *path, u8_statbuf *st)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    wchar_t *w = win_path(path);
    int ok = GetFileAttributesExW(w, GetFileExInfoStandard, &a);
    free(w);
    if (!ok) {
        errno = ENOENT;
        return -1;
    }
    st->is_dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st->size = (int64_t)(((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow);
    uint64_t ft = ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    st->mtime_ns = (int64_t)(ft * 100);
    return 0;
}

struct u8_dir {
    HANDLE h;
    WIN32_FIND_DATAW fd;
    int first;
    char *name;
};

u8_dir *u8_opendir(const char *path)
{
    size_t n = strlen(path);
    char *pat = xmalloc(n + 3);
    memcpy(pat, path, n);
    strcpy(pat + n, n && path[n - 1] == '/' ? "*" : "/*");
    wchar_t *w = win_path(pat);
    free(pat);
    u8_dir *d = xcalloc(1, sizeof *d);
    d->h = FindFirstFileW(w, &d->fd);
    free(w);
    if (d->h == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->first = 1;
    return d;
}

const char *u8_readdir(u8_dir *d)
{
    for (;;) {
        if (!d->first && !FindNextFileW(d->h, &d->fd))
            return NULL;
        d->first = 0;
        const wchar_t *n = d->fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0)))
            continue;
        free(d->name);
        d->name = u8_from_wide(n);
        return d->name;
    }
}

void u8_closedir(u8_dir *d)
{
    FindClose(d->h);
    free(d->name);
    free(d);
}

int u8_getenv(const char *name, char *out, size_t len)
{
    wchar_t *wn = wide_from_u8(name);
    const wchar_t *v = _wgetenv(wn);
    free(wn);
    if (!v || !*v)
        return -1;
    char *u = u8_from_wide(v);
    snprintf(out, len, "%s", u);
    free(u);
    for (char *p = out; *p; p++)
        if (*p == '\\')
            *p = '/';
    return 0;
}

void sleep_ms(unsigned ms)
{
    Sleep(ms);
}

long get_pid(void)
{
    return (long)GetCurrentProcessId();
}

void local_time(int64_t t, struct tm *out)
{
    time_t tt = (time_t)t;
    localtime_s(out, &tt);
}

int stderr_is_tty(void)
{
    return _isatty(_fileno(stderr));
}

int path_ncmp(const char *a, const char *b, size_t n)
{
    return _strnicmp(a, b, n);
}

int normalize_path(const char *in, char *out, size_t outlen)
{
    wchar_t *w = wide_from_u8(in);
    wchar_t buf[32768];
    DWORD n = GetFullPathNameW(w, 32768, buf, NULL);
    free(w);
    if (n == 0 || n >= 32768) {
        errno = ENOENT;
        return -1;
    }
    char *u = u8_from_wide(buf);
    char *p = u;
    if (strncmp(p, "\\\\?\\", 4) == 0)
        p += 4;
    for (char *c = p; *c; c++)
        if (*c == '\\')
            *c = '/';
    /* Keep "C:" or the "//server/share" part of UNC paths as an untouchable prefix. */
    size_t prefix = 0;
    if (p[0] && p[1] == ':') {
        if (p[0] >= 'a' && p[0] <= 'z')
            p[0] = (char)(p[0] - 'a' + 'A');
        prefix = 2;
    } else if (p[0] == '/' && p[1] == '/') {
        char *s = strchr(p + 2, '/');
        s = s ? strchr(s + 1, '/') : NULL;
        prefix = s ? (size_t)(s - p) : strlen(p);
    }
    clean_path(p, prefix, out, outlen);
    free(u);
    return 0;
}

uint64_t now_ms(void)
{
    return (uint64_t)GetTickCount64();
}

#else /* POSIX */

void platform_init(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;
}

FILE *u8_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

int u8_mkdir(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return 0;
    return -1;
}

int u8_rename(const char *from, const char *to)
{
    return rename(from, to);
}

int u8_unlink(const char *path)
{
    return unlink(path);
}

int u8_stat(const char *path, u8_statbuf *st)
{
    struct stat s;
    if (stat(path, &s) != 0)
        return -1;
    st->is_dir = S_ISDIR(s.st_mode);
    st->size = (int64_t)s.st_size;
    st->mtime_ns = (int64_t)s.st_mtim.tv_sec * 1000000000 + s.st_mtim.tv_nsec;
    return 0;
}

struct u8_dir {
    DIR *d;
};

u8_dir *u8_opendir(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return NULL;
    u8_dir *r = xmalloc(sizeof *r);
    r->d = d;
    return r;
}

const char *u8_readdir(u8_dir *d)
{
    struct dirent *e;
    while ((e = readdir(d->d)) != NULL) {
        const char *n = e->d_name;
        if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0')))
            continue;
        return n;
    }
    return NULL;
}

void u8_closedir(u8_dir *d)
{
    closedir(d->d);
    free(d);
}

int u8_getenv(const char *name, char *out, size_t len)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return -1;
    snprintf(out, len, "%s", v);
    return 0;
}

void sleep_ms(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000 };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

long get_pid(void)
{
    return (long)getpid();
}

void local_time(int64_t t, struct tm *out)
{
    time_t tt = (time_t)t;
    localtime_r(&tt, out);
}

int stderr_is_tty(void)
{
    return isatty(STDERR_FILENO);
}

int path_ncmp(const char *a, const char *b, size_t n)
{
    return strncmp(a, b, n);
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
        clean_path(in, 0, out, outlen);
        return 0;
    }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd))
        return -1;
    char joined[PATH_MAX * 2];
    snprintf(joined, sizeof joined, "%s/%s", cwd, in);
    clean_path(joined, 0, out, outlen);
    return 0;
}

uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

#endif
