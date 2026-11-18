/*
 * txtmux —— 多路 TXT 中继管理器（纯 Win32，XP SP1 ~ Windows 11，32/64 位）。
 *
 * 每条规则 = 「一个 DNS TXT 记录 + 一个本地监听端口」：
 *   本机某端口收到连接 → 查 TXT 得到 ip:port → 建连 → 双向转发。
 * TXT 结果按规则缓存 30 秒；解析失败沿用上次成功地址。
 */

#define _WIN32_WINNT 0x0501
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windns.h>
#include <winhttp.h>
#include <commctrl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define APP_TITLE     L"TXT 端口转发 (txtmux)"
#define WM_APP_LOG    (WM_APP + 1)
#define WM_APP_STATE  (WM_APP + 2)
#define IDT_REFRESH   1

#define IDC_ED_RECORD 2001
#define IDC_ED_PORT   2002
#define IDC_LB_RULES  2003
#define IDC_ED_LOG    2004
#define IDC_LB_STATUS 2005
#define IDC_CK_AUTO   2006
#define IDC_BT_ADD    1001
#define IDC_BT_DEL    1002
#define IDC_BT_START  1003
#define IDC_BT_STOP   1004
#define IDC_BT_ENABLE 1005
#define IDC_BT_DISABLE 1006

#define MAX_RULES      32
#define RESOLVE_TTL_MS 30000
#define MAX_LOG_CHARS  16000

typedef struct {
    WCHAR record[256];   /* TXT 记录名，或 http(s):// 开头的入口 URL */
    int kind;            /* 0 = TXT 记录，1 = 入口 URL（跟随 307） */
    int port;
    BOOL enabled;   /* 是否参与「启动全部」/ 自动启动 */
    /* 运行时 */
    SOCKET listenSock;
    HANDLE thread;
    volatile LONG stopFlag;
    volatile LONG running;
    /* 解析缓存 */
    WCHAR targetHost[256];
    int targetPort;
    DWORD targetTick;
    BOOL hasTarget;
} RULE;

static HINSTANCE g_inst;
static HWND g_hwnd, g_edRecord, g_edPort, g_lbRules, g_edLog, g_lbStatus, g_ckAuto;
static WCHAR g_iniPath[MAX_PATH];
static CRITICAL_SECTION g_lock;
static WCHAR *g_logBuf;
static size_t g_logLen, g_logCap;
static RULE *g_rules[MAX_RULES];
static int g_ruleCount;
static BOOL g_autoStart;
static WCHAR g_bindAddr[64] = L"127.0.0.1";   /* 监听地址，ini 里可改成 0.0.0.0 */
static int g_proxyPort = 0;                    /* >0 时额外起 HTTP/SOCKS5 代理 */
static volatile LONG g_shutdown = 0;           /* 退出时通知代理线程 */
static WCHAR g_upstream[128] = L"";            /* PAC 兜底代理，如 127.0.0.1:10809 */

/* ---------- 日志 ---------- */

static void log_line(const WCHAR *fmt, ...)
{
    WCHAR line[1024];
    va_list ap;
    SYSTEMTIME st;

    va_start(ap, fmt);
    _vsnwprintf(line, 1000, fmt, ap);
    va_end(ap);
    GetLocalTime(&st);

    EnterCriticalSection(&g_lock);
    if (!g_logBuf) {
        g_logCap = MAX_LOG_CHARS + 2048;
        g_logBuf = (WCHAR *)calloc(g_logCap, sizeof(WCHAR));
    }
    if (g_logBuf) {
        int n = _snwprintf(g_logBuf + g_logLen, g_logCap - g_logLen - 1,
                           L"%02d:%02d:%02d %s\r\n", st.wHour, st.wMinute, st.wSecond, line);
        if (n > 0) {
            g_logLen += (size_t)n;
            if (g_logLen >= g_logCap - 1) {
                size_t keep = g_logCap / 2;
                memmove(g_logBuf, g_logBuf + (g_logLen - keep), keep * sizeof(WCHAR));
                g_logLen = keep;
                g_logBuf[g_logLen] = 0;
            }
        }
    }
    LeaveCriticalSection(&g_lock);

    if (g_hwnd) PostMessage(g_hwnd, WM_APP_LOG, 0, 0);
}

/* ---------- 小工具 ---------- */

static void get_text(HWND h, WCHAR *out, int cch)
{
    out[0] = 0;
    if (h) GetWindowTextW(h, out, cch);
}

/* ---------- URL 解析 ---------- */

/* 解析 http(s)://host[:port][/path]，返回 host/port，并给出路径起点。
 * 返回 FALSE 表示不是合法 URL。 */
static BOOL parse_url(const WCHAR *url, WCHAR *host, int hostCch, int *port, BOOL *secure, const WCHAR **path)
{
    const WCHAR *p = url;
    const WCHAR *start;
    int i = 0;
    int defaultPort;

    if (!wcsncmp(p, L"https://", 8)) { *secure = TRUE; defaultPort = 443; p += 8; }
    else if (!wcsncmp(p, L"http://", 7)) { *secure = FALSE; defaultPort = 80; p += 7; }
    else return FALSE;

    start = p;
    while (*p && *p != L'/' && *p != L':' && *p != L'?' && *p != L'#') {
        if (i >= hostCch - 1) return FALSE;
        host[i++] = *p++;
    }
    host[i] = 0;
    if (!host[0]) return FALSE;

    *port = defaultPort;
    if (*p == L':') {
        WCHAR num[16];
        int j = 0;
        p++;
        while (*p >= L'0' && *p <= L'9' && j < 15) num[j++] = *p++;
        num[j] = 0;
        if (!j) return FALSE;
        *port = _wtoi(num);
        if (*port <= 0 || *port > 65535) return FALSE;
    }
    if (path) *path = (*p == L'/') ? p : L"/";
    (void)start;
    return TRUE;
}

/* ---------- DNS ---------- */

