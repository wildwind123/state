#define _GNU_SOURCE
#include "server.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
typedef SOCKET sock_t;
#define close_sock closesocket
#define SEND_FLAGS 0
#define sock_error() ((int)WSAGetLastError())
#else
#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define close_sock close
#define SEND_FLAGS MSG_NOSIGNAL
#define sock_error() errno
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include "compat.h"
#include "dirstat.h"
#include "sysinfo.h"
#include "util.h"

extern const unsigned char web_index_html[];
extern const size_t web_index_html_len;
extern const unsigned char web_dirstat_html[];
extern const size_t web_dirstat_html_len;

/* ---------- system sampler: CPU every second, GPU every two ---------- */

static struct {
    pthread_mutex_t lock;
    si_cpu cpu;
    si_gpu *gpus;
    size_t ngpus;
} g_sys = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* Last two minutes of CPU and memory usage, so charts are full when a page opens. */
#define HISTORY_LEN 120
static struct {
    int64_t t;
    double cpu;
    double mem;
} g_hist[HISTORY_LEN];
static size_t g_hist_len, g_hist_next; /* guarded by g_sys.lock */

static si_cpu_sample g_prev_sample;

/* Takes one CPU sample (and optionally GPU) and publishes it. */
static void sample_once(int with_gpu)
{
    si_cpu_sample cur;
    si_cpu c;
    si_cpu_sample_read(&cur);
    si_cpu_compute(&g_prev_sample, &cur, &c);
    si_cpu_sample_free(&g_prev_sample);
    g_prev_sample = cur;

    si_gpu *gpus = NULL;
    size_t ngpus = 0;
    if (with_gpu)
        si_gpus_read(&gpus, &ngpus);

    si_mem m;
    double mem_pct = si_mem_read(&m) == 0 && m.total ? 100.0 * (double)m.used / (double)m.total : 0;

    pthread_mutex_lock(&g_sys.lock);
    g_hist[g_hist_next].t = (int64_t)time(NULL);
    g_hist[g_hist_next].cpu = c.usage;
    g_hist[g_hist_next].mem = mem_pct;
    g_hist_next = (g_hist_next + 1) % HISTORY_LEN;
    if (g_hist_len < HISTORY_LEN)
        g_hist_len++;
    si_cpu_free(&g_sys.cpu);
    g_sys.cpu = c;
    if (with_gpu) {
        free(g_sys.gpus);
        g_sys.gpus = gpus;
        g_sys.ngpus = ngpus;
    }
    pthread_mutex_unlock(&g_sys.lock);
}

/* First sample before the server accepts requests, so /api/system is never empty. */
static void sampler_init(void)
{
    si_cpu_sample_read(&g_prev_sample);
    sleep_ms(250);
    sample_once(1);
}

static void *sampler_thread(void *arg)
{
    (void)arg;
    for (unsigned tick = 1;; tick++) {
        sleep_ms(1000);
        sample_once(tick % 2 == 0);
    }
    return NULL;
}

/* ---------- loaded scans, kept in memory between requests ---------- */

#define MAX_LOADED 4

typedef struct {
    int used;
    char file[PATH_MAX];
    int64_t mtime_ns;
    int64_t fsize;
    uint64_t last_use;
    ds_scan scan;
} loaded_scan;

static loaded_scan g_loaded[MAX_LOADED];
static uint64_t g_use_counter;
static pthread_mutex_t g_loaded_lock = PTHREAD_MUTEX_INITIALIZER;

/* Must be called with g_loaded_lock held. The result stays valid until the lock is released. */
static ds_scan *get_scan_locked(const char *file)
{
    u8_statbuf st;
    if (u8_stat(file, &st) != 0)
        return NULL;
    loaded_scan *slot = NULL;
    for (int i = 0; i < MAX_LOADED; i++) {
        if (g_loaded[i].used && strcmp(g_loaded[i].file, file) == 0) {
            slot = &g_loaded[i];
            break;
        }
    }
    if (slot && slot->fsize == st.size && slot->mtime_ns == st.mtime_ns) {
        slot->last_use = ++g_use_counter;
        return &slot->scan;
    }
    if (!slot) {
        slot = &g_loaded[0];
        for (int i = 0; i < MAX_LOADED; i++) {
            if (!g_loaded[i].used) {
                slot = &g_loaded[i];
                break;
            }
            if (g_loaded[i].last_use < slot->last_use)
                slot = &g_loaded[i];
        }
    }
    if (slot->used)
        ds_scan_free(&slot->scan);
    slot->used = 0;
    if (ds_cache_load(file, &slot->scan) != 0)
        return NULL;
    slot->used = 1;
    snprintf(slot->file, sizeof slot->file, "%s", file);
    slot->mtime_ns = st.mtime_ns;
    slot->fsize = st.size;
    slot->last_use = ++g_use_counter;
    return &slot->scan;
}

