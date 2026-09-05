/*
 * battery_server - pure C web service exposing POST /api/v1/optimize
 * for battery charge/discharge optimization (MILP core per C strategy).
 *
 * Build: gcc -O2 -Wall -o battery_server.exe main.c json_util.c solver.c -lws2_32
 * Run:   battery_server.exe [port]      (default port 8000)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "json_util.h"
#include "solver.h"

#define DEFAULT_PORT 8000
#define RESPONSE_MAX (1u << 16)

static const char* H200 =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json; charset=utf-8\r\n"
    "Connection: close\r\n"
    "Access-Control-Allow-Origin: *\r\n";

static const char* H400 =
    "HTTP/1.1 400 Bad Request\r\n"
    "Content-Type: application/json; charset=utf-8\r\n"
    "Connection: close\r\n";

static const char* H404 =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: application/json; charset=utf-8\r\n"
    "Connection: close\r\n";

static const char* H500 =
    "HTTP/1.1 500 Internal Server Error\r\n"
    "Content-Type: application/json; charset=utf-8\r\n"
    "Connection: close\r\n";

static int send_all(SOCKET s, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, data + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static void send_json(SOCKET client, const char* status_header, const char* body) {
    char head[512];
    int blen = (int)strlen(body);
    snprintf(head, sizeof(head), "%sContent-Length: %d\r\n\r\n", status_header, blen);
    if (send_all(client, head, (int)strlen(head)) == 0) {
        send_all(client, body, blen);
    }
}

/* find a byte sequence inside a buffer */
static char* find_bytes(const char* hay, int hay_len, const char* needle, int needle_len) {
    if (hay_len < needle_len) return NULL;
    for (int i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, (size_t)needle_len) == 0) {
            return (char*)(hay + i);
        }
    }
    return NULL;
}

static char ci_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int ci_eq(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (ci_lower(a[i]) != ci_lower(b[i])) return 0;
    }
    return 1;
}

static int parse_content_length(const char* headers, int hlen) {
    for (int i = 0; i + 15 <= hlen; i++) {
        if (ci_eq(headers + i, "content-length:", 15)) {
            const char* p = headers + i + 15;
            while (p < headers + hlen && (*p == ' ' || *p == '\t')) p++;
            return atoi(p);
        }
    }
    return -1;
}

static void parse_request_line(const char* buf, int len,
                               char* method, size_t msz,
                               char* path, size_t psz) {
    const char* p = buf;
    const char* end = buf + len;
    while (p < end && *p == ' ') p++;
    const char* q = p;
    while (q < end && *q != ' ' && *q != '\r' && *q != '\n') q++;
    size_t ml = (size_t)(q - p);
    if (ml >= msz) ml = msz - 1;
    memcpy(method, p, ml);
    method[ml] = 0;
    p = q;
    while (p < end && *p == ' ') p++;
    q = p;
    while (q < end && *q != ' ' && *q != '\r' && *q != '\n' && *q != '?') q++;
    size_t pl = (size_t)(q - p);
    if (pl >= psz) pl = psz - 1;
    memcpy(path, p, pl);
    path[pl] = 0;
}

/* read the full request: headers + body (by Content-Length) */
static int read_request(SOCKET client, char** out, int* out_len) {
    int cap = 1 << 16;
    char* buf = (char*)malloc((size_t)cap);
    if (!buf) return -1;
    int total = 0;
    while (1) {
        if (total == cap) {
            cap *= 2;
            if (cap > (1 << 24)) { free(buf); return -1; }
            char* nb = (char*)realloc(buf, (size_t)cap);
            if (!nb) { free(buf); return -1; }
            buf = nb;
        }
        int n = recv(client, buf + total, cap - total, 0);
        if (n <= 0) break;
        total += n;
        char* hdr_end = find_bytes(buf, total, "\r\n\r\n", 4);
        if (hdr_end) {
            int hlen = (int)(hdr_end - buf) + 4;
            int clen = parse_content_length(buf, hlen);
            if (clen < 0) clen = 0;
            if (total >= hlen + clen) break;
        }
    }
    *out = buf;
    *out_len = total;
    return 0;
}
static void handle_connection(SOCKET client) {
    char* buf = NULL;
    int len = 0;
    if (read_request(client, &buf, &len) != 0) {
        closesocket(client);
        return;
    }

    char method[16] = {0};
    char path[512] = {0};
    parse_request_line(buf, len, method, sizeof(method), path, sizeof(path));

    const char* body = NULL;
    int blen = 0;
    char* hdr_end = find_bytes(buf, len, "\r\n\r\n", 4);
    if (hdr_end) {
        int hlen = (int)(hdr_end - buf) + 4;
        int clen = parse_content_length(buf, hlen);
        if (clen > 0 && hlen + clen <= len) {
            body = buf + hlen;
            blen = clen;
        }
    }

    char response[RESPONSE_MAX];

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/v1/optimize") == 0) {
        if (!body) {
            snprintf(response, sizeof(response),
                     "{\"status\":\"error\",\"message\":\"missing request body\"}");
            send_json(client, H400, response);
        } else {
            char* req = (char*)malloc((size_t)blen + 1);
            if (!req) {
                snprintf(response, sizeof(response),
                         "{\"status\":\"error\",\"message\":\"out of memory\"}");
                send_json(client, H500, response);
            } else {
                memcpy(req, body, (size_t)blen);
                req[blen] = 0;
                int rc = run_optimize(req, response, sizeof(response));
                free(req);
                send_json(client, rc == 0 ? H200 : H400, response);
            }
        }
    } else if (strcmp(method, "GET") == 0 &&
               (strcmp(path, "/health") == 0 || strcmp(path, "/api/v1/health") == 0)) {
        snprintf(response, sizeof(response),
                 "{\"status\":\"ok\",\"service\":\"battery_optimizer_c\",\"version\":\"0.1.0\"}");
        send_json(client, H200, response);
    } else {
        snprintf(response, sizeof(response),
                 "{\"status\":\"error\",\"message\":\"not found\",\"path\":\"%s\"}", path);
        send_json(client, H404, response);
    }

    free(buf);
    closesocket(client);
}
int main(int argc, char** argv) {
    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "invalid port: %s\n", argc > 1 ? argv[1] : "");
        return 1;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    if (bind(listener, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind() failed: %d (port %d already in use?)\n",
                WSAGetLastError(), port);
        closesocket(listener);
        WSACleanup();
        return 1;
    }
    if (listen(listener, 16) == SOCKET_ERROR) {
        fprintf(stderr, "listen() failed: %d\n", WSAGetLastError());
        closesocket(listener);
        WSACleanup();
        return 1;
    }

    printf("battery_optimizer_c listening on http://0.0.0.0:%d\n", port);
    printf("POST /api/v1/optimize    GET /health\n");
    fflush(stdout);

    while (1) {
        SOCKET client = accept(listener, NULL, NULL);
        if (client == INVALID_SOCKET) {
            fprintf(stderr, "accept() failed: %d\n", WSAGetLastError());
            continue;
        }
        handle_connection(client);
    }

    closesocket(listener);
    WSACleanup();
    return 0;
}