static BOOL resolve_txt(const WCHAR *record, WCHAR *out, int outCch)
{
    PDNS_RECORD result = NULL;
    DNS_STATUS st;
    BOOL found = FALSE;

    st = DnsQuery_W(record, DNS_TYPE_TEXT, DNS_QUERY_STANDARD, NULL, &result, NULL);
    if (st != 0 || !result) return FALSE;

    {
        PDNS_RECORD rec;
        for (rec = result; rec && !found; rec = rec->pNext) {
            if (rec->wType == DNS_TYPE_TEXT) {
                DNS_TXT_DATAW *txt = (DNS_TXT_DATAW *)&rec->Data;
                if (txt->dwStringCount > 0) {
                    WCHAR value[512];
                    DWORD i;
                    value[0] = 0;
                    for (i = 0; i < txt->dwStringCount && i < 8; i++) {
                        lstrcpynW(value + lstrlenW(value), txt->pStringArray[i], 200);
                    }
                    {
                        WCHAR *s = value, *e;
                        while (*s == L' ' || *s == L'\t') s++;
                        e = s + lstrlenW(s);
                        while (e > s && (e[-1] == L' ' || e[-1] == L'\t' || e[-1] == L'\r' || e[-1] == L'\n')) e--;
                        *e = 0;
                    }
                    if (value[0]) {
                        lstrcpynW(out, value, outCch);
                        found = TRUE;
                    }
                }
            }
        }
    }
    DnsRecordListFree(result, DnsFreeRecordList);
    return found;
}

/* 解析 "host:port" / "[v6]:port" */
static void copy_n(WCHAR *dst, const WCHAR *src, int count, int cch)
{
    if (count >= cch) count = cch - 1;
    if (count < 0) count = 0;
    memcpy(dst, src, (size_t)count * sizeof(WCHAR));
    dst[count] = 0;
}

static BOOL parse_endpoint(const WCHAR *raw, WCHAR *hostOut, int hostCch, int *portOut)
{
    const WCHAR *colon;

    while (*raw == L' ' || *raw == L'\t') raw++;
    if (!raw[0]) return FALSE;

    if (raw[0] == L'[') {
        const WCHAR *close = wcschr(raw, L']');
        if (!close) return FALSE;
        copy_n(hostOut, raw + 1, (int)(close - raw) - 1, hostCch);
        if (close[1] != L':') return FALSE;
        *portOut = _wtoi(close + 2);
    } else {
        colon = wcschr(raw, L':');
        if (colon && wcsrchr(raw, L':') == colon) {
            copy_n(hostOut, raw, (int)(colon - raw), hostCch);
            *portOut = _wtoi(colon + 1);
        } else {
            return FALSE; /* 没端口或裸 IPv6，不支持 */
        }
    }
    return hostOut[0] && *portOut > 0 && *portOut <= 65535;
}

