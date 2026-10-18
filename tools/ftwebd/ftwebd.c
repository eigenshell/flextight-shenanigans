/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ftwebd: the scanner box's web server. Serves the page, the strip
 * folders and their files, runs whitelisted ftpoc tasks and streams their
 * output (SSE). LAN only, single user, no auth. The box does no image or
 * planning work: the page does (ftweb.wasm); here files are only checked
 * before they are kept.
 *
 *   ftwebd [--port 8080] [--strips strips] [--www www] [--ftpoc ftpoc.exe]
 *          [--archive dir] [--archive-ppi 3150|6300]
 *
 * Run it from ftpoc's own directory: ftpoc reads focus.txt from its
 * current directory, and the page's planner gets it from here.
 *
 *   GET  /, /index.html, /<file>          the page's files (from --www;
 *                                         ETag, 304 while unchanged)
 *   GET  /api/focus.txt                   this scanner's focus fit (404:
 *                                         none, the built-in curve)
 *   GET  /api/strips                      [{name, job, png, print}]
 *   POST /api/strips  name=[&type=quick&ppi=&holder=&emulsion=]
 *                                         create the folder; with the
 *                                         landing page's choices, kept
 *                                         as strip.scan (+ created =)
 *   GET  /api/scans                       recent scans: [] (to come)
 *   GET  /api/strips/{s}/files            [{name, size}]
 *   GET  /api/strips/{s}/files/{f}        one file (Range: bytes=N-:
 *                                         206 with what is there, 416
 *                                         nothing new: the live bar)
 *   GET|PUT /api/strips/{s}/strip.job     whole file (PUT: validated)
 *   GET|PUT /api/strips/{s}/strip.print   whole file (PUT: validated)
 *   GET|PUT /api/strips/{s}/strip.preview.job   (a crop; as strip.job)
 *   GET|PUT /api/strips/{s}/strip.review  "<frame> pending|confirmed|
 *                                         skipped" lines (PUT: checked)
 *   POST /api/tasks  op=preview&strip=&film=neg|slide
 *                    op=scan&strip=[&share=0|1][&arc=1,3-4&film=]
 *                                         202 {id} | 409 busy
 *   GET  /api/tasks/current, /api/tasks/{id}
 *   GET  /api/tasks/{id}/events?since=k   SSE, id = line number
 *   POST /api/tasks/{id}/stop
 *
 * A good preview's job is kept as strip.preview.job: the page plans
 * every size choice from it (the planner excludes frames). */
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <time.h>
#include "ft_emulsion.h"
#include "ft_focus.h"
#include "ft_holder.h"
#include "ft_job.h"
#include "ft_plan.h"
#include "ft_print_file.h"
#include "http.h"
#include "proc.h"
#include "task.h"

static const char *g_strips = "strips", *g_www = "www";
static ft_lens g_lens;
static const char *g_archive, *g_archive_ppi = "6300";
static pthread_mutex_t g_put_mu = PTHREAD_MUTEX_INITIALIZER;

static void sock_close(sock_t s)
{
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

/* strip and file names: [A-Za-z0-9._-], 1..64, no leading '.' */
static int name_ok(const char *s)
{
    size_t i, n = strlen(s);
    if (n < 1 || n > 64 || s[0] == '.')
        return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static long file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode) ? (long)st.st_size : -1;
}

static int make_dir(const char *p)
{
#ifdef _WIN32
    return _mkdir(p) && errno != EEXIST ? -1 : 0;
#else
    return mkdir(p, 0777) && errno != EEXIST ? -1 : 0;
#endif
}

static void api_strips_list(sock_t s)
{
    char *b = malloc(HTTP_BODY_MAX), p[700], nm[160];
    size_t k = 0;
    DIR *d = opendir(g_strips);
    struct dirent *de;
    if (!b) {
        http_error(s, 500, "out of memory");
        if (d)
            closedir(d);
        return;
    }
    b[k++] = '[';
    while (d && (de = readdir(d)) != NULL) {
        if (!name_ok(de->d_name))
            continue;
        snprintf(p, sizeof p, "%s/%s", g_strips, de->d_name);
        if (!is_dir(p) || k + 400 > HTTP_BODY_MAX)
            continue;
        json_esc(nm, sizeof nm, de->d_name);
        snprintf(p, sizeof p, "%s/%s/strip.job", g_strips, de->d_name);
        k += (size_t)snprintf(b + k, HTTP_BODY_MAX - k,
                              "%s{\"name\":%s,\"job\":%s", k > 1 ? "," : "",
                              nm, file_size(p) >= 0 ? "true" : "false");
        snprintf(p, sizeof p, "%s/%s/strip.png", g_strips, de->d_name);
        k += (size_t)snprintf(b + k, HTTP_BODY_MAX - k, ",\"png\":%s",
                              file_size(p) >= 0 ? "true" : "false");
        snprintf(p, sizeof p, "%s/%s/strip.print", g_strips, de->d_name);
        k += (size_t)snprintf(b + k, HTTP_BODY_MAX - k, ",\"print\":%s}",
                              file_size(p) >= 0 ? "true" : "false");
    }
    if (d)
        closedir(d);
    b[k++] = ']';
    http_send(s, 200, "application/json", b, k);
    free(b);
}

