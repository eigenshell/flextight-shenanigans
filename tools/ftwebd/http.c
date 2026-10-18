/* SPDX-License-Identifier: GPL-2.0-or-later */
/* http.h */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "http.h"

static const char *reason(int code)
{
    switch (code) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    case 500: return "Internal Server Error";
    default: return "Error";
    }
}

int http_send_all(sock_t s, const char *b, size_t n)
{
    while (n) {
        int k = send(s, b, n > 65536 ? 65536 : (int)n, 0);
        if (k <= 0)
            return -1;
        b += k;
        n -= (size_t)k;
    }
    return 0;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* %XX and (form) '+' -> s[0..n) decoded into out; -1 on a bad escape, a
 * NUL or overflow */
static int url_decode(const char *s, size_t n, int plus, char *out,
                      size_t cap)
{
    size_t i, k = 0;
    for (i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        if (c == '%') {
            int h, l;
            if (i + 2 >= n)
                return -1;
            h = hexv(s[i + 1]);
            l = hexv(s[i + 2]);
            if (h < 0 || l < 0)
                return -1;
            c = h * 16 + l;
            i += 2;
        } else if (c == '+' && plus) {
            c = ' ';
        }
        if (!c || k + 1 >= cap)
            return -1;
        out[k++] = (char)c;
    }
    out[k] = 0;
    return 0;
}

int http_param(const char *qs, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    const char *p = qs;
    while (p && *p) {
        const char *amp = strchr(p, '&'), *end = amp ? amp : p + strlen(p);
        if ((size_t)(end - p) > kl && strncmp(p, key, kl) == 0 && p[kl] == '=')
            return url_decode(p + kl + 1, (size_t)(end - p - kl - 1), 1, out,
                              n);
        p = amp ? amp + 1 : NULL;
    }
    return -1;
}

size_t json_esc(char *out, size_t n, const char *s)
{
    size_t k = 0;
    if (n < 3) {
        if (n)
            out[0] = 0;
        return 0;
    }
    out[k++] = '"';
    for (; *s && k + 8 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[k++] = '\\';
            out[k++] = (char)c;
        } else if (c < 0x20) {
            k += (size_t)snprintf(out + k, n - k, "\\u%04x", c);
        } else {
            out[k++] = (char)c;
        }
    }
    out[k++] = '"';
    out[k] = 0;
    return k;
}

/* the header's value (case-insensitive name) in head, or NULL */
static const char *header(const char *head, const char *name, char *out,
                          size_t n)
{
    size_t nl = strlen(name);
    const char *p = strstr(head, "\r\n");
    while (p && p[2]) {
        const char *line = p + 2, *e = strstr(line, "\r\n");
        size_t i;
        if (!e)
            break;
        for (i = 0; i < nl && line + i < e &&
                    tolower((unsigned char)line[i]) == tolower((unsigned char)name[i]);
             i++)
            ;
        if (i == nl && line[nl] == ':') {
            const char *v = line + nl + 1;
            size_t m;
            while (*v == ' ' || *v == '\t')
                v++;
            m = (size_t)(e - v);
            if (m >= n)
                m = n - 1;
            memcpy(out, v, m);
            out[m] = 0;
            return out;
        }
        p = e;
    }
    return NULL;
}

int http_read(http_req *r)
{
    char head[8192], v[64], target[1024];
    size_t k = 0, have, want = 0;
    char *end = NULL, *q;
    long cl = 0;

    r->body = NULL;
    r->nbody = 0;
    r->last_event_id[0] = 0;
    r->if_none_match[0] = 0;
    while (!end) {
        int got;
        if (k + 1 >= sizeof head)
            return 413;
        got = recv(r->s, head + k, (int)(sizeof head - 1 - k), 0);
        if (got <= 0)
            return 400;
        k += (size_t)got;
        head[k] = 0;
        end = strstr(head, "\r\n\r\n");
    }
    if (sscanf(head, "%7s %1023s", r->method, target) != 2)
        return 400;
    if (header(head, "Content-Length", v, sizeof v)) {
        char *e;
        cl = strtol(v, &e, 10);
        if (*e || cl < 0)
            return 400;
        if (cl > HTTP_BODY_MAX)
            return 413;
    }
    if (header(head, "Last-Event-ID", v, sizeof v))
        snprintf(r->last_event_id, sizeof r->last_event_id, "%.23s", v);
    if (header(head, "If-None-Match", v, sizeof v))
        snprintf(r->if_none_match, sizeof r->if_none_match, "%.47s", v);
    q = strchr(target, '?');
    snprintf(r->query, sizeof r->query, "%s", q ? q + 1 : "");
    if (url_decode(target, q ? (size_t)(q - target) : strlen(target), 0,
                   r->path, sizeof r->path))
        return 400;

    want = (size_t)cl;
    r->body = malloc(want + 1);
    if (!r->body)
        return 500;
    have = k - (size_t)(end + 4 - head);
    if (have > want)
        have = want;
    memcpy(r->body, end + 4, have);
    while (have < want) {
        int got = recv(r->s, r->body + have, (int)(want - have), 0);
        if (got <= 0)
            return 400;
        have += (size_t)got;
    }
    r->body[want] = 0;
    r->nbody = want;
    return 0;
}