/* 用 WinHTTP 请求入口 URL（自动跟随 307/302），返回最终 URL */
static BOOL fetch_final_url(const WCHAR *url, WCHAR *out, int outCch)
{
    WCHAR host[256];
    const WCHAR *path = NULL;
    int port = 0;
    BOOL secure = FALSE;
    HINTERNET session = NULL, conn = NULL, req = NULL;
    BOOL ok = FALSE;

    if (!parse_url(url, host, 256, &port, &secure, &path)) return FALSE;

    session = WinHttpOpen(L"txtmux/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return FALSE;

    conn = WinHttpConnect(session, host, (INTERNET_PORT)port, 0);
    if (conn) {
        req = WinHttpOpenRequest(conn, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 secure ? WINHTTP_FLAG_SECURE : 0);
        if (req) {
            DWORD sec = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                        SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            DWORD len = 0;

            WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, NULL)) {
                WinHttpQueryOption(req, WINHTTP_OPTION_URL, NULL, &len);
                if (len > 0 && len <= (DWORD)(outCch * sizeof(WCHAR))) {
                    if (WinHttpQueryOption(req, WINHTTP_OPTION_URL, out, &len)) ok = TRUE;
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    return ok;
}

/* 入口 URL → 跳转后的 host:port */
static BOOL resolve_redirect(const WCHAR *url, WCHAR *host, int hostCch, int *port)
{
    WCHAR finalUrl[2048], h[256];
    const WCHAR *path = NULL;
    BOOL secure = FALSE;
    int p = 0;

    if (!fetch_final_url(url, finalUrl, 2048)) return FALSE;
    if (!parse_url(finalUrl, h, 256, &p, &secure, &path)) return FALSE;
    lstrcpynW(host, h, hostCch);
    *port = p;
    if (p != 443 && p != 80)
        log_line(L"跟随跳转: %s", finalUrl);
    return TRUE;
}

/* 取该规则当前目标（带缓存与回退） */
static BOOL rule_target(RULE *r, WCHAR *host, int *port, BOOL *changed)
{
    WCHAR raw[512], h[256];
    int p = 0;

    *changed = FALSE;

    EnterCriticalSection(&g_lock);
    if (r->hasTarget && (GetTickCount() - r->targetTick) < RESOLVE_TTL_MS) {
        lstrcpynW(host, r->targetHost, 256);
        *port = r->targetPort;
        LeaveCriticalSection(&g_lock);
        return TRUE;
    }
    LeaveCriticalSection(&g_lock);

    if ((r->kind == 1) ? resolve_redirect(r->record, h, 256, &p)
                       : (resolve_txt(r->record, raw, 512) && parse_endpoint(raw, h, 256, &p))) {
        EnterCriticalSection(&g_lock);
        if (!r->hasTarget || lstrcmpW(r->targetHost, h) != 0 || r->targetPort != p) *changed = TRUE;
        lstrcpynW(r->targetHost, h, 256);
        r->targetPort = p;
        r->targetTick = GetTickCount();
        r->hasTarget = TRUE;
        LeaveCriticalSection(&g_lock);
        lstrcpynW(host, h, 256);
        *port = p;
        if (*changed) log_line(L"%s -> %s:%d", r->record, h, p);
        return TRUE;
    }

    EnterCriticalSection(&g_lock);
    if (r->hasTarget) {
        lstrcpynW(host, r->targetHost, 256);
        *port = r->targetPort;
        LeaveCriticalSection(&g_lock);
        log_line(L"WARN 解析 %s 失败，沿用 %s:%d", r->record, host, *port);
        return TRUE;
    }
    LeaveCriticalSection(&g_lock);
    return FALSE;
}

/* ---------- 转发 ---------- */

typedef struct {
    SOCKET from;
    SOCKET to;
} COPY_CTX;

static DWORD WINAPI copy_thread(LPVOID param)
{
    COPY_CTX *ctx = (COPY_CTX *)param;
    char buf[8192];
    for (;;) {
        int n = recv(ctx->from, buf, sizeof(buf), 0);
        if (n <= 0) break;
        {
            int sent = 0;
            while (sent < n) {
                int w = send(ctx->to, buf + sent, n - sent, 0);
                if (w <= 0) { sent = n; break; }
                sent += w;
            }
            if (sent != n) break;
        }
    }
    shutdown(ctx->to, SD_SEND);
    return 0;
}

/* 在两个已连通的 socket 之间做双向转发，直到两边都结束 */
static void bridge(SOCKET a, SOCKET b)
{
    COPY_CTX up, down;
    HANDLE th1, th2;

    up.from = a;   up.to = b;
    down.from = b; down.to = a;
    th1 = CreateThread(NULL, 0, copy_thread, &up, 0, NULL);
    th2 = CreateThread(NULL, 0, copy_thread, &down, 0, NULL);
    if (th1) { WaitForSingleObject(th1, INFINITE); CloseHandle(th1); }
    if (th2) { WaitForSingleObject(th2, INFINITE); CloseHandle(th2); }
    closesocket(a);
    closesocket(b);
}

typedef struct {
    RULE *rule;
    SOCKET client;
} CLIENT_CTX;

static DWORD WINAPI client_thread(LPVOID param)
{
    CLIENT_CTX *ctx = (CLIENT_CTX *)param;
    RULE *r = ctx->rule;
    SOCKET client = ctx->client;
    WCHAR host[256];
    int port = 0;
    BOOL changed = FALSE;
    char hostA[256], portA[16];
    struct addrinfo hints, *res = NULL, *p;
    SOCKET upstream = INVALID_SOCKET;

    free(ctx);

    if (!rule_target(r, host, &port, &changed)) {
        log_line(L"[%s] 没有可用目标，丢弃连接", r->record);
        closesocket(client);
        return 0;
    }

    WideCharToMultiByte(CP_ACP, 0, host, -1, hostA, sizeof(hostA), NULL, NULL);
    _snprintf(portA, sizeof(portA), "%d", port);

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;   /* 按目标自身的 A/AAAA 选择，IPv6 不被降级 */
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hostA, portA, &hints, &res) != 0) {
        log_line(L"[%s] 解析目标 %s 失败", r->record, host);
        closesocket(client);
        return 0;
    }

    for (p = res; p; p = p->ai_next) {
        upstream = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (upstream == INVALID_SOCKET) continue;
        if (connect(upstream, p->ai_addr, (int)p->ai_addrlen) == 0) break;
        closesocket(upstream);
        upstream = INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (upstream == INVALID_SOCKET) {
        log_line(L"[%s] 连接 %s:%d 失败", r->record, host, port);
        closesocket(client);
        return 0;
    }

    bridge(client, upstream);
    return 0;
}

static DWORD WINAPI accept_thread(LPVOID param)
{
    RULE *r = (RULE *)param;
    struct sockaddr_in addr;
    SOCKET s;
    BOOL reuse = TRUE;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        log_line(L"[%s] 创建 socket 失败", r->record);
        InterlockedExchange(&r->running, 0);
        return 0;
    }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    if (!lstrcmpW(g_bindAddr, L"0.0.0.0")) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        char bindA[64];
        WideCharToMultiByte(CP_ACP, 0, g_bindAddr, -1, bindA, sizeof(bindA), NULL, NULL);
        addr.sin_addr.s_addr = inet_addr(bindA);
        if (addr.sin_addr.s_addr == INADDR_NONE) addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    addr.sin_port = htons((u_short)r->port);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(s, 16) != 0) {
        log_line(L"[%s] 监听 %s:%d 失败（端口被占用？）", r->record, g_bindAddr, r->port);
        closesocket(s);
        InterlockedExchange(&r->running, 0);
        PostMessage(g_hwnd, WM_APP_STATE, 0, 0);
        return 0;
    }

    r->listenSock = s;
    InterlockedExchange(&r->running, 1);
    log_line(L"[%s] 已监听 %s:%d", r->record, g_bindAddr, r->port);
    PostMessage(g_hwnd, WM_APP_STATE, 0, 0);

    while (!r->stopFlag) {
        fd_set set;
        struct timeval tv;
        SOCKET client;

        FD_ZERO(&set);
        FD_SET(s, &set);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        if (select(0, &set, NULL, NULL, &tv) <= 0) continue;

        client = accept(s, NULL, NULL);
        if (client == INVALID_SOCKET) continue;
        {
            CLIENT_CTX *ctx = (CLIENT_CTX *)calloc(1, sizeof(CLIENT_CTX));
            HANDLE th;
            if (!ctx) { closesocket(client); continue; }
            ctx->rule = r;
            ctx->client = client;
            th = CreateThread(NULL, 0, client_thread, ctx, 0, NULL);
            if (th) CloseHandle(th); else { closesocket(client); free(ctx); }
        }
    }

    closesocket(s);
    r->listenSock = INVALID_SOCKET;
    InterlockedExchange(&r->running, 0);
    PostMessage(g_hwnd, WM_APP_STATE, 0, 0);
    return 0;
}