/* ---------- background scan ---------- */

static struct {
    pthread_mutex_t lock;
    int running;
    char path[PATH_MAX];
    uint64_t items;
    uint64_t bytes;
    int64_t started_at;
    int64_t finished_at;
    char error[512];
} g_scan = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void scan_progress(uint64_t items, uint64_t bytes, void *ud)
{
    (void)ud;
    pthread_mutex_lock(&g_scan.lock);
    g_scan.items = items;
    g_scan.bytes = bytes;
    pthread_mutex_unlock(&g_scan.lock);
}

static void *scan_thread(void *arg)
{
    char *path = arg;
    ds_scan_opts opts = { .progress = scan_progress };
    ds_scan s;
    char err[512] = "";
    if (ds_scan_path(path, &opts, &s, err, sizeof err) == 0) {
        ds_cache_save(&s, NULL, 0, err, sizeof err);
        ds_scan_free(&s);
    }
    pthread_mutex_lock(&g_scan.lock);
    g_scan.running = 0;
    g_scan.finished_at = (int64_t)time(NULL);
    snprintf(g_scan.error, sizeof g_scan.error, "%s", err);
    pthread_mutex_unlock(&g_scan.lock);
    printf("scan of %s finished%s%s\n", path, err[0] ? ": " : "", err);
    fflush(stdout);
    free(path);
    return NULL;
}

/* ---------- HTTP plumbing ---------- */

typedef struct {
    char method[16];
    char path[2048];
    char query[4096];
} http_req;

static const char *status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 431: return "Request Header Fields Too Large";
    default: return "Internal Server Error";
    }
}

static void send_all(sock_t fd, const char *p, size_t n)
{
    while (n) {
        int chunk = n > 1 << 30 ? 1 << 30 : (int)n;
        int w = (int)send(fd, p, chunk, SEND_FLAGS);
        if (w < 0 && sock_error() == EINTR)
            continue;
        if (w <= 0)
            return;
        p += w;
        n -= (size_t)w;
    }
}

static void respond(sock_t fd, const http_req *req, int code, const char *ctype, const char *body,
                    size_t len)
{
    char hdr[512];
    int h = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     code, status_text(code), ctype, len);
    send_all(fd, hdr, (size_t)h);
    if (len)
        send_all(fd, body, len);
    printf("%s %s %d\n", req ? req->method : "-", req ? req->path : "-", code);
    fflush(stdout);
}

static void respond_sb(sock_t fd, const http_req *req, int code, sbuf *b)
{
    respond(fd, req, code, "application/json; charset=utf-8", b->data, b->len);
    sb_free(b);
}

static void respond_error(sock_t fd, const http_req *req, int code, const char *msg, const char *path)
{
    sbuf b;
    sb_init(&b);
    sb_puts(&b, "{\"error\":");
    sb_json_str(&b, msg);
    if (path) {
        sb_puts(&b, ",\"path\":");
        sb_json_str(&b, path);
    }
    sb_puts(&b, "}");
    respond_sb(fd, req, code, &b);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *s, size_t n, char *out, size_t outlen, int plus_space)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < outlen; i++) {
        if (s[i] == '%' && i + 2 < n && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            out[o++] = (char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        } else if (s[i] == '+' && plus_space) {
            out[o++] = ' ';
        } else {
            out[o++] = s[i];
        }
    }
    out[o] = '\0';
}

/* Copies the decoded value of query parameter name into out. Returns 1 if present. */
static int get_param(const char *query, const char *name, char *out, size_t outlen)
{
    const char *p = query;
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end)
            end = p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        const char *kend = eq ? eq : end;
        char key[64];
        url_decode(p, (size_t)(kend - p), key, sizeof key, 1);
        if (strcmp(key, name) == 0) {
            if (eq)
                url_decode(eq + 1, (size_t)(end - eq - 1), out, outlen, 1);
            else
                out[0] = '\0';
            return 1;
        }
        p = *end ? end + 1 : end;
    }
    return 0;
}

static long get_param_long(const char *query, const char *name, long def, long min, long max)
{
    char v[32];
    if (!get_param(query, name, v, sizeof v) || !v[0])
        return def;
    long n = strtol(v, NULL, 10);
    return n < min ? min : n > max ? max : n;
}