void http_free(http_req *r)
{
    free(r->body);
    r->body = NULL;
}

void http_send(sock_t s, int code, const char *type, const char *body,
               size_t n)
{
    char h[256];
    int k = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                     "Content-Length: %lu\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n", code, reason(code), type,
                     (unsigned long)n);
    if (http_send_all(s, h, (size_t)k) == 0 && n)
        http_send_all(s, body, n);
}

void http_json(sock_t s, int code, const char *fmt, ...)
{
    static const size_t cap = 256 * 1024;
    char *b = malloc(cap);
    va_list ap;
    int n;
    if (!b)
        return;
    va_start(ap, fmt);
    n = vsnprintf(b, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap)
        http_send(s, 500, "application/json", "{\"error\":\"too long\"}", 20);
    else
        http_send(s, code, "application/json", b, (size_t)n);
    free(b);
}

void http_error(sock_t s, int code, const char *msg)
{
    char m[600];
    json_esc(m, sizeof m, msg);
    http_json(s, code, "{\"error\":%s}", m);
}

static const char *mime(const char *path)
{
    static const char *const map[][2] = {
        {".html", "text/html; charset=utf-8"}, {".js", "text/javascript"},
        {".css", "text/css"}, {".png", "image/png"},
        {".jpg", "image/jpeg"}, {".wasm", "application/wasm"},
        {".json", "application/json"},
        {".txt", "text/plain; charset=utf-8"},
        {".job", "text/plain; charset=utf-8"},
        {".print", "text/plain; charset=utf-8"},
    };
    const char *dot = strrchr(path, '.');
    size_t i;
    for (i = 0; dot && i < sizeof map / sizeof map[0]; i++)
        if (strcmp(dot, map[i][0]) == 0)
            return map[i][1];
    return "application/octet-stream";
}

/* path with cache headers ("Cache-Control: ...\r\n" + more) */
static void send_file(sock_t s, const char *path, const char *cache)
{
    char h[400], buf[65536];
    FILE *f = fopen(path, "rb");
    long n;
    size_t got;
    int k;
    if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET)) {
        if (f)
            fclose(f);
        http_error(s, 404, "no such file");
        return;
    }
    k = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                 "Content-Length: %ld\r\n%sConnection: close\r\n\r\n",
                 mime(path), n, cache);
    if (http_send_all(s, h, (size_t)k) == 0)
        while ((got = fread(buf, 1, sizeof buf, f)) > 0)
            if (http_send_all(s, buf, got))
                break;
    fclose(f);
}

void http_file(sock_t s, const char *path)
{
    send_file(s, path, "Cache-Control: no-store\r\n");
}

void http_static(sock_t s, const char *path, const char *inm)
{
    char tag[48], cache[120];
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) {
        http_error(s, 404, "no such file");
        return;
    }
    snprintf(tag, sizeof tag, "\"%lx-%lx\"", (unsigned long)st.st_size,
             (unsigned long)st.st_mtime);
    snprintf(cache, sizeof cache, "Cache-Control: no-cache\r\nETag: %s\r\n",
             tag);
    if (inm && strcmp(inm, tag) == 0) {
        char h[200];
        int k = snprintf(h, sizeof h, "HTTP/1.1 304 Not Modified\r\n%s"
                         "Connection: close\r\n\r\n", cache);
        http_send_all(s, h, (size_t)k);
        return;
    }
    send_file(s, path, cache);
}

int http_sse_head(sock_t s)
{
    static const char h[] = "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/event-stream\r\n"
                            "Cache-Control: no-store\r\n"
                            "Connection: close\r\n\r\n";
    return http_send_all(s, h, sizeof h - 1);
}
