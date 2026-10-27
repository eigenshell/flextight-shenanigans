/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Just enough HTTP/1.1 for one page on a LAN: one request per connection
 * (Connection: close), bodies by Content-Length, files streamed, SSE. */
#ifndef FTWEBD_HTTP_H
#define FTWEBD_HTTP_H
#include <stddef.h>
#include <stdio.h>

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET sock_t;
#else
typedef int sock_t;
#endif

#define HTTP_BODY_MAX (256 * 1024) /* job and .print files are a few KB */

typedef struct {
  sock_t s;
  char method[8];
  char path[512];  /* decoded, without the query */
  char query[512]; /* raw */
  char last_event_id[24];
  char if_none_match[48];
  long range_from; /* Range: bytes=N- ; -1 = none */
  char* body;      /* NUL-terminated, malloc'd (or NULL) */
  size_t nbody;
} http_req;

/* reads the request head and body; 0, or an HTTP status to answer with */
int http_read(http_req* r);
void http_free(http_req* r);

int http_send_all(sock_t s, const char* b, size_t n);
void http_send(sock_t s, int code, const char* type, const char* body,
               size_t n);
/* body = printf(fmt, ...) as application/json */
void http_json(sock_t s, int code, const char* fmt, ...);
/* {"error": msg} */
void http_error(sock_t s, int code, const char* msg);
/* a whole file, its type from the extension; 404 if missing. from >= 0
 * (a Range: bytes=from- request): 206 with the bytes there now from
 * there, at most HTTP_RANGE_MAX, or 416 when the file ends before it
 * (a file still being written: nothing new yet) */
#define HTTP_RANGE_MAX (1L << 20)
/* path opened "rb"; on Windows sharing read, write and delete, so a
 * writer can rename a new file over path while this reads the old one
 * (task_write_file). NULL if it cannot be opened */
FILE* http_fopen_read(const char* path);
void http_file(sock_t s, const char* path);
void http_file_from(sock_t s, const char* path, long from);
/* the page's own files (www): revalidated by ETag (size and mtime),
 * 304 when inm (the request's If-None-Match) still matches, so the
 * browser keeps ftweb.wasm and asks only whether it changed */
void http_static(sock_t s, const char* path, const char* inm);
/* starts an event stream (text/event-stream, no length) */
int http_sse_head(sock_t s);

/* key's value from a=1&b=2 (query or form body), URL-decoded; 0 or -1 */
int http_param(const char* qs, const char* key, char* out, size_t n);
/* s as a JSON string literal with its quotes (truncated to fit) */
size_t json_esc(char* out, size_t n, const char* s);

#endif