/* the landing page's choices (all four, or none: -> 0 with *text "")
 * as strip.scan's text; -1 with msg on a bad one */
static int scan_choices(const char *body, char *text, size_t cap,
                        char *msg, size_t mcap)
{
    char type[16], ppi[16], holder[32], em[40], when[32], *end;
    int have = !http_param(body, "type", type, sizeof type) +
               !http_param(body, "ppi", ppi, sizeof ppi) +
               !http_param(body, "holder", holder, sizeof holder) +
               !http_param(body, "emulsion", em, sizeof em);
    int bins[6];
    double v;
    time_t now = time(NULL);
    text[0] = 0;
    if (have == 0)
        return 0;
    if (have != 4)
        return snprintf(msg, mcap, "type, ppi, holder and emulsion go "
                                   "together"), -1;
    if (strcmp(type, "quick") != 0)
        return snprintf(msg, mcap, "type: only quick scans so far"), -1;
    v = strtod(ppi, &end);
    if (end == ppi || *end || !ft_bins_for_ppi(v, bins))
        return snprintf(msg, mcap, "ppi: %s is out of the optics' range",
                        ppi), -1;
    if (!ft_holder_find(holder))
        return snprintf(msg, mcap, "holder: no holder '%s'", holder), -1;
    if (!ft_emulsion_find(em))
        return snprintf(msg, mcap, "emulsion: no emulsion '%s'", em), -1;
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", localtime(&now));
    snprintf(text, cap, "type = %s\nppi = %s\nholder = %s\nemulsion = %s\n"
                        "created = %s\n", type, ppi, holder, em, when);
    return 0;
}

static void api_strips_create(sock_t s, const http_req *r)
{
    char name[80], p[400], nm[160], scan[300], msg[120];
    const char *body = r->body ? r->body : "";
    FILE *f;
    if (http_param(body, "name", name, sizeof name) || !name_ok(name)) {
        http_error(s, 422, "a strip name is 1..64 of A-Z a-z 0-9 . _ - "
                           "(not starting with .)");
        return;
    }
    if (scan_choices(body, scan, sizeof scan, msg, sizeof msg)) {
        http_error(s, 422, msg);
        return;
    }
    snprintf(p, sizeof p, "%s/%s", g_strips, name);
    if (make_dir(p)) {
        http_error(s, 500, "cannot create the folder");
        return;
    }
    if (scan[0]) {
        snprintf(p, sizeof p, "%s/%s/strip.scan", g_strips, name);
        f = fopen(p, "wb");
        if (!f || fputs(scan, f) < 0 || fclose(f)) {
            http_error(s, 500, "cannot write strip.scan");
            return;
        }
    }
    json_esc(nm, sizeof nm, name);
    http_json(s, 200, "{\"name\":%s}", nm);
}

static void api_files(sock_t s, const char *dir)
{
    char *b = malloc(HTTP_BODY_MAX), p[700], nm[160];
    size_t k = 0;
    DIR *d = opendir(dir);
    struct dirent *de;
    if (!b || !d) {
        free(b);
        if (d)
            closedir(d);
        http_error(s, 404, "no such strip");
        return;
    }
    b[k++] = '[';
    while ((de = readdir(d)) != NULL) {
        long n;
        if (!name_ok(de->d_name) || k + 200 > HTTP_BODY_MAX)
            continue;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if ((n = file_size(p)) < 0)
            continue;
        json_esc(nm, sizeof nm, de->d_name);
        k += (size_t)snprintf(b + k, HTTP_BODY_MAX - k,
                              "%s{\"name\":%s,\"size\":%ld}",
                              k > 1 ? "," : "", nm, n);
    }
    closedir(d);
    b[k++] = ']';
    http_send(s, 200, "application/json", b, k);
    free(b);
}