/* 解析 ini 里的一行规则："record port [enabled]"。enabled 省略时视为 1。 */
static BOOL parse_rule_line(const WCHAR *line, WCHAR *record, int recordCch, int *port, BOOL *enabled)
{
    const WCHAR *p = line;
    const WCHAR *tok;
    int i = 0;
    WCHAR num[32];

    while (*p == L' ' || *p == L'\t') p++;
    if (!*p) return FALSE;

    tok = p;
    while (*p && *p != L' ' && *p != L'\t') p++;
    {
        int len = (int)(p - tok);
        if (len <= 0 || len >= recordCch) return FALSE;
        memcpy(record, tok, (size_t)len * sizeof(WCHAR));
        record[len] = 0;
    }

    while (*p == L' ' || *p == L'\t') p++;
    if (!*p) return FALSE;
    i = 0;
    while (*p && *p != L' ' && *p != L'\t' && i < 31) num[i++] = *p++;
    num[i] = 0;
    *port = _wtoi(num);
    if (*port <= 0 || *port > 65535) return FALSE;

    while (*p == L' ' || *p == L'\t') p++;
    if (*p == L'0') *enabled = FALSE;
    else *enabled = TRUE;
    return TRUE;
}

/* ---------- 本地代理（HTTP CONNECT + SOCKS5）---------- */

/* 在规则里找与 host 匹配的一条：
 * 1) 规则的入口域名就是它（URL 模式），或
 * 2) 规则最近一次解析到的目标域名就是它。 */
static RULE *find_rule_for_host(const WCHAR *host)
{
    int i;
    BOOL changed;

    /* 第一遍：入口域名或已缓存的目标域名匹配 */
    for (i = 0; i < g_ruleCount; i++) {
        RULE *r = g_rules[i];
        WCHAR h[256];
        int p = 0;
        BOOL secure = FALSE;
        const WCHAR *path = NULL;

        if (r->kind == 1 && parse_url(r->record, h, 256, &p, &secure, &path)) {
            if (lstrcmpiW(h, host) == 0) return r;
        }
        EnterCriticalSection(&g_lock);
        if (r->hasTarget && lstrcmpiW(r->targetHost, host) == 0) {
            LeaveCriticalSection(&g_lock);
            return r;
        }
        LeaveCriticalSection(&g_lock);
    }

    /* 第二遍：让 URL 规则先解析一次，再拿解析出来的目标域名比对
     * （例如规则是 https://webdav.example.com/，浏览器访问的是 307 之后的
     *   webdav.gw.example.com，只有解析过才知道是同一个服务） */
    for (i = 0; i < g_ruleCount; i++) {
        RULE *r = g_rules[i];
        WCHAR th[256];
        int tp = 0;

        if (r->kind != 1) continue;
        changed = FALSE;
        if (!rule_target(r, th, &tp, &changed)) continue;
        EnterCriticalSection(&g_lock);
        if (r->hasTarget && lstrcmpiW(r->targetHost, host) == 0) {
            LeaveCriticalSection(&g_lock);
            return r;
        }
        LeaveCriticalSection(&g_lock);
    }
    return NULL;
}

/* 把 host:port 连出去；命中规则就改连规则的当前端点 */
static SOCKET dial_for(const WCHAR *host, int port, BOOL *usedRule)
{
    WCHAR targetHost[256];
    int targetPort = port;
    RULE *r = find_rule_for_host(host);
    char hostA[256], portA[16];
    struct addrinfo hints, *res = NULL, *p;
    SOCKET s = INVALID_SOCKET;

    *usedRule = FALSE;
    if (r) {
        BOOL changed = FALSE;
        if (rule_target(r, targetHost, &targetPort, &changed)) {
            *usedRule = TRUE;
            log_line(L"[代理] %s:%d -> %s:%d", host, port, targetHost, targetPort);
        }
    }
    if (!*usedRule) lstrcpynW(targetHost, host, 256);

    WideCharToMultiByte(CP_ACP, 0, targetHost, -1, hostA, sizeof(hostA), NULL, NULL);
    _snprintf(portA, sizeof(portA), "%d", targetPort);

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;   /* 按目标自身的 A/AAAA 选择，IPv6 不被降级 */
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hostA, portA, &hints, &res) != 0) return INVALID_SOCKET;

    for (p = res; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, p->ai_addr, (int)p->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return s;
}