static int parse_request(char *buf, http_req *req)
{
    char *sp1 = strchr(buf, ' ');
    if (!sp1 || (size_t)(sp1 - buf) >= sizeof req->method)
        return -1;
    char *target = sp1 + 1;
    char *sp2 = strchr(target, ' ');
    if (!sp2)
        return -1;
    memcpy(req->method, buf, (size_t)(sp1 - buf));
    req->method[sp1 - buf] = '\0';
    char *q = memchr(target, '?', (size_t)(sp2 - target));
    char *pend = q ? q : sp2;
    url_decode(target, (size_t)(pend - target), req->path, sizeof req->path, 0);
    req->query[0] = '\0';
    if (q) {
        size_t ql = (size_t)(sp2 - q - 1);
        if (ql >= sizeof req->query)
            ql = sizeof req->query - 1;
        memcpy(req->query, q + 1, ql);
        req->query[ql] = '\0';
    }
    return 0;
}

/* ---------- handlers ---------- */

static void path_param(const http_req *req, char *abs, size_t len)
{
    char raw[PATH_MAX] = "/";
    get_param(req->query, "path", raw, sizeof raw);
    if (!raw[0] || normalize_path(raw, abs, len) != 0)
        snprintf(abs, len, "/");
}

static void handle_tree(sock_t fd, const http_req *req)
{
    char abs[PATH_MAX];
    path_param(req, abs, sizeof abs);
    ds_json_opts o = {
        .depth = (int)get_param_long(req->query, "depth", 3, 0, 32),
        .limit = (uint32_t)get_param_long(req->query, "limit", 1000, 0, 1000000),
        .min_ratio = 0.0005,
    };
    char v[32];
    if (get_param(req->query, "min", v, sizeof v) && v[0])
        o.min_ratio = strtod(v, NULL);

    ds_cache_entry e;
    if (ds_cache_find(abs, &e) != 0) {
        respond_error(fd, req, 404, "no scan covers this path yet; start a scan first", abs);
        return;
    }
    sbuf b;
    sb_init(&b);
    pthread_mutex_lock(&g_loaded_lock);
    ds_scan *s = get_scan_locked(e.file);
    ds_node *n = s ? ds_find(s, abs) : NULL;
    if (n)
        ds_json_scan(&b, s, n, abs, &o);
    pthread_mutex_unlock(&g_loaded_lock);
    ds_cache_entry_free(&e);

    if (!s)
        respond_error(fd, req, 500, "cannot read the scan cache", abs);
    else if (!n)
        respond_error(fd, req, 404, "path was not found in the last scan", abs);
    else {
        respond_sb(fd, req, 200, &b);
        return;
    }
    sb_free(&b);
}

static void handle_scans(sock_t fd, const http_req *req)
{
    ds_cache_entry *list;
    size_t n;
    if (ds_cache_list(&list, &n) != 0) {
        respond_error(fd, req, 500, "cannot read the cache directory", NULL);
        return;
    }
    sbuf b;
    sb_init(&b);
    ds_json_cache_list(&b, list, n);
    ds_cache_list_free(list, n);
    respond_sb(fd, req, 200, &b);
}

static void status_json(sbuf *b)
{
    pthread_mutex_lock(&g_scan.lock);
    sb_printf(b, "{\"running\":%s,\"path\":", g_scan.running ? "true" : "false");
    sb_json_str(b, g_scan.path);
    sb_printf(b,
              ",\"items\":%" PRIu64 ",\"bytes\":%" PRIu64 ",\"started_at\":%" PRId64
              ",\"finished_at\":%" PRId64 ",\"error\":",
              g_scan.items, g_scan.bytes, g_scan.started_at, g_scan.finished_at);
    if (g_scan.error[0])
        sb_json_str(b, g_scan.error);
    else
        sb_puts(b, "null");
    sb_puts(b, "}");
    pthread_mutex_unlock(&g_scan.lock);
}

static void handle_scan(sock_t fd, const http_req *req)
{
    char abs[PATH_MAX];
    path_param(req, abs, sizeof abs);
    u8_statbuf st;
    if (u8_stat(abs, &st) != 0) {
        respond_error(fd, req, 404, "no such file or directory", abs);
        return;
    }
    pthread_mutex_lock(&g_scan.lock);
    if (g_scan.running) {
        char busy[PATH_MAX];
        snprintf(busy, sizeof busy, "%s", g_scan.path);
        pthread_mutex_unlock(&g_scan.lock);
        respond_error(fd, req, 409, "a scan is already running", busy);
        return;
    }
    g_scan.running = 1;
    snprintf(g_scan.path, sizeof g_scan.path, "%s", abs);
    g_scan.items = g_scan.bytes = 0;
    g_scan.started_at = (int64_t)time(NULL);
    g_scan.finished_at = 0;
    g_scan.error[0] = '\0';
    pthread_mutex_unlock(&g_scan.lock);

    pthread_t t;
    char *arg = xstrdup(abs);
    if (pthread_create(&t, NULL, scan_thread, arg) != 0) {
        free(arg);
        pthread_mutex_lock(&g_scan.lock);
        g_scan.running = 0;
        pthread_mutex_unlock(&g_scan.lock);
        respond_error(fd, req, 500, "cannot start scan thread", abs);
        return;
    }
    pthread_detach(t);
    sbuf b;
    sb_init(&b);
    status_json(&b);
    respond_sb(fd, req, 202, &b);
}