/* strip.review: "<frame> pending|confirmed|skipped" lines; 0 or -1 with
 * msg naming the line */
static int review_ok(const char *b, size_t n, char *msg, size_t cap)
{
    size_t at = 0;
    int ln = 0;
    while (at < n) {
        char line[64], word[16];
        size_t len = 0;
        int frame, k = -1;
        ln++;
        while (at < n && b[at] != '\n' && len + 1 < sizeof line)
            line[len++] = b[at++];
        line[len] = 0;
        if (at < n && b[at] != '\n')
            return snprintf(msg, cap, "line %d: too long", ln), -1;
        at++;
        if (!len)
            continue;
        if (sscanf(line, "%d %15s%n", &frame, word, &k) != 2 ||
            (size_t)k != len || frame < 0 || frame > 9999 ||
            (strcmp(word, "pending") && strcmp(word, "confirmed") &&
             strcmp(word, "skipped")))
            return snprintf(msg, cap, "line %d: '%.40s' is not <frame> "
                            "pending|confirmed|skipped", ln, line), -1;
    }
    return 0;
}

/* the body checked by the C that reads the file (the job also against
 * this scanner's optics: ftpoc moves by it), then kept as dir/name */
static void api_put(sock_t s, const char *dir, const char *name,
                    const http_req *r)
{
    static ft_job j;
    static ft_print_file pf;
    const char *body = r->body ? r->body : "";
    char p[400], m[120];
    ft_err e;
    FILE *f;
    int rc;
    pthread_mutex_lock(&g_put_mu);
    if (strcmp(name, "strip.review") == 0
            ? review_ok(body, r->nbody, e.msg, sizeof e.msg)
        : strcmp(name, "strip.print") != 0
            ? ft_job_parse(&j, body, r->nbody, &e) ||
              ft_job_validate(&j, &g_lens, &e)
            : ft_print_file_parse(body, r->nbody, &pf, &e)) {
        pthread_mutex_unlock(&g_put_mu);
        http_error(s, 422, e.msg);
        return;
    }
    snprintf(p, sizeof p, "%s/%s", dir, name);
    f = fopen(p, "wb");
    rc = !f;
    if (f) {
        rc = fwrite(body, 1, r->nbody, f) != r->nbody;
        rc |= fclose(f) != 0;
    }
    pthread_mutex_unlock(&g_put_mu);
    snprintf(m, sizeof m, "cannot write %s", name);
    if (rc)
        http_error(s, 500, m);
    else
        http_json(s, 200, "{\"ok\":true}");
}

static void api_task_start(sock_t s, const http_req *r)
{
    char op[16], strip[80], film[16], share[8], arc[80], err[300], p[400];
    const char *body = r->body ? r->body : "";
    task_req rq;
    int id, rc;
    if (http_param(body, "op", op, sizeof op) ||
        http_param(body, "strip", strip, sizeof strip) || !name_ok(strip)) {
        http_error(s, 422, "op and a valid strip name are required");
        return;
    }
    snprintf(p, sizeof p, "%s/%s", g_strips, strip);
    if (!is_dir(p)) {
        http_error(s, 404, "no such strip");
        return;
    }
    rq.op = op;
    rq.strip = strip;
    rq.film = http_param(body, "film", film, sizeof film) ? NULL : film;
    rq.share = http_param(body, "share", share, sizeof share) ||
               strcmp(share, "0") != 0;
    rq.arc = http_param(body, "arc", arc, sizeof arc) || !arc[0] ? NULL
                                                                 : arc;
    if (rq.arc && g_archive && strcmp(op, "scan") == 0) {
        /* <archive>/<strip>/ for ftpoc's -o */
        snprintf(p, sizeof p, "%s/%s", g_archive, strip);
        if (make_dir(g_archive) || make_dir(p)) {
            http_error(s, 500, "cannot create the archive folder");
            return;
        }
    }
    rc = task_start(&rq, &id, err, sizeof err);
    if (rc == 1)
        http_error(s, 409, err);
    else if (rc)
        http_error(s, 422, err);
    else
        http_json(s, 202, "{\"id\":%d}", id);
}