/* 生成 PAC：命中规则的域名走本代理，其余交给 upstream（比如 v2ray 的 http 端口）*/
static void send_pac(SOCKET client)
{
    char body[8192];
    int used = 0;
    int i;
    char head[128];
    char upstreamA[256] = "";

    if (g_upstream[0]) {
        WideCharToMultiByte(CP_ACP, 0, g_upstream, -1, upstreamA, sizeof(upstreamA), NULL, NULL);
        _snprintf(head, sizeof(head), "PROXY %s; DIRECT", upstreamA);
    } else {
        _snprintf(head, sizeof(head), "DIRECT");
    }

    used += _snprintf(body + used, sizeof(body) - used,
                      "function FindProxyForURL(url, host) {\r\n");

    for (i = 0; i < g_ruleCount && used < (int)sizeof(body) - 400; i++) {
        RULE *r = g_rules[i];
        WCHAR h[256];
        int p = 0;
        BOOL secure = FALSE;
        const WCHAR *path = NULL;
        char hA[256];

        if (r->kind == 1 && parse_url(r->record, h, 256, &p, &secure, &path)) {
            WideCharToMultiByte(CP_ACP, 0, h, -1, hA, sizeof(hA), NULL, NULL);
            used += _snprintf(body + used, sizeof(body) - used,
                              "  if (host == \"%s\") return \"PROXY %s:%d;\";\r\n",
                              hA, "127.0.0.1", g_proxyPort);
        }
        EnterCriticalSection(&g_lock);
        if (r->hasTarget) {
            WideCharToMultiByte(CP_ACP, 0, r->targetHost, -1, hA, sizeof(hA), NULL, NULL);
            used += _snprintf(body + used, sizeof(body) - used,
                              "  if (host == \"%s\") return \"PROXY %s:%d;\";\r\n",
                              hA, "127.0.0.1", g_proxyPort);
        }
        LeaveCriticalSection(&g_lock);
    }

    used += _snprintf(body + used, sizeof(body) - used, "  return \"%s\";\r\n}\r\n", head);
    if (used >= (int)sizeof(body)) used = (int)sizeof(body) - 1;

    {
        char resp[9000];
        int n = _snprintf(resp, sizeof(resp),
                          "HTTP/1.1 200 OK\r\nContent-Type: application/x-ns-proxy-autoconfig\r\n"
                          "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                          used, body);
        if (n > 0) send(client, resp, n, 0);
    }
}

static BOOL read_exact(SOCKET s, char *buf, int len)
{
    int got = 0;
    while (got < len) {
        int n = recv(s, buf + got, len - got, 0);
        if (n <= 0) return FALSE;
        got += n;
    }
    return TRUE;
}

/* 处理一个代理连接：先看头一个字节区分 SOCKS5(0x05) 与 HTTP */
static void handle_proxy_client(SOCKET client)
{
    unsigned char first = 0;
    WCHAR whost[256], wport[16];
    int port = 0;
    SOCKET upstream;
    BOOL usedRule = FALSE;

    if (recv(client, (char *)&first, 1, 0) != 1) { closesocket(client); return; }

    if (first == 0x05) {
        /* ---- SOCKS5 ---- */
        unsigned char buf[512];
        int nmethods, i;
        unsigned char method = 0;

        if (!read_exact(client, (char *)buf, 1)) { closesocket(client); return; }
        nmethods = buf[0];
        for (i = 0; i < nmethods; i++) {
            if (!read_exact(client, (char *)buf, 1)) { closesocket(client); return; }
            if (buf[0] == 0x00) method = 0x00;   /* 只支持 no-auth */
            else if (!method) method = 0xFF;
        }
        {
            char resp[2];
            resp[0] = 0x05;
            resp[1] = (char)(method ? method : 0xFF);
            send(client, resp, 2, 0);
        }
        if (!method) { closesocket(client); return; }

        if (!read_exact(client, (char *)buf, 4)) { closesocket(client); return; }
        if (buf[0] != 0x05 || buf[1] != 0x01) { closesocket(client); return; } /* 只支持 CONNECT */

        if (buf[3] == 0x01) {             /* IPv4 */
            char ip[4];
            char ipText[32];
            if (!read_exact(client, ip, 4)) { closesocket(client); return; }
            _snprintf(ipText, sizeof(ipText), "%u.%u.%u.%u",
                      (unsigned char)ip[0], (unsigned char)ip[1],
                      (unsigned char)ip[2], (unsigned char)ip[3]);
            MultiByteToWideChar(CP_ACP, 0, ipText, -1, whost, 256);
        } else if (buf[3] == 0x03) {      /* 域名 */
            unsigned char len = 0;
            char nameA[256];
            if (!read_exact(client, (char *)&len, 1)) { closesocket(client); return; }
            if (len == 0 || !read_exact(client, nameA, len)) { closesocket(client); return; }
            nameA[len] = 0;
            MultiByteToWideChar(CP_ACP, 0, nameA, -1, whost, 256);
        } else {
            closesocket(client);          /* IPv6 目标暂不支持 */
            return;
        }
        {
            unsigned char pb[2];
            if (!read_exact(client, (char *)pb, 2)) { closesocket(client); return; }
            port = ((int)pb[0] << 8) | pb[1];
        }

        upstream = dial_for(whost, port, &usedRule);
        if (upstream == INVALID_SOCKET) {
            char fail[10] = { 0x05, 0x01, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
            send(client, fail, 10, 0);
            closesocket(client);
            return;
        }
        {
            unsigned char ok[10] = { 0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
            send(client, (char *)ok, 10, 0);
        }
        bridge(client, upstream);
        return;
    }

    /* ---- HTTP 代理：CONNECT host:port HTTP/1.1 ---- */
    {
        char req[2048];
        int used = 0;
        char line[512];
        char *sp1, *sp2;

        req[used++] = (char)first;
        while (used < (int)sizeof(req) - 1) {
            int n = recv(client, req + used, 1, 0);
            if (n <= 0) break;
            used++;
            if (used >= 4 && !memcmp(req + used - 4, "\r\n\r\n", 4)) break;
            if (used >= 2 && req[used - 1] == '\n' && req[used - 2] == '\n') break;
        }
        req[used] = 0;

        /* 只处理首行 */
        {
            int i = 0;
            while (i < used && req[i] != '\r' && req[i] != '\n' && i < (int)sizeof(line) - 1) {
                line[i] = req[i];
                i++;
            }
            line[i] = 0;
        }

        sp1 = strchr(line, ' ');
        if (!sp1) { closesocket(client); return; }
        sp2 = strchr(sp1 + 1, ' ');
        if (!sp2) { closesocket(client); return; }
        *sp2 = 0;

        if (_stricmp(line, "CONNECT") != 0) {
            char method[16], target[512];
            int mi = 0, ti = 0;
            char *sp;

            /* 拆出 METHOD 和 URL，用来识别 PAC 请求 */
            sp = line;
            while (*sp && *sp != ' ' && mi < 15) method[mi++] = *sp++;
            method[mi] = 0;
            while (*sp == ' ') sp++;
            while (*sp && *sp != ' ' && ti < 511) target[ti++] = *sp++;
            target[ti] = 0;

            if (!_stricmp(method, "GET") &&
                (strstr(target, "/proxy.pac") || !strcmp(target, "/"))) {
                send_pac(client);
            } else {
                const char *msg = "HTTP/1.1 405 Method Not Allowed\r\n\r\n";
                send(client, msg, (int)strlen(msg), 0);
            }
            closesocket(client);
            return;
        }

        {
            char *target = sp1 + 1;
            char *colon = strrchr(target, ':');
            char hostA[256];
            if (!colon) { closesocket(client); return; }
            *colon = 0;
            port = atoi(colon + 1);
            lstrcpynA(hostA, target, sizeof(hostA));
            MultiByteToWideChar(CP_ACP, 0, hostA, -1, whost, 256);
        }
        if (!whost[0] || port <= 0) { closesocket(client); return; }

        upstream = dial_for(whost, port, &usedRule);
        if (upstream == INVALID_SOCKET) {
            const char *msg = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
            send(client, msg, (int)strlen(msg), 0);
            closesocket(client);
            return;
        }
        {
            const char *ok = "HTTP/1.1 200 Connection established\r\n\r\n";
            send(client, ok, (int)strlen(ok), 0);
        }
        bridge(client, upstream);
        (void)wport;
    }
}

typedef struct {
    SOCKET listenSock;
    int port;
} PROXY_CTX;

static DWORD WINAPI proxy_client_thread(LPVOID param)
{
    handle_proxy_client((SOCKET)(UINT_PTR)param);
    return 0;
}

static DWORD WINAPI proxy_accept_thread(LPVOID param)
{
    PROXY_CTX *ctx = (PROXY_CTX *)param;
    SOCKET s = ctx->listenSock;

    log_line(L"代理已就绪: %s:%d（HTTP CONNECT / SOCKS5）", g_bindAddr, ctx->port);
    free(ctx);

    while (!g_shutdown) {
        fd_set set;
        struct timeval tv;
        SOCKET client;

        FD_ZERO(&set);
        FD_SET(s, &set);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        if (select(0, &set, NULL, NULL, &tv) <= 0) continue;

        client = accept(s, NULL, NULL);
        if (client == INVALID_SOCKET) continue;
        {
            HANDLE th = CreateThread(NULL, 0, proxy_client_thread,
                                     (LPVOID)(UINT_PTR)client, 0, NULL);
            if (th) CloseHandle(th); else closesocket(client);
        }
    }
    closesocket(s);
    return 0;
}

static void proxy_start(void)
{
    struct sockaddr_in addr;
    SOCKET s;
    BOOL reuse = TRUE;
    PROXY_CTX *ctx;

    if (g_proxyPort <= 0) return;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { log_line(L"代理: 创建 socket 失败"); return; }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    if (!lstrcmpW(g_bindAddr, L"0.0.0.0")) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        char bindA[64];
        WideCharToMultiByte(CP_ACP, 0, g_bindAddr, -1, bindA, sizeof(bindA), NULL, NULL);
        addr.sin_addr.s_addr = inet_addr(bindA);
        if (addr.sin_addr.s_addr == INADDR_NONE) addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    addr.sin_port = htons((u_short)g_proxyPort);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(s, 16) != 0) {
        log_line(L"代理: 监听 %s:%d 失败（端口被占用？）", g_bindAddr, g_proxyPort);
        closesocket(s);
        return;
    }

    ctx = (PROXY_CTX *)calloc(1, sizeof(PROXY_CTX));
    if (!ctx) { closesocket(s); return; }
    ctx->listenSock = s;
    ctx->port = g_proxyPort;
    {
        HANDLE th = CreateThread(NULL, 0, proxy_accept_thread, ctx, 0, NULL);
        if (th) CloseHandle(th); else { closesocket(s); free(ctx); }
    }
}

/* ---------- 规则管理 ---------- */

static void rule_start(RULE *r)
{
    if (r->thread) return;
    r->stopFlag = 0;
    r->thread = CreateThread(NULL, 0, accept_thread, r, 0, NULL);
}

static void rule_stop(RULE *r)
{
    if (!r->thread) return;
    r->stopFlag = 1;
    WaitForSingleObject(r->thread, 3000);
    CloseHandle(r->thread);
    r->thread = NULL;
    r->listenSock = INVALID_SOCKET;
    InterlockedExchange(&r->running, 0);
}

static void refresh_list(void)
{
    int i, sel;
    WCHAR line[512];

    if (!g_lbRules) return;
    sel = (int)SendMessageW(g_lbRules, LB_GETCURSEL, 0, 0);
    SendMessageW(g_lbRules, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_lbRules, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < g_ruleCount; i++) {
        RULE *r = g_rules[i];
        WCHAR state[300];
        if (r->thread && r->running) {
            EnterCriticalSection(&g_lock);
            if (r->hasTarget)
                _snwprintf(state, 300, L"运行中 → %s:%d", r->targetHost, r->targetPort);
            else
                lstrcpynW(state, L"运行中（等待解析）", 300);
            LeaveCriticalSection(&g_lock);
        } else if (r->thread) {
            lstrcpynW(state, L"启动中…", 300);
        } else if (!r->enabled) {
            lstrcpynW(state, L"已禁用", 300);
        } else {
            lstrcpynW(state, L"已停止", 300);
        }
        _snwprintf(line, 512, L"%-28s  :%-6d  %s", r->record, r->port, state);
        SendMessageW(g_lbRules, LB_ADDSTRING, 0, (LPARAM)line);
    }
    SendMessageW(g_lbRules, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_lbRules, NULL, TRUE);
    if (sel >= 0 && sel < g_ruleCount) SendMessageW(g_lbRules, LB_SETCURSEL, sel, 0);
}

static void config_save(void)
{
    WCHAR buf[512];
    int i;

    WritePrivateProfileStringW(L"main", L"autostart", g_autoStart ? L"1" : L"0", g_iniPath);
    _snwprintf(buf, 512, L"%d", g_ruleCount);
    WritePrivateProfileStringW(L"main", L"count", buf, g_iniPath);

    for (i = 0; i < MAX_RULES; i++) {
        WCHAR key[32];
        _snwprintf(key, 32, L"rule%d", i + 1);
        if (i < g_ruleCount) {
            _snwprintf(buf, 512, L"%s %d %d", g_rules[i]->record, g_rules[i]->port,
                       g_rules[i]->enabled ? 1 : 0);
            WritePrivateProfileStringW(L"rules", key, buf, g_iniPath);
        } else {
            WritePrivateProfileStringW(L"rules", key, NULL, g_iniPath);
        }
    }
}

static void config_load(void)
{
    WCHAR buf[512], key[32];
    int count, i;

    g_autoStart = GetPrivateProfileIntW(L"main", L"autostart", 1, g_iniPath) ? TRUE : FALSE;
    {
        WCHAR bindBuf[64];
        GetPrivateProfileStringW(L"main", L"bind", L"127.0.0.1", bindBuf, 64, g_iniPath);
        if (bindBuf[0]) lstrcpynW(g_bindAddr, bindBuf, 64);
    }
    g_proxyPort = GetPrivateProfileIntW(L"main", L"proxy", 0, g_iniPath);
    if (g_proxyPort < 0 || g_proxyPort > 65535) g_proxyPort = 0;
    GetPrivateProfileStringW(L"main", L"upstream", L"", g_upstream, 128, g_iniPath);
    count = GetPrivateProfileIntW(L"main", L"count", 0, g_iniPath);
    if (count > MAX_RULES) count = MAX_RULES;

    g_ruleCount = 0;
    for (i = 0; i < count; i++) {
        WCHAR rec[256];
        int port = 0;
        BOOL enabled = TRUE;

        _snwprintf(key, 32, L"rule%d", i + 1);
        buf[0] = 0;
        GetPrivateProfileStringW(L"rules", key, L"", buf, 512, g_iniPath);
        if (!parse_rule_line(buf, rec, 256, &port, &enabled)) continue;
        {
            RULE *r = (RULE *)calloc(1, sizeof(RULE));
            if (!r) break;
            lstrcpynW(r->record, rec, 256);
            r->kind = (!wcsncmp(rec, L"http://", 7) || !wcsncmp(rec, L"https://", 8)) ? 1 : 0;
            r->port = port;
            r->enabled = enabled;
            r->listenSock = INVALID_SOCKET;
            g_rules[g_ruleCount++] = r;
        }
    }
}

/* ---------- 界面 ---------- */

static HWND mk(const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int h, int id, HFONT font)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, g_hwnd, (HMENU)(INT_PTR)id, g_inst, NULL);
    if (c && font) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static void build_ui(void)
{
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    mk(L"STATIC", L"TXT 记录", SS_LEFT, 18, 20, 76, 20, 0, font);
    g_edRecord = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 96, 18, 300, 24, IDC_ED_RECORD, font);

    mk(L"STATIC", L"本地端口", SS_LEFT, 404, 20, 56, 20, 0, font);
    g_edPort = mk(L"EDIT", L"9000", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, 462, 18, 56, 24, IDC_ED_PORT, font);

    mk(L"BUTTON", L"添加", BS_PUSHBUTTON, 526, 18, 76, 26, IDC_BT_ADD, font);

    g_lbRules = mk(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 18, 54, 588, 168, IDC_LB_RULES, font);

    mk(L"BUTTON", L"启用所选", BS_PUSHBUTTON, 18, 232, 78, 30, IDC_BT_ENABLE, font);
    mk(L"BUTTON", L"停用所选", BS_PUSHBUTTON, 100, 232, 78, 30, IDC_BT_DISABLE, font);
    mk(L"BUTTON", L"删除", BS_PUSHBUTTON, 182, 232, 64, 30, IDC_BT_DEL, font);
    mk(L"BUTTON", L"全部启动", BS_PUSHBUTTON, 250, 232, 78, 30, IDC_BT_START, font);
    mk(L"BUTTON", L"全部停止", BS_PUSHBUTTON, 332, 232, 78, 30, IDC_BT_STOP, font);
    g_ckAuto = mk(L"BUTTON", L"启动时自动连接", BS_AUTOCHECKBOX, 420, 236, 120, 24, IDC_CK_AUTO, font);
    g_lbStatus = mk(L"STATIC", L"", SS_LEFT, 548, 240, 58, 22, IDC_LB_STATUS, font);

    g_edLog = mk(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
                 18, 272, 588, 150, IDC_ED_LOG, font);
}