static void handle_system(sock_t fd, const http_req *req)
{
    si_snapshot s;
    si_snapshot_base(&s);
    pthread_mutex_lock(&g_sys.lock);
    si_cpu_copy(&s.cpu, &g_sys.cpu);
    s.ngpus = g_sys.ngpus;
    if (s.ngpus) {
        s.gpus = xmalloc(s.ngpus * sizeof *s.gpus);
        memcpy(s.gpus, g_sys.gpus, s.ngpus * sizeof *s.gpus);
    }
    pthread_mutex_unlock(&g_sys.lock);
    sbuf b;
    sb_init(&b);
    si_snapshot_json(&b, &s);
    si_snapshot_free(&s);
    respond_sb(fd, req, 200, &b);
}

static void handle_history(sock_t fd, const http_req *req)
{
    sbuf b;
    sb_init(&b);
    sb_puts(&b, "{\"samples\":[");
    pthread_mutex_lock(&g_sys.lock);
    size_t start = (g_hist_next + HISTORY_LEN - g_hist_len) % HISTORY_LEN;
    for (size_t i = 0; i < g_hist_len; i++) {
        size_t k = (start + i) % HISTORY_LEN;
        sb_printf(&b, "%s{\"t\":%" PRId64 ",\"cpu\":%.1f,\"mem\":%.1f}", i ? "," : "", g_hist[k].t,
                  g_hist[k].cpu, g_hist[k].mem);
    }
    pthread_mutex_unlock(&g_sys.lock);
    sb_puts(&b, "]}");
    respond_sb(fd, req, 200, &b);
}

static void handle(sock_t fd, const http_req *req)
{
    int get = strcmp(req->method, "GET") == 0;
    int post = strcmp(req->method, "POST") == 0;
    const char *p = req->path;

    if (strcmp(req->method, "OPTIONS") == 0) {
        respond(fd, req, 204, "text/plain", "", 0);
    } else if (strcmp(p, "/") == 0 || strcmp(p, "/index.html") == 0) {
        if (!get)
            respond_error(fd, req, 405, "use GET", NULL);
        else
            respond(fd, req, 200, "text/html; charset=utf-8", (const char *)web_index_html,
                    web_index_html_len);
    } else if (strcmp(p, "/dirstat") == 0 || strcmp(p, "/dirstat/") == 0) {
        if (!get)
            respond_error(fd, req, 405, "use GET", NULL);
        else
            respond(fd, req, 200, "text/html; charset=utf-8", (const char *)web_dirstat_html,
                    web_dirstat_html_len);
    } else if (strcmp(p, "/api/system/history") == 0) {
        get ? handle_history(fd, req) : respond_error(fd, req, 405, "use GET", NULL);
    } else if (strcmp(p, "/api/system") == 0) {
        get ? handle_system(fd, req) : respond_error(fd, req, 405, "use GET", NULL);
    } else if (strcmp(p, "/api/dirstat") == 0) {
        get ? handle_tree(fd, req) : respond_error(fd, req, 405, "use GET", NULL);
    } else if (strcmp(p, "/api/dirstat/scans") == 0) {
        get ? handle_scans(fd, req) : respond_error(fd, req, 405, "use GET", NULL);
    } else if (strcmp(p, "/api/dirstat/scan") == 0) {
        post ? handle_scan(fd, req) : respond_error(fd, req, 405, "use POST", NULL);
    } else if (strcmp(p, "/api/dirstat/status") == 0) {
        sbuf b;
        sb_init(&b);
        status_json(&b);
        respond_sb(fd, req, 200, &b);
    } else {
        respond_error(fd, req, 404, "not found", NULL);
    }
}

