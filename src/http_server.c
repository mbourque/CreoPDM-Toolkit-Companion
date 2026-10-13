/* http_server.c - localhost-only HTTP server + main-thread marshalling.
 *
 * Threading: Pro/TOOLKIT is NOT thread-safe; every Toolkit call must run on
 * the Creo main thread. The accept loop runs on a worker thread that never
 * touches Toolkit. user_initialize (main thread) creates a message-only
 * window; each request is PostMessage()d to it and executed in its window
 * procedure (the Creo message pump dispatches it), while the worker waits on
 * an event. Requests are handled strictly one at a time (sequential accept
 * loop), so exports never overlap. If Creo is busy (modal dialog, regen) the
 * wait times out (CREOPDM_TOOLKIT_TIMEOUT_MS, default 30000) and the caller
 * gets {"ok":false,"error":"Creo busy..."}; a late result is discarded.
 */
#include "common.h"

#define WM_CPDM_REQ   (WM_APP + 0x4D2)
#define CPDM_WNDCLASS "CreoPDMToolkitMsgWnd"

typedef struct {
    volatile LONG refs;           /* worker + main thread */
    HANDLE        done;
    char          method[8];
    char          path[256];
    char          query[512];
    int           status;
    Sb            json;
} Req;

static HWND          g_hwnd;
static HANDLE        g_thread;
static SOCKET        g_listen = INVALID_SOCKET;
static volatile LONG g_stop;
static DWORD         g_timeout_ms = 30000;

static void req_release(Req *r)
{
    if (InterlockedDecrement(&r->refs) == 0) {
        CloseHandle(r->done);
        sb_free(&r->json);
        free(r);
    }
}

static LRESULT CALLBACK msg_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CPDM_REQ) {
        Req *r = (Req *)l;
        r->status = route_request(r->method, r->path, r->query, &r->json);   /* main thread */
        SetEvent(r->done);
        req_release(r);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* Returns malloc'd JSON; *status set. */
static char *run_on_main(const char *method, const char *path, const char *query,
                         DWORD timeout, int *status)
{
    Req *r = (Req *)calloc(1, sizeof *r);
    char *res;
    strncpy(r->method, method, sizeof r->method - 1);
    strncpy(r->path, path, sizeof r->path - 1);
    strncpy(r->query, query, sizeof r->query - 1);
    r->refs = 2;
    r->done = CreateEventA(NULL, TRUE, FALSE, NULL);
    *status = 200;
    if (!PostMessageA(g_hwnd, WM_CPDM_REQ, 0, (LPARAM)r)) {
        r->refs = 1;
        req_release(r);
        return _strdup("{\"ok\": false, \"error\": \"Could not reach Creo main thread\"}\n");
    }
    if (WaitForSingleObject(r->done, timeout) == WAIT_OBJECT_0 && r->json.p) {
        res = _strdup(r->json.p);
        *status = r->status;
    } else {
        res = _strdup("{\"ok\": false, \"error\": \"Creo busy (dialog open or regenerating); timed out\"}\n");
        logf_("request timed out: %s %s", method, path);
    }
    req_release(r);
    return res;
}

static void send_response(SOCKET s, int code, const char *body)
{
    char hdr[256];
    size_t bl = strlen(body);
    const char *st = code == 200 ? "OK" : code == 404 ? "Not Found" :
                     code == 405 ? "Method Not Allowed" : "Bad Request";
    int n = _snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %u\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
        code, st, (unsigned)bl);
    send(s, hdr, n, 0);
    while (bl > 0) {
        int chunk = bl > 65536 ? 65536 : (int)bl;
        int sent = send(s, body, chunk, 0);
        if (sent <= 0) break;
        body += sent;
        bl -= sent;
    }
}