static void on_add(void)
{
    WCHAR record[256], portText[32];
    int port, i;

    get_text(g_edRecord, record, 256);
    get_text(g_edPort, portText, 32);
    port = _wtoi(portText);

    if (!record[0]) { log_line(L"记录名不能为空"); return; }
    if (port <= 0 || port > 65535) { log_line(L"本地端口不合法"); return; }
    if (g_ruleCount >= MAX_RULES) { log_line(L"最多 %d 条规则", MAX_RULES); return; }

    for (i = 0; i < g_ruleCount; i++) {
        if (g_rules[i]->port == port) { log_line(L"端口 %d 已被规则 #%d 使用", port, i + 1); return; }
        if (lstrcmpiW(g_rules[i]->record, record) == 0) { log_line(L"记录 %s 已存在", record); return; }
    }

    {
        RULE *r = (RULE *)calloc(1, sizeof(RULE));
        if (!r) { log_line(L"内存不足"); return; }
        lstrcpynW(r->record, record, 256);
        r->kind = (!wcsncmp(record, L"http://", 7) || !wcsncmp(record, L"https://", 8)) ? 1 : 0;
        r->port = port;
        r->enabled = TRUE;
        r->listenSock = INVALID_SOCKET;
        g_rules[g_ruleCount] = r;
        g_ruleCount++;
    }
    config_save();
    refresh_list();
    log_line(L"已添加规则: %s -> 127.0.0.1:%d", record, port);
}