static void *conn_thread(void *arg)
{
    sock_t fd = *(sock_t *)arg;
    free(arg);
#ifdef _WIN32
    DWORD tv = 10000;
#else
    struct timeval tv = { .tv_sec = 10 };
#endif
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);

    char buf[8192];
    size_t n = 0;
    for (;;) {
        int r = (int)recv(fd, buf + n, (int)(sizeof buf - 1 - n), 0);
        if (r <= 0) {
            close_sock(fd);
            return NULL;
        }
        n += (size_t)r;
        buf[n] = '\0';
        if (strstr(buf, "\r\n\r\n"))
            break;
        if (n >= sizeof buf - 1) {
            respond_error(fd, NULL, 431, "request headers too large", NULL);
            close_sock(fd);
            return NULL;
        }
    }
    http_req req;
    if (parse_request(buf, &req) != 0)
        respond_error(fd, NULL, 400, "malformed request", NULL);
    else
        handle(fd, &req);
    close_sock(fd);
    return NULL;
}

static int parse_addr(const char *a, char *host, size_t hl, char *port, size_t pl)
{
    const char *colon;
    host[0] = '\0';
    if (a[0] == '[') {
        const char *rb = strchr(a, ']');
        if (!rb || rb[1] != ':' || (size_t)(rb - a - 1) >= hl)
            return -1;
        memcpy(host, a + 1, (size_t)(rb - a - 1));
        host[rb - a - 1] = '\0';
        colon = rb + 1;
    } else if ((colon = strrchr(a, ':')) != NULL) {
        if ((size_t)(colon - a) >= hl)
            return -1;
        memcpy(host, a, (size_t)(colon - a));
        host[colon - a] = '\0';
    } else {
        colon = a - 1; /* bare port */
    }
    const char *ps = colon + 1;
    if (!*ps || strlen(ps) >= pl)
        return -1;
    for (const char *c = ps; *c; c++)
        if (!isdigit((unsigned char)*c))
            return -1;
    strcpy(port, ps);
    return 0;
}

int server_run(const char *addr)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "stat: cannot initialise Winsock\n");
        return 1;
    }
#endif
    char host[256], port[16];
    if (parse_addr(addr, host, sizeof host, port, sizeof port) != 0) {
        fprintf(stderr, "stat: invalid address '%s' (expected HOST:PORT)\n", addr);
        return 2;
    }
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM,
                              .ai_flags = AI_PASSIVE };
    struct addrinfo *res;
    int rc = getaddrinfo(host[0] ? host : NULL, port, &hints, &res);
    if (rc != 0) {
#ifdef _WIN32
        fprintf(stderr, "stat: %s: %s\n", addr, gai_strerrorA(rc));
#else
        fprintf(stderr, "stat: %s: %s\n", addr, gai_strerror(rc));
#endif
        return 1;
    }
    sock_t lfd = INVALID_SOCKET;
    int err = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        lfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (lfd == INVALID_SOCKET)
            continue;
#ifdef _WIN32
        /* SO_REUSEADDR on Windows lets two servers share a port; this is the safe variant. */
        int one = 1;
        setsockopt(lfd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
#else
        int one = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
        if (bind(lfd, ai->ai_addr, (int)ai->ai_addrlen) == 0 && listen(lfd, 64) == 0)
            break;
        err = sock_error();
        close_sock(lfd);
        lfd = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (lfd == INVALID_SOCKET) {
#ifdef _WIN32
        fprintf(stderr, "stat: cannot listen on %s (Winsock error %d)\n", addr, err);
#else
        fprintf(stderr, "stat: cannot listen on %s: %s\n", addr, strerror(err));
#endif
        return 1;
    }

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    char cache[PATH_MAX];
    ds_cache_dir(cache, sizeof cache);
    int v6 = strchr(host, ':') != NULL;
    printf("stat " STAT_VERSION "\n");
    printf("stat server listening on http://%s%s%s:%s/\n", v6 ? "[" : "",
           host[0] ? host : "0.0.0.0", v6 ? "]" : "", port);
    printf("dirstat: http://%s%s%s:%s/dirstat\n", v6 ? "[" : "", host[0] ? host : "0.0.0.0",
           v6 ? "]" : "", port);
    printf("cache directory: %s\n", cache);
    fflush(stdout);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t sampler;
    sampler_init();
    pthread_create(&sampler, &attr, sampler_thread, NULL);
    for (;;) {
        sock_t c = accept(lfd, NULL, NULL);
        if (c == INVALID_SOCKET) {
            if (sock_error() != EINTR)
                fprintf(stderr, "stat: accept failed (error %d)\n", sock_error());
            continue;
        }
        sock_t *arg = xmalloc(sizeof *arg);
        *arg = c;
        pthread_t t;
        if (pthread_create(&t, &attr, conn_thread, arg) != 0) {
            close_sock(c);
            free(arg);
        }
    }
}