static void api_events(sock_t s, int id, const http_req *r)
{
    char v[24], *buf;
    long since = 0;
    int ended = 0;
    if (http_param(r->query, "since", v, sizeof v) == 0)
        since = atol(v);
    if (r->last_event_id[0])
        since = atol(r->last_event_id);
    if (since < 0)
        since = 0;
    buf = malloc(256 * 1024);
    if (!buf) {
        http_error(s, 500, "out of memory");
        return;
    }
    if (http_sse_head(s) == 0)
        for (;;) {
            long n = task_events(id, &since, buf, 256 * 1024, 15, &ended);
            if (n < 0) {
                static const char gone[] = "event: gone\ndata: \n\n";
                http_send_all(s, gone, sizeof gone - 1);
                break;
            }
            if (n == 0 && !ended) {
                static const char ka[] = ": keepalive\n\n";
                if (http_send_all(s, ka, sizeof ka - 1))
                    break;
                continue;
            }
            if (n > 0 && http_send_all(s, buf, (size_t)n))
                break;
            if (ended) {
                static const char end[] = "event: end\ndata: \n\n";
                http_send_all(s, end, sizeof end - 1);
                break;
            }
        }
    free(buf);
}

/* the files the page writes (PUT, checked) */
static int kept_file(const char *f)
{
    return strcmp(f, "strip.job") == 0 || strcmp(f, "strip.print") == 0 ||
           strcmp(f, "strip.preview.job") == 0 ||
           strcmp(f, "strip.review") == 0;
}

/* path split on '/': seg[0..n) point into a copy */
static int split(const char *path, char *copy, size_t cap, char **seg,
                 int max)
{
    int n = 0;
    char *p;
    snprintf(copy, cap, "%s", path);
    for (p = strtok(copy, "/"); p && n < max; p = strtok(NULL, "/"))
        seg[n++] = p;
    return p ? -1 : n;
}

static void route(http_req *r)
{
    sock_t s = r->s;
    char copy[512], *seg[8], dir[300], p[600], js[600];
    int n = split(r->path, copy, sizeof copy, seg, 8);
    int get = strcmp(r->method, "GET") == 0;
    int post = strcmp(r->method, "POST") == 0;
    int put = strcmp(r->method, "PUT") == 0;

    if (n <= 1 && (n == 0 || name_ok(seg[0]))) {   /* the page's files */
        snprintf(p, sizeof p, "%s/%s", g_www, n ? seg[0] : "index.html");
        if (get)
            http_static(s, p, r->if_none_match);
        else
            http_error(s, 405, "GET only");
        return;
    }
    if (n < 2 || strcmp(seg[0], "api") != 0) {
        http_error(s, 404, "not found");
        return;
    }
    if (n == 2 && strcmp(seg[1], "focus.txt") == 0 && get) {
        http_file(s, "focus.txt");
        return;
    }
    if (n == 2 && strcmp(seg[1], "scans") == 0 && get) {
        http_json(s, 200, "[]");
        return;
    }
    if (strcmp(seg[1], "strips") == 0) {
        if (n == 2 && get) {
            api_strips_list(s);
            return;
        }
        if (n == 2 && post) {
            api_strips_create(s, r);
            return;
        }
        if (n < 4 || !name_ok(seg[2])) {
            http_error(s, 404, "not found");
            return;
        }
        snprintf(dir, sizeof dir, "%s/%s", g_strips, seg[2]);
        if (!is_dir(dir)) {
            http_error(s, 404, "no such strip");
            return;
        }
        if (n == 4 && strcmp(seg[3], "files") == 0 && get)
            api_files(s, dir);
        else if (n == 5 && strcmp(seg[3], "files") == 0 && get &&
                 name_ok(seg[4])) {
            snprintf(p, sizeof p, "%s/%s", dir, seg[4]);
            http_file_from(s, p, r->range_from);
        } else if (n == 4 && kept_file(seg[3]) && get) {
            snprintf(p, sizeof p, "%s/%s", dir, seg[3]);
            http_file(s, p);
        } else if (n == 4 && kept_file(seg[3]) && put)
            api_put(s, dir, seg[3], r);
        else
            http_error(s, 404, "not found");
        return;
    }
    if (strcmp(seg[1], "tasks") == 0) {
        int id;
        if (n == 2 && post) {
            api_task_start(s, r);
            return;
        }
        if (n < 3) {
            http_error(s, 404, "not found");
            return;
        }
        id = strcmp(seg[2], "current") == 0 ? 0 : atoi(seg[2]);
        if (n == 3 && get) {
            task_json(id, js, sizeof js);
            http_send(s, 200, "application/json", js, strlen(js));
        } else if (n == 4 && strcmp(seg[3], "events") == 0 && get && id > 0) {
            api_events(s, id, r);
        } else if (n == 4 && strcmp(seg[3], "stop") == 0 && post && id > 0) {
            int rc = task_stop(id);
            if (rc == -2)
                http_error(s, 404, "no such task");
            else if (rc)
                http_error(s, 409, "not running");
            else
                http_json(s, 200, "{\"ok\":true}");
        } else {
            http_error(s, 404, "not found");
        }
        return;
    }
    http_error(s, 404, "not found");
}