static void on_delete(void)
{
    int sel;
    if (!g_lbRules) return;
    sel = (int)SendMessageW(g_lbRules, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= g_ruleCount) { log_line(L"请先在列表里选中一条规则"); return; }

    {
        RULE *r = g_rules[sel];
        rule_stop(r);
        log_line(L"已删除规则: %s :%d", r->record, r->port);
        free(r);
    }
    memmove(&g_rules[sel], &g_rules[sel + 1], (size_t)(g_ruleCount - sel - 1) * sizeof(RULE *));
    g_ruleCount--;
    config_save();
    refresh_list();
}

static int selected_rule(void)
{
    int sel;
    if (!g_lbRules) return -1;
    sel = (int)SendMessageW(g_lbRules, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= g_ruleCount) return -1;
    return sel;
}

static void on_set_enabled(BOOL enable)
{
    int sel = selected_rule();
    RULE *r;

    if (sel < 0) { log_line(L"请先在列表里选中一条规则"); return; }
    r = g_rules[sel];
    r->enabled = enable;
    if (enable) {
        if (!r->thread) rule_start(r);
        log_line(L"已启用: %s :%d", r->record, r->port);
    } else {
        rule_stop(r);
        log_line(L"已停用: %s :%d", r->record, r->port);
    }
    config_save();
    refresh_list();
}