static void handle_client(SOCKET c)
{
    char buf[8192];
    int got = 0;
    DWORD to = 5000;
    char method[8] = {0}, target[768] = {0}, *qm, *json;
    int status;
    int is_health;

    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
    while (got < (int)sizeof buf - 1) {
        int n = recv(c, buf + got, (int)sizeof buf - 1 - got, 0);
        if (n <= 0) break;
        got += n;
        buf[got] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }
    if (got <= 0) return;
    buf[got] = 0;
    if (sscanf(buf, "%7s %767s", method, target) != 2) {
        send_response(c, 400, "{\"ok\": false, \"error\": \"bad request\"}\n");
        return;
    }
    qm = strchr(target, '?');
    if (qm) *qm++ = 0;
    if (strlen(target) > 255 || (qm && strlen(qm) > 511)) {
        send_response(c, 400, "{\"ok\": false, \"error\": \"request too long\"}\n");
        return;
    }
    is_health = strcmp(target, "/health") == 0;

    json = run_on_main(method, target, qm ? qm : "", is_health ? 3000 : g_timeout_ms, &status);
    /* /health must still answer when Creo is busy */
    if (is_health && strstr(json, "Creo busy")) {
        free(json);
        json = _strdup("{\"ok\": true, \"service\": \"" CPDM_SERVICE "\", \"version\": \"" CPDM_VERSION
                       "\", \"model\": null, \"busy\": true}\n");
        status = 200;
    }
    send_response(c, status, json);
    free(json);
}

static DWORD WINAPI server_thread(LPVOID arg)
{
    (void)arg;
    while (!g_stop) {
        SOCKET c = accept(g_listen, NULL, NULL);
        if (c == INVALID_SOCKET) {
            if (g_stop) break;
            Sleep(50);
            continue;
        }
        handle_client(c);
        shutdown(c, SD_SEND);
        closesocket(c);
    }
    return 0;
}

static int env_int(const char *name, int dflt)
{
    char b[16];
    if (GetEnvironmentVariableA(name, b, sizeof b) > 0) {
        int v = atoi(b);
        if (v > 0) return v;
        logf_("Ignoring invalid %s '%s'", name, b);
    }
    return dflt;
}

int http_start(void)
{
    WSADATA wsa;
    WNDCLASSA wc;
    struct sockaddr_in addr;
    int port;

    /* CREOPDM_TOOLKIT_PORT wins; CREOPDM_STRUCTURE_PORT kept for the old name. */
    port = env_int("CREOPDM_STRUCTURE_PORT", 8767);
    port = env_int("CREOPDM_TOOLKIT_PORT", port);
    if (port > 65535) { logf_("Port %d out of range, using 8767", port); port = 8767; }
    g_timeout_ms = (DWORD)env_int("CREOPDM_TOOLKIT_TIMEOUT_MS", 30000);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = msg_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = CPDM_WNDCLASS;
    RegisterClassA(&wc);
    g_hwnd = CreateWindowExA(0, CPDM_WNDCLASS, "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!g_hwnd) { logf_("HTTP not started: CreateWindowEx failed (%lu)", GetLastError()); return 0; }

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { logf_("HTTP not started: WSAStartup failed"); return 0; }
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) { logf_("HTTP not started: socket() failed"); return 0; }

    /* No SO_REUSEADDR: on Windows it would let a second instance steal the port. */
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);          /* 127.0.0.1 ONLY */
    if (bind(g_listen, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(g_listen, 4) != 0) {
        logf_("HTTP not started: cannot bind 127.0.0.1:%d (WSA error %d; port in use?). "
              "Ribbon command still works.", port, WSAGetLastError());
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return 0;
    }
    g_thread = CreateThread(NULL, 0, server_thread, NULL, 0, NULL);
    if (!g_thread) { logf_("HTTP not started: CreateThread failed"); return 0; }
    logf_("HTTP listening on 127.0.0.1:%d (timeout %lu ms)", port, (unsigned long)g_timeout_ms);
    return 1;
}

void http_stop(void)
{
    g_stop = 1;
    if (g_listen != INVALID_SOCKET) { closesocket(g_listen); g_listen = INVALID_SOCKET; }
    if (g_thread) { WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); g_thread = NULL; }
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = NULL; }
    WSACleanup();
}