static void *serve(void *arg)
{
    http_req r;
    int code;
    memset(&r, 0, sizeof r);
    r.s = *(sock_t *)arg;
    free(arg);
    code = http_read(&r);
    if (code)
        http_error(r.s, code, "bad request");
    else
        route(&r);
    http_free(&r);
    sock_close(r.s);
    return NULL;
}

static void usage(void)
{
    fprintf(stderr, "usage: ftwebd [--port 8080] [--strips strips] "
                    "[--www www] [--ftpoc ftpoc]\n"
                    "              [--archive dir] [--archive-ppi 3150|6300]"
                    "\n");
}

static void load_lens(void)
{
    FILE *f = fopen("focus.txt", "r");
    ft_err e;
    g_lens = ft_lens_default;
    if (!f)
        return;
    if (ft_lens_read(&g_lens, f, &e)) {
        fprintf(stderr, "focus.txt: %s; using the built-in curve\n", e.msg);
        g_lens = ft_lens_default;
    } else {
        printf("focus.txt: this scanner's fit checks the jobs\n");
    }
    fclose(f);
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    const char *ftpoc = "ftpoc.exe";
    WSADATA wsa;
#else
    const char *ftpoc = "./ftpoc";
#endif
    static char ftpoc_abs[1024];
    struct sockaddr_in a;
    task_cfg cfg;
    sock_t ls;
    int i, port = 8080, one = 1;

    for (i = 1; i < argc; i++) {
        if (i + 1 >= argc) {
            usage();
            return 2;
        }
        if (strcmp(argv[i], "--port") == 0)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--strips") == 0)
            g_strips = argv[++i];
        else if (strcmp(argv[i], "--www") == 0)
            g_www = argv[++i];
        else if (strcmp(argv[i], "--ftpoc") == 0)
            ftpoc = argv[++i];
        else if (strcmp(argv[i], "--archive") == 0)
            g_archive = argv[++i];
        else if (strcmp(argv[i], "--archive-ppi") == 0) {
            g_archive_ppi = argv[++i];
            if (strcmp(g_archive_ppi, "3150") && strcmp(g_archive_ppi, "6300")) {
                usage();
                return 2;
            }
        }
        else {
            usage();
            return 2;
        }
    }
    if (port < 1 || port > 65535) {
        usage();
        return 2;
    }
    if (proc_abspath(ftpoc, ftpoc_abs, sizeof ftpoc_abs)) {
        fprintf(stderr, "ftwebd: no ftpoc at %s (--ftpoc)\n", ftpoc);
        return 1;
    }
    if (make_dir(g_strips)) {
        fprintf(stderr, "ftwebd: cannot create %s\n", g_strips);
        return 1;
    }
    load_lens();
    cfg.ftpoc = ftpoc_abs;
    cfg.strips = g_strips;
    cfg.archive = g_archive;
    cfg.archive_ppi = g_archive_ppi;
    task_init(&cfg);

#ifdef _WIN32
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
        fprintf(stderr, "ftwebd: WSAStartup failed\n");
        return 1;
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)port);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 16)) {
        fprintf(stderr, "ftwebd: cannot listen on port %d\n", port);
        return 1;
    }
    printf("ftwebd: http://<this machine>:%d/  (ftpoc %s, strips in %s)\n",
           port, ftpoc_abs, g_strips);
    fflush(stdout);
    for (;;) {
        pthread_t th;
        sock_t *c = malloc(sizeof *c);
        if (!c)
            continue;
        *c = accept(ls, NULL, NULL);
#ifdef _WIN32
        if (*c == INVALID_SOCKET) {
#else
        if (*c < 0) {
#endif
            free(c);
            continue;
        }
        if (pthread_create(&th, NULL, serve, c)) {
            sock_close(*c);
            free(c);
            continue;
        }
        pthread_detach(th);
    }
}