static void on_start_all(void)
{
    int i, n = 0;
    for (i = 0; i < g_ruleCount; i++) {
        if (g_rules[i]->enabled && !g_rules[i]->thread) { rule_start(g_rules[i]); n++; }
    }
    log_line(L"启动 %d 条规则", n);
    refresh_list();
}

static void on_stop_all(void)
{
    int i;
    for (i = 0; i < g_ruleCount; i++) rule_stop(g_rules[i]);
    log_line(L"已停止全部规则");
    refresh_list();
}

static void update_status_text(void)
{
    WCHAR buf[64];
    int running = 0, i;
    for (i = 0; i < g_ruleCount; i++) {
        if (g_rules[i]->thread && g_rules[i]->running) running++;
    }
    _snwprintf(buf, 64, L"%d/%d 运行", running, g_ruleCount);
    SetWindowTextW(g_lbStatus, buf);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        build_ui();
        SetTimer(hwnd, IDT_REFRESH, 2000, NULL);
        return 0;

    case WM_TIMER:
        if (wParam == IDT_REFRESH) {
            update_status_text();
            refresh_list();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BT_ADD:
            on_add();
            return 0;
        case IDC_BT_ENABLE:
            on_set_enabled(TRUE);
            return 0;
        case IDC_BT_DISABLE:
            on_set_enabled(FALSE);
            return 0;
        case IDC_BT_DEL:
            on_delete();
            return 0;
        case IDC_BT_START:
            on_start_all();
            return 0;
        case IDC_BT_STOP:
            on_stop_all();
            return 0;
        case IDC_CK_AUTO:
            g_autoStart = (SendMessageW(g_ckAuto, BM_GETCHECK, 0, 0) == BST_CHECKED);
            config_save();
            return 0;
        case IDC_LB_RULES:
            if (HIWORD(wParam) == LBN_DBLCLK) {
                int sel = selected_rule();
                if (sel >= 0) on_set_enabled(!g_rules[sel]->enabled);
            }
            return 0;
        }
        return 0;

    case WM_APP_LOG: {
        WCHAR *copy = NULL;
        EnterCriticalSection(&g_lock);
        if (g_logBuf) {
            size_t len = wcslen(g_logBuf);
            copy = (WCHAR *)malloc((len + 1) * sizeof(WCHAR));
            if (copy) wcscpy(copy, g_logBuf);
        }
        LeaveCriticalSection(&g_lock);
        if (copy && g_edLog) {
            SetWindowTextW(g_edLog, copy);
            SendMessageW(g_edLog, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
            SendMessageW(g_edLog, EM_SCROLLCARET, 0, 0);
            free(copy);
        }
        return 0;
    }

    case WM_APP_STATE:
        update_status_text();
        refresh_list();
        return 0;

    case WM_CLOSE:
        on_stop_all();
        InterlockedExchange(&g_shutdown, 1);
        config_save();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_REFRESH);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG msg;
    WSADATA wsa;
    WCHAR exe[MAX_PATH];

    (void)prev;
    (void)cmdline;
    g_inst = inst;

    InitializeCriticalSection(&g_lock);

    {
        HMODULE user32 = LoadLibraryW(L"user32.dll");
        if (user32) {
            typedef BOOL (WINAPI *SETDPIAWARE)(void);
            SETDPIAWARE p = (SETDPIAWARE)(void *)GetProcAddress(user32, "SetProcessDPIAware");
            if (p) p();
        }
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    /* ini 放 exe 旁边 */
    {
        DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        WCHAR *slash;
        if (n == 0 || n >= MAX_PATH) exe[0] = 0;
        slash = wcsrchr(exe, L'\\');
        if (slash) *slash = 0;
        _snwprintf(g_iniPath, MAX_PATH - 1, L"%s\\txtmux.ini", exe);
    }

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"TxtMuxWnd";
    if (!RegisterClassExW(&wc)) return 1;

    hwnd = CreateWindowExW(0, L"TxtMuxWnd", APP_TITLE,
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, 640, 476,
                           NULL, NULL, inst, NULL);
    if (!hwnd) return 1;

    config_load();
    SetWindowTextW(g_ckAuto, L"启动时自动连接");
    SendMessageW(g_ckAuto, BM_SETCHECK, g_autoStart ? BST_CHECKED : BST_UNCHECKED, 0);
    refresh_list();
    log_line(L"就绪：添加规则后点「启动全部」（最多 %d 条）", MAX_RULES);

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    if (g_proxyPort > 0) proxy_start();
    if (g_autoStart && g_ruleCount > 0) on_start_all();

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_logBuf) free(g_logBuf);
    DeleteCriticalSection(&g_lock);
    WSACleanup();
    return 0;
}
