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
#include <shellapi.h>
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
#define IDC_ED_MATCH  2007
#define IDC_CK_RUN    2008
#define IDC_CK_TRAY   2009

#define IDM_SHOW      3001
#define IDM_STARTALL  3002
#define IDM_STOPALL   3003
#define IDM_AUTORUN   3004
#define IDM_QUIT      3005

#define WM_TRAYMSG    (WM_APP + 20)
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
    WCHAR match[256];    /* 可选：客户端访问的域名（支持 * 通配），空则用 record/解析结果 */
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
static HWND g_hwnd, g_edRecord, g_edPort, g_lbRules, g_edLog, g_lbStatus, g_ckAuto, g_edMatch;
static HWND g_ckRun, g_ckTray;

/* 自启动（HKCU\...\Run）与托盘 */
static BOOL g_autoRunOn = FALSE;      /* 注册表里当前是否已设置开机自启 */
static BOOL g_closeToTray = TRUE;     /* 关窗口时缩到托盘而不是退出（可关）*/
static BOOL g_startMinimized = FALSE; /* 命令行 -min */
static BOOL g_quitting = FALSE;       /* 正在真正退出，WM_CLOSE 不再缩托盘 */
static HICON g_trayIcon = NULL;
static BOOL g_trayShown = FALSE;
static UINT g_msgTaskbarCreated = 0;  /* explorer.exe 重启后要重新加回托盘图标 */
static HWND g_stRecord, g_stPort, g_stMatch;
static HWND g_btAdd, g_btEnable, g_btDisable, g_btDel, g_btStart, g_btStop;
static HFONT g_monoFont;
static int g_dpi = 96;
static BOOL g_logFollow = TRUE;        /* stuck to the last line unless the user scrolls away */
static int  g_logLastFirst = -1;       /* first visible line right after our own scroll */
static void log_scroll_bottom(void);                 /* system DPI: every layout number is multiplied by dpi/96 */
static WCHAR g_logFilePath[MAX_PATH];
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

static BOOL host_match(const WCHAR *pattern, const WCHAR *host);
static WCHAR g_upstream[128] = L"";            /* 上游代理，如 127.0.0.1:10809 */
static BOOL g_upstreamBlock = FALSE;           /* 上游失败时：TRUE=拒绝，FALSE=直连 */
static int  g_selRule = -1;                    /* 列表里选中并正在编辑的规则下标 */
static BOOL g_sortChanged = FALSE;             /* 启动时有没有因为排序改过顺序 */

static BOOL rule_sort(void);

/* ---------- 日志 ---------- */

/* Mirror a log line to txtmux.log next to the exe (UTF-8). The on-screen
 * log is memory only, so without this there is no way to inspect it. */
/* ---------- log splitting ----------
 * The active log is always txtmux.log. When the day changes it is renamed to
 * txtmux-YYYYMMDD[-n].log. When all txtmux-*.log files together exceed
 * logMaxMB (default 1024) the oldest ones (by name = by date) are deleted
 * until the total is back under the limit. */

#define LOG_TOTAL_MB_DEFAULT 1024
#define LOG_PRUNE_EVERY      256

static WCHAR g_logDir[MAX_PATH];
static char  g_logDate[16];
static int   g_logMaxMB = LOG_TOTAL_MB_DEFAULT;
static int   g_logLines;

typedef struct {
    WCHAR name[MAX_PATH];
    ULONGLONG size;
} LOG_ENTRY;

static void log_today(char *out, int cch)
{
    SYSTEMTIME st;

    GetLocalTime(&st);
    _snprintf(out, (size_t)cch, "%04d%02d%02d", (int)st.wYear, (int)st.wMonth, (int)st.wDay);
    out[cch - 1] = 0;
}

static BOOL log_file_date(const WCHAR *path, char *out, int cch)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    FILETIME local;
    SYSTEMTIME st;

    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return FALSE;
    if (!FileTimeToLocalFileTime(&fad.ftLastWriteTime, &local)) return FALSE;
    if (!FileTimeToSystemTime(&local, &st)) return FALSE;
    _snprintf(out, (size_t)cch, "%04d%02d%02d", (int)st.wYear, (int)st.wMonth, (int)st.wDay);
    out[cch - 1] = 0;
    return TRUE;
}

static ULONGLONG log_file_size(const WCHAR *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;

    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return 0;
    return ((ULONGLONG)fad.nFileSizeHigh << 32) | (ULONGLONG)fad.nFileSizeLow;
}

/* rename the active txtmux.log to txtmux-<date>[-n].log */
static void log_rotate_active(const char *date)
{
    WCHAR wdate[16], target[MAX_PATH];
    int n;

    if (!g_logDir[0]) return;
    MultiByteToWideChar(CP_ACP, 0, date, -1, wdate, 16);
    for (n = 0; n < 1000; n++) {
        if (n == 0) _snwprintf(target, MAX_PATH - 1, L"%s\\txtmux-%s.log", g_logDir, wdate);
        else        _snwprintf(target, MAX_PATH - 1, L"%s\\txtmux-%s-%d.log", g_logDir, wdate, n);
        if (GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES) break;
    }
    if (n >= 1000) return;
    MoveFileExW(g_logFilePath, target, MOVEFILE_REPLACE_EXISTING);
}

static void log_prune(void)
{
    WIN32_FIND_DATAW fd;
    HANDLE h;
    WCHAR pattern[MAX_PATH];
    LOG_ENTRY *items;
    int count = 0, i, j, cap = 512;
    ULONGLONG total, limit;

    if (!g_logDir[0] || g_logMaxMB <= 0) return;
    limit = (ULONGLONG)g_logMaxMB * 1024ULL * 1024ULL;
    items = (LOG_ENTRY *)calloc((size_t)cap, sizeof(LOG_ENTRY));
    if (!items) return;

    _snwprintf(pattern, MAX_PATH - 1, L"%s\\txtmux-*.log", g_logDir);
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (count >= cap) break;
            lstrcpynW(items[count].name, fd.cFileName, MAX_PATH);
            items[count].size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            count++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    /* names sort by YYYYMMDD, so name order is oldest first */
    for (i = 1; i < count; i++) {
        LOG_ENTRY tmp = items[i];
        j = i - 1;
        while (j >= 0 && wcscmp(items[j].name, tmp.name) > 0) {
            items[j + 1] = items[j];
            j--;
        }
        items[j + 1] = tmp;
    }

    total = log_file_size(g_logFilePath);
    for (i = 0; i < count; i++) total += items[i].size;

    for (i = 0; i < count && total > limit; i++) {
        WCHAR full[MAX_PATH];
        _snwprintf(full, MAX_PATH - 1, L"%s\\%s", g_logDir, items[i].name);
        if (DeleteFileW(full)) total -= items[i].size;
    }

    /* only today's file is left and still over the limit: rename it so the
     * next prune pass can delete it */
    if (total > limit && log_file_size(g_logFilePath) > limit / 2) {
        log_rotate_active(g_logDate);
    }
    free(items);
}

/* on day change: archive the old file, then prune the total size */
static void log_ensure_today(void)
{
    char today[16];

    if (!g_logDir[0]) return;
    log_today(today, sizeof(today));
    if (g_logDate[0] && strcmp(today, g_logDate) == 0) return;
    if (g_logDate[0]) log_rotate_active(g_logDate);
    lstrcpynA(g_logDate, today, (int)sizeof(g_logDate));
    log_prune();
}

static void log_init(void)
{
    WCHAR *slash;
    char filedate[16], today[16];

    if (!g_logFilePath[0]) return;
    lstrcpynW(g_logDir, g_logFilePath, MAX_PATH);
    slash = wcsrchr(g_logDir, L'\\');
    if (slash) *slash = 0;

    g_logMaxMB = (int)GetPrivateProfileIntW(L"main", L"logMaxMB", LOG_TOTAL_MB_DEFAULT, g_iniPath);
    if (g_logMaxMB < 0) g_logMaxMB = 0;

    log_today(today, sizeof(today));
    if (log_file_date(g_logFilePath, filedate, sizeof(filedate)) && strcmp(filedate, today) != 0) {
        log_rotate_active(filedate);           /* archive a stale log left over from an earlier run */
    }
    lstrcpynA(g_logDate, today, (int)sizeof(g_logDate));
    log_prune();
}

static void log_to_file(const WCHAR *line)
{
    char utf8[2048];
    FILE *f;
    int n;

    if (!g_logFilePath[0]) return;
    EnterCriticalSection(&g_lock);      /* several threads share one file: write the whole line under the lock */
    log_ensure_today();
    n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, (int)sizeof(utf8) - 1, NULL, NULL);
    if (n > 1) {
        f = _wfopen(g_logFilePath, L"ab");
        if (f) {
            fwrite(utf8, 1, (size_t)(n - 1), f);
            fwrite("\r\n", 1, 2, f);
            fclose(f);
        }
    }
    if (g_logMaxMB > 0 && ++g_logLines >= LOG_PRUNE_EVERY) {
        g_logLines = 0;
        log_prune();
    }
    LeaveCriticalSection(&g_lock);
}

/* scale a 96-dpi design value to the current system DPI */
static int ui(int v)
{
    return MulDiv(v, g_dpi, 96);
}

static void set_bounds(HWND w, int x, int y, int cx, int cy)
{
    if (w) MoveWindow(w, x, y, cx, cy, TRUE);
}

static HFONT create_mono_font(int height)
{
    LOGFONTW lf;

    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -height;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW(lf.lfFaceName, L"Consolas", LF_FACESIZE);
    return CreateFontIndirectW(&lf);
}

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

    log_to_file(line);

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

    /* 关键：不能用 DEFAULT_PROXY。系统代理很可能就是本程序自己，
     * 那样跟随 307 的请求会打回 127.0.0.1 形成自指循环，解析永远失败。 */
    session = WinHttpOpen(L"txtmux/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        log_line(L"WARN WinHTTP 初始化失败: %lu", GetLastError());
        return FALSE;
    }

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
    if (!ok) log_line(L"WARN 跟随跳转失败: %s（WinHTTP 错误 %lu）", url, GetLastError());
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
static BOOL parse_rule_line(const WCHAR *line, WCHAR *record, int recordCch, int *port, BOOL *enabled,
                            WCHAR *match, int matchCch)
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
    {
        /* consume the enabled token first, otherwise the match field below
         * would read it ("1") instead of the domain. */
        const WCHAR *tokEnd = p;
        while (*tokEnd && *tokEnd != L' ' && *tokEnd != L'\t') tokEnd++;
        *enabled = (p[0] == L'0') ? FALSE : TRUE;
        p = tokEnd;
    }

    /* 可选的第四段：匹配域名 */
    if (match && matchCch > 0) {
        const WCHAR *m = p;
        int k = 0;
        match[0] = 0;
        while (*m == L' ' || *m == L'\t') m++;
        while (*m && *m != L' ' && *m != L'\t' && k < matchCch - 1) match[k++] = *m++;
        match[k] = 0;
    }
    return TRUE;
}

/* ---------- 本地代理（HTTP CONNECT + SOCKS5）---------- */

/* 在规则里找与 host 匹配的一条：
 * 1) 规则的入口域名就是它（URL 模式），或
 * 2) 规则最近一次解析到的目标域名就是它。 */
static RULE *find_rule_for_host(const WCHAR *host)
{
    int i;

    /* Only rules that carry a match host take part in proxy routing.
     * A rule without one is a pure local-port forwarder (see README). */
    for (i = 0; i < g_ruleCount; i++) {
        RULE *r = g_rules[i];
        if (r->match[0] && host_match(r->match, host)) return r;
    }
    return NULL;
}

/* 通过 upstream 里的 HTTP 代理建立到 host:port 的隧道（CONNECT） */
static SOCKET dial_via_upstream(const WCHAR *host, int port)
{
    char upA[160], hostA[256], req[600], resp[600];
    char *colon;
    struct addrinfo hints, *res = NULL, *p;
    SOCKET s = INVALID_SOCKET;
    int used = 0;

    if (!g_upstream[0]) return INVALID_SOCKET;

    WideCharToMultiByte(CP_ACP, 0, g_upstream, -1, upA, sizeof(upA), NULL, NULL);
    {
        char *prefix = upA;
        if (!_strnicmp(prefix, "http://", 7)) prefix += 7;
        else if (!_strnicmp(prefix, "socks5://", 9)) prefix += 9;
        if (prefix != upA) memmove(upA, prefix, strlen(prefix) + 1);
    }
    colon = strrchr(upA, ':');
    if (!colon) {
        /* 只写了端口：默认本机 */
        char onlyPort[32];
        lstrcpynA(onlyPort, upA, sizeof(onlyPort));
        _snprintf(upA, sizeof(upA), "127.0.0.1:%s", onlyPort);
        colon = strrchr(upA, ':');
        if (!colon) return INVALID_SOCKET;
    }
    *colon = 0;

    WideCharToMultiByte(CP_ACP, 0, host, -1, hostA, sizeof(hostA), NULL, NULL);

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(upA, colon + 1, &hints, &res) != 0) return INVALID_SOCKET;
    for (p = res; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, p->ai_addr, (int)p->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    _snprintf(req, sizeof(req),
              "CONNECT %s:%d HTTP/1.1\r\nHost: %s:%d\r\nProxy-Connection: keep-alive\r\n\r\n",
              hostA, port, hostA, port);
    if (send(s, req, (int)strlen(req), 0) <= 0) { closesocket(s); return INVALID_SOCKET; }

    /* 读到响应头结束 */
    while (used < (int)sizeof(resp) - 1) {
        int n = recv(s, resp + used, 1, 0);
        if (n <= 0) break;
        used++;
        if (used >= 4 && !memcmp(resp + used - 4, "\r\n\r\n", 4)) break;
    }
    resp[used] = 0;
    if (!strstr(resp, " 200")) {
        log_line(L"[代理] 上游 %s 拒绝隧道 %s:%d（%s）", g_upstream, host, port, resp[0] ? L"非 200" : L"无响应");
        closesocket(s);
        return INVALID_SOCKET;
    }
    log_line(L"[代理] %s:%d -> 上游 %s", host, port, g_upstream);
    return s;
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
    if (!*usedRule) {
        SOCKET viaProxy = dial_via_upstream(host, port);
        if (viaProxy != INVALID_SOCKET) return viaProxy;
        if (g_upstream[0] && g_upstreamBlock) return INVALID_SOCKET;
        if (g_upstream[0]) log_line(L"WARN 上游 %s 不可用，改为直连 %s:%d", g_upstream, host, port);
        lstrcpynW(targetHost, host, 256);
    }

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
        char hA[256];

        /* only rules with a match host appear in the PAC: the others are
         * local-port forwarders and are not dispatched by the browser */
        if (!r->match[0]) continue;
        WideCharToMultiByte(CP_ACP, 0, r->match, -1, hA, (int)sizeof(hA), NULL, NULL);
        if (strchr(hA, '*'))
            used += _snprintf(body + used, sizeof(body) - used,
                              "  if (shExpMatch(host, \"%s\")) return \"PROXY 127.0.0.1:%d;\";\r\n",
                              hA, g_proxyPort);
        else
            used += _snprintf(body + used, sizeof(body) - used,
                              "  if (host == \"%s\") return \"PROXY 127.0.0.1:%d;\";\r\n",
                              hA, g_proxyPort);
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
/* Plain http:// forwarding: rewrite the absolute URL to origin-form, dial it
 * the same way a CONNECT would (rule target / upstream / direct) and copy both
 * directions. Returns TRUE when handled (a response was written), FALSE when
 * this is not a plain-HTTP request we can handle. */
static BOOL forward_plain_http(SOCKET client, const char *req, int used,
                               const char *method, const char *url)
{
    WCHAR wurl[1024], whost[256];
    const WCHAR *wpath = NULL;
    char pathA[1024], hostA[256], out[8192];
    char *hdr;
    const char *p, *end;
    int port = 0, hdrLen, outLen = 0, bodyLen, n;
    BOOL secure = FALSE, usedRule = FALSE;
    SOCKET up;

    if (_strnicmp(url, "http://", 7) != 0) return FALSE;
    MultiByteToWideChar(CP_ACP, 0, url, -1, wurl, 1024);
    if (!parse_url(wurl, whost, 256, &port, &secure, &wpath) || secure) return FALSE;
    WideCharToMultiByte(CP_ACP, 0, wpath, -1, pathA, (int)sizeof(pathA), NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, whost, -1, hostA, (int)sizeof(hostA), NULL, NULL);

    /* target is this proxy itself: refuse, otherwise the request would loop
     * through the upstream back into us */
    if (port == g_proxyPort && (!_stricmp(hostA, "127.0.0.1") || !_stricmp(hostA, "localhost"))) {
        const char *no = "HTTP/1.1 403 Forbidden\r\n\r\n";
        send(client, no, (int)strlen(no), 0);
        return TRUE;
    }

    hdr = strstr(req, "\r\n\r\n");
    if (!hdr) return FALSE;
    hdrLen = (int)(hdr - req) + 4;
    bodyLen = used - hdrLen;
    if (bodyLen < 0) bodyLen = 0;

    log_line(L"[proxy] request(HTTP) %s:%d", whost, port);
    up = dial_for(whost, port, &usedRule);
    if (up == INVALID_SOCKET) {
        const char *bad = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
        send(client, bad, (int)strlen(bad), 0);
        return TRUE;
    }

    /* request line becomes origin-form; headers are copied as-is (minus
     * Proxy-Connection / Connection) and Connection: close is forced to keep
     * the response framing simple */
    n = _snprintf(out, sizeof(out), "%s %s HTTP/1.1\r\n", method, pathA);
    outLen += (n > 0 && n < (int)sizeof(out)) ? n : 0;

    p = strstr(req, "\r\n");
    if (!p) { closesocket(up); return FALSE; }
    p += 2;
    end = req + hdrLen - 4;
    while (p < end && outLen < (int)sizeof(out) - 4) {
        const char *eol = strstr(p, "\r\n");
        int len;
        if (!eol || eol > end) break;
        len = (int)(eol - p);
        if (len > 0 && _strnicmp(p, "Proxy-Connection:", 17) != 0 &&
            _strnicmp(p, "Connection:", 11) != 0) {
            if (outLen + len + 2 < (int)sizeof(out)) {
                memcpy(out + outLen, p, (size_t)len);
                outLen += len;
                out[outLen++] = '\r';
                out[outLen++] = '\n';
            }
        }
        p = eol + 2;
    }
    n = _snprintf(out + outLen, sizeof(out) - outLen, "Connection: close\r\n\r\n");
    outLen += (n > 0 && n < (int)sizeof(out) - outLen) ? n : 0;

    if (bodyLen > 0 && outLen + bodyLen < (int)sizeof(out)) {
        memcpy(out + outLen, req + hdrLen, (size_t)bodyLen);
        outLen += bodyLen;
    }

    if (send(up, out, outLen, 0) <= 0) {
        const char *bad = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
        send(client, bad, (int)strlen(bad), 0);
        closesocket(up);
        return TRUE;
    }
    bridge(client, up);
    return TRUE;
}

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
        int offered = 0;

        if (!read_exact(client, (char *)buf, 1)) { closesocket(client); return; }
        nmethods = buf[0];
        for (i = 0; i < nmethods; i++) {
            if (!read_exact(client, (char *)buf, 1)) { closesocket(client); return; }
            if (buf[0] == 0x00) offered = 1;   /* only no-auth is supported */
        }
        {
            char resp[2];
            resp[0] = 0x05;
            resp[1] = offered ? (char)0x00 : (char)0xFF;
            send(client, resp, 2, 0);
        }
        if (!offered) { closesocket(client); return; }

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

        log_line(L"[proxy] request %s:%d", whost, port);
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

        if (_strnicmp(line, "CONNECT ", 8) != 0) {
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
            } else if (!forward_plain_http(client, req, used, method, target)) {
                log_line(L"[proxy] rejected: unsupported request (405)");
                {
                    const char *msg = "HTTP/1.1 405 Method Not Allowed\r\n\r\n";
                    send(client, msg, (int)strlen(msg), 0);
                }
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

        log_line(L"[proxy] request %s:%d", whost, port);
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

/* 域名通配匹配：pattern 支持 *（任意字符，含点），大小写不敏感 */
static BOOL host_match(const WCHAR *pattern, const WCHAR *host)
{
    const WCHAR *p = pattern, *h = host;

    while (*p) {
        if (*p == L'*') {
            p++;
            if (!*p) return TRUE;
            while (*h) {
                if (host_match(p, h)) return TRUE;
                h++;
            }
            return host_match(p, h);
        }
        if (!*h) return FALSE;
        if (towlower(*p) != towlower(*h)) return FALSE;
        p++;
        h++;
    }
    return *h == 0;
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
    WritePrivateProfileStringW(L"main", L"closeToTray", g_closeToTray ? L"1" : L"0", g_iniPath);
    _snwprintf(buf, 512, L"%d", g_ruleCount);
    WritePrivateProfileStringW(L"main", L"count", buf, g_iniPath);

    for (i = 0; i < MAX_RULES; i++) {
        WCHAR key[32];
        _snwprintf(key, 32, L"rule%d", i + 1);
        if (i < g_ruleCount) {
            if (g_rules[i]->match[0])
                _snwprintf(buf, 512, L"%s %d %d %s", g_rules[i]->record, g_rules[i]->port,
                           g_rules[i]->enabled ? 1 : 0, g_rules[i]->match);
            else
                _snwprintf(buf, 512, L"%s %d %d", g_rules[i]->record, g_rules[i]->port,
                           g_rules[i]->enabled ? 1 : 0);
            WritePrivateProfileStringW(L"rules", key, buf, g_iniPath);
        } else {
            WritePrivateProfileStringW(L"rules", key, NULL, g_iniPath);
        }
    }
}

/* Read an ini value, then cut any inline ";" comment and trailing blanks.
 * GetPrivateProfileString keeps inline comments as part of the value, so
 *   upstream=127.0.0.1:10808   ; note
 * used to arrive as that whole string and broke the upstream handshake. */
static DWORD ini_str(const WCHAR *section, const WCHAR *key, const WCHAR *def,
                     WCHAR *buf, DWORD cch, const WCHAR *file)
{
    DWORD n;
    WCHAR *semi;

    n = GetPrivateProfileStringW(section, key, def, buf, cch, file);
    semi = wcschr(buf, L';');
    if (semi) *semi = 0;
    n = (DWORD)wcslen(buf);
    while (n > 0 && (buf[n - 1] == L' ' || buf[n - 1] == L'\t' ||
                     buf[n - 1] == L'\r' || buf[n - 1] == L'\n')) {
        buf[--n] = 0;
    }
    return n;
}

static void config_load(void)
{
    WCHAR buf[512], key[32];
    int count, i;

    g_autoStart = GetPrivateProfileIntW(L"main", L"autostart", 1, g_iniPath) ? TRUE : FALSE;
    g_closeToTray = GetPrivateProfileIntW(L"main", L"closeToTray", 1, g_iniPath) ? TRUE : FALSE;
    {
        WCHAR bindBuf[64];
        ini_str(L"main", L"bind", L"127.0.0.1", bindBuf, 64, g_iniPath);
        if (bindBuf[0]) lstrcpynW(g_bindAddr, bindBuf, 64);
    }
    g_proxyPort = GetPrivateProfileIntW(L"main", L"proxy", 0, g_iniPath);
    if (g_proxyPort < 0 || g_proxyPort > 65535) g_proxyPort = 0;
    {
        WCHAR mode[32];
        ini_str(L"main", L"upstream", L"", g_upstream, 128, g_iniPath);
        ini_str(L"main", L"upstreamFallback", L"direct", mode, 32, g_iniPath);
        g_upstreamBlock = (_wcsicmp(mode, L"block") == 0);
    }
    count = GetPrivateProfileIntW(L"main", L"count", 0, g_iniPath);
    if (count > MAX_RULES) count = MAX_RULES;

    g_ruleCount = 0;
    for (i = 0; i < count; i++) {
        WCHAR rec[256], m[256];
        int port = 0;
        BOOL enabled = TRUE;

        _snwprintf(key, 32, L"rule%d", i + 1);
        buf[0] = 0;
        ini_str(L"rules", key, L"", buf, 512, g_iniPath);
        if (!parse_rule_line(buf, rec, 256, &port, &enabled, m, 256)) continue;
        {
            RULE *r = (RULE *)calloc(1, sizeof(RULE));
            if (!r) break;
            lstrcpynW(r->record, rec, 256);
            lstrcpynW(r->match, m, 256);
            r->kind = (!wcsncmp(rec, L"http://", 7) || !wcsncmp(rec, L"https://", 8)) ? 1 : 0;
            r->port = port;
            r->enabled = enabled;
            r->listenSock = INVALID_SOCKET;
            g_rules[g_ruleCount++] = r;
        }
    }
    if (rule_sort()) g_sortChanged = TRUE;
}

/* ---------- 开机自启（HKCU\Software\Microsoft\Windows\CurrentVersion\Run） ---------- */

#define AUTORUN_KEY   L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define AUTORUN_VALUE L"txtmux"

static BOOL autorun_get(void)
{
    HKEY k;
    DWORD type = 0, size = 0;
    BOOL on = FALSE;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTORUN_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return FALSE;
    if (RegQueryValueExW(k, AUTORUN_VALUE, NULL, &type, NULL, &size) == ERROR_SUCCESS && size > 0)
        on = TRUE;
    RegCloseKey(k);
    return on;
}

static BOOL autorun_set(BOOL on)
{
    HKEY k;
    LONG rc;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, AUTORUN_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return FALSE;

    if (on) {
        WCHAR exe[MAX_PATH], cmd[MAX_PATH + 16];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        /* -min：开机启动时直接缩到托盘，不弹窗口 */
        _snwprintf(cmd, MAX_PATH + 15, L"\"%s\" -min", exe);
        rc = RegSetValueExW(k, AUTORUN_VALUE, 0, REG_SZ,
                            (const BYTE *)cmd, (DWORD)((lstrlenW(cmd) + 1) * sizeof(WCHAR)));
    } else {
        rc = RegDeleteValueW(k, AUTORUN_VALUE);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

/* ---------- 托盘图标 ---------- */

/* 用 V1 结构（XP~Win11 都认），不用 NOTIFYICONDATAW 的新字段 */
typedef struct {
    DWORD cbSize;
    HWND  hWnd;
    UINT  uID;
    UINT  uFlags;
    UINT  uCallbackMessage;
    HICON hIcon;
    WCHAR szTip[64];
} TRAYICON_V1;

static int tray_running_count(void)
{
    int i, n = 0;
    for (i = 0; i < g_ruleCount; i++)
        if (g_rules[i]->thread && g_rules[i]->running) n++;
    return n;
}

static void tray_update_tip(void)
{
    TRAYICON_V1 td;

    if (!g_trayShown) return;
    ZeroMemory(&td, sizeof(td));
    td.cbSize = sizeof(td);
    td.hWnd = g_hwnd;
    td.uID = 1;
    td.uFlags = NIF_TIP;
    _snwprintf(td.szTip, 63, L"txtmux  %d/%d 运行", tray_running_count(), g_ruleCount);
    Shell_NotifyIconW(NIM_MODIFY, (PNOTIFYICONDATAW)&td);
}

static void tray_add(void)
{
    TRAYICON_V1 td;

    if (g_trayShown || !g_hwnd) return;
    if (!g_trayIcon) {
        int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
        if (cx <= 0) cx = 16;
        if (cy <= 0) cy = 16;
        g_trayIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                       cx, cy, LR_DEFAULTCOLOR);
        if (!g_trayIcon) g_trayIcon = LoadIconW(NULL, IDI_APPLICATION);
    }
    ZeroMemory(&td, sizeof(td));
    td.cbSize = sizeof(td);
    td.hWnd = g_hwnd;
    td.uID = 1;
    td.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    td.uCallbackMessage = WM_TRAYMSG;
    td.hIcon = g_trayIcon;
    _snwprintf(td.szTip, 63, L"txtmux  %d/%d 运行", tray_running_count(), g_ruleCount);
    if (Shell_NotifyIconW(NIM_ADD, (PNOTIFYICONDATAW)&td)) g_trayShown = TRUE;
}

static void tray_remove(void)
{
    TRAYICON_V1 td;

    if (!g_trayShown) return;
    ZeroMemory(&td, sizeof(td));
    td.cbSize = sizeof(td);
    td.hWnd = g_hwnd;
    td.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, (PNOTIFYICONDATAW)&td);
    g_trayShown = FALSE;
}

static void show_main_window(void)
{
    ShowWindow(g_hwnd, SW_SHOW);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
}

static void hide_to_tray(void)
{
    tray_add();
    ShowWindow(g_hwnd, SW_HIDE);
    log_line(L"已最小化到托盘（双击图标恢复，右键菜单可退出）");
}

static void tray_menu(void)
{
    HMENU m = CreatePopupMenu();
    POINT pt;

    if (!m) return;
    AppendMenuW(m, MF_STRING, IDM_SHOW, L"显示主窗口");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_STARTALL, L"启动全部规则");
    AppendMenuW(m, MF_STRING, IDM_STOPALL, L"停止全部规则");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_autoRunOn ? MF_CHECKED : 0), IDM_AUTORUN, L"开机自启");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_QUIT, L"退出");

    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);           /* 不设前台菜单不会自动消失 */
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
}

/* ---------- 界面 ---------- */

static HWND mk(const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int h, int id, HFONT font)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, g_hwnd, (HMENU)(INT_PTR)id, g_inst, NULL);
    if (c && font) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static void layout_ui(int cw, int ch)
{
    int pad = ui(18), row = ui(26), gap = ui(8), vgap = ui(10);
    int y1 = ui(16);
    int y2 = y1 + row + vgap;
    int y3 = y2 + row + ui(14);
    int addW = ui(86), portW = ui(62), portLabW = ui(56), recLabW = ui(78), matchLabW = ui(152);
    int addX = cw - pad - addW;
    int portX = addX - gap - portW;
    int portLabX = portX - ui(2) - portLabW;
    int recX = pad + recLabW;
    int recW = portLabX - gap - recX;
    int btnW = ui(80), btnGap = ui(6);
    int btnY, toggleY, listH, logY, logH, x;

    if (!g_lbRules) return;
    if (recW < ui(120)) recW = ui(120);

    /* 第一行：记录名 + 本地端口 + 添加 */
    set_bounds(g_stRecord, pad, y1 + ui(3), recLabW, ui(20));
    set_bounds(g_edRecord, recX, y1, recW, row);
    set_bounds(g_stPort, portLabX, y1 + ui(3), portLabW, ui(20));
    set_bounds(g_edPort, portX, y1, portW, row);
    set_bounds(g_btAdd, addX, y1, addW, row);

    /* 第二行：匹配域名 */
    set_bounds(g_stMatch, pad, y2 + ui(3), matchLabW, ui(20));
    set_bounds(g_edMatch, pad + matchLabW, y2, cw - pad * 2 - matchLabW, row);

    /* 底部：日志约占可用高度的 35%，按钮行在日志上方，列表吃掉剩余高度 */
    logH = (ch - y3) * 35 / 100;
    if (logH < ui(76)) logH = ui(76);
    if (logH > ch - y3 - ui(150)) logH = ch - y3 - ui(150);
    if (logH < ui(50)) logH = ui(50);
    btnY = ch - pad - logH - vgap - row;
    toggleY = btnY - ui(4) - row;
    listH = toggleY - vgap - y3;
    if (listH < ui(60)) listH = ui(60);

    set_bounds(g_lbRules, pad, y3, cw - pad * 2, listH);

    x = pad;
    set_bounds(g_btEnable, x, btnY, btnW, row);            x += btnW + btnGap;
    set_bounds(g_btDisable, x, btnY, btnW, row);           x += btnW + btnGap;
    set_bounds(g_btDel, x, btnY, ui(64), row);             x += ui(64) + btnGap;
    set_bounds(g_btStart, x, btnY, btnW, row);             x += btnW + btnGap;
    set_bounds(g_btStop, x, btnY, btnW, row);              x += btnW + btnGap;
    {
        int ckW = ui(126);
        set_bounds(g_ckAuto, x, btnY + ui(2), ckW, row - ui(2));
        x += ckW + ui(6);
        set_bounds(g_lbStatus, x, btnY + ui(4), cw - pad - x, ui(20));
    }

    set_bounds(g_ckRun, pad, toggleY, ui(96), row);
    set_bounds(g_ckTray, pad + ui(104), toggleY, ui(210), row);

    logY = ch - pad - logH;
    set_bounds(g_edLog, pad, logY, cw - pad * 2, logH);
    if (g_logFollow) log_scroll_bottom();     /* 窗口尺寸变了也继续贴底 */
}

static void build_ui(void)
{
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    RECT rc;

    g_monoFont = create_mono_font(ui(13));
    if (!g_monoFont) g_monoFont = font;

    g_stRecord = mk(L"STATIC", L"TXT 记录", SS_LEFT, 0, 0, 10, 10, 0, font);
    g_edRecord = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 0, 0, 10, 10, IDC_ED_RECORD, font);
    g_stPort = mk(L"STATIC", L"本地端口", SS_LEFT, 0, 0, 10, 10, 0, font);
    g_edPort = mk(L"EDIT", L"9000", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, 0, 0, 10, 10, IDC_ED_PORT, font);
    g_btAdd = mk(L"BUTTON", L"添加/修改", BS_PUSHBUTTON, 0, 0, 10, 10, IDC_BT_ADD, font);
    g_stMatch = mk(L"STATIC", L"匹配域名(可选，支持 *)", SS_LEFT, 0, 0, 10, 10, 0, font);
    g_edMatch = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 0, 0, 10, 10, IDC_ED_MATCH, font);

    g_lbRules = mk(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                   0, 0, 10, 10, IDC_LB_RULES, g_monoFont);

    g_btEnable = mk(L"BUTTON", L"启用所选", BS_PUSHBUTTON, 0, 0, 10, 10, IDC_BT_ENABLE, font);
    g_btDisable = mk(L"BUTTON", L"停用所选", BS_PUSHBUTTON, 0, 0, 10, 10, IDC_BT_DISABLE, font);
    g_btDel = mk(L"BUTTON", L"删除", BS_PUSHBUTTON, 0, 0, 10, 10, IDC_BT_DEL, font);
    g_btStart = mk(L"BUTTON", L"全部启动", BS_PUSHBUTTON, 0, 0, 10, 10, IDC_BT_START, font);
    g_btStop = mk(L"BUTTON", L"全部停止", BS_PUSHBUTTON, 0, 0, 10, 10, IDC_BT_STOP, font);
    g_ckAuto = mk(L"BUTTON", L"启动时自动连接", BS_AUTOCHECKBOX, 0, 0, 10, 10, IDC_CK_AUTO, font);
    g_ckRun = mk(L"BUTTON", L"开机自启", BS_AUTOCHECKBOX, 0, 0, 10, 10, IDC_CK_RUN, font);
    g_ckTray = mk(L"BUTTON", L"关闭时最小化到托盘", BS_AUTOCHECKBOX, 0, 0, 10, 10, IDC_CK_TRAY, font);
    g_lbStatus = mk(L"STATIC", L"", SS_LEFT, 0, 0, 10, 10, IDC_LB_STATUS, font);

    /* 日志：多行只读，不折行（横向滚动），否则长行会被截断/回卷 */
    g_edLog = mk(L"EDIT", L"",
                 WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_HSCROLL |
                 ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                 0, 0, 10, 10, IDC_ED_LOG, font);

    GetClientRect(g_hwnd, &rc);
    layout_ui(rc.right - rc.left, rc.bottom - rc.top);
}

/* 排序权重：精确域名(0) → 通配(1) → 未写匹配域名(2，仅端口转发)
 * 命中的是第一条匹配的规则，所以精确必须排在通配前面。 */
static int rule_rank(const RULE *r)
{
    if (!r->match[0]) return 2;
    return wcschr(r->match, L'*') ? 1 : 0;
}

static int rule_index_of(const RULE *r)
{
    int i;

    for (i = 0; i < g_ruleCount; i++) {
        if (g_rules[i] == r) return i;
    }
    return -1;
}

/* 稳定插入排序：只在权重更大时后移，同权重保持原有相对顺序 */
static BOOL rule_sort(void)
{
    int i, j;
    BOOL changed = FALSE;

    for (i = 1; i < g_ruleCount; i++) {
        RULE *cur = g_rules[i];
        int rank = rule_rank(cur);

        j = i - 1;
        while (j >= 0 && rule_rank(g_rules[j]) > rank) {
            g_rules[j + 1] = g_rules[j];
            j--;
        }
        if (j != i - 1) {
            g_rules[j + 1] = cur;
            changed = TRUE;
        }
    }
    return changed;
}

/* 把某条规则的参数填到顶部编辑栏；-1 表示清空 */
static void list_show_rule(int idx)
{
    if (!g_edRecord) return;
    if (idx < 0 || idx >= g_ruleCount) {
        SetWindowTextW(g_edRecord, L"");
        SetWindowTextW(g_edMatch, L"");
        SetWindowTextW(g_edPort, L"");
        return;
    }
    SetWindowTextW(g_edRecord, g_rules[idx]->record);
    SetWindowTextW(g_edMatch, g_rules[idx]->match);
    {
        WCHAR portText[16];
        _snwprintf(portText, 16, L"%d", g_rules[idx]->port);
        SetWindowTextW(g_edPort, portText);
    }
}

/* 列表选中变化：把这条规则读到编辑栏（清空选择就清空编辑栏） */
static void on_select(void)
{
    int sel;

    if (!g_lbRules) return;
    sel = (int)SendMessageW(g_lbRules, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= g_ruleCount) sel = -1;
    g_selRule = sel;
    list_show_rule(sel);
}

static void on_add(void)
{
    WCHAR record[256], portText[32], match[256];
    int port, i;
    RULE *r;

    get_text(g_edRecord, record, 256);
    get_text(g_edMatch, match, 256);
    get_text(g_edPort, portText, 32);
    port = _wtoi(portText);

    if (!record[0]) { log_line(L"记录名不能为空"); return; }
    if (port <= 0 || port > 65535) { log_line(L"本地端口不合法"); return; }

    r = (g_selRule >= 0 && g_selRule < g_ruleCount) ? g_rules[g_selRule] : NULL;
    if (!r && g_ruleCount >= MAX_RULES) { log_line(L"最多 %d 条规则", MAX_RULES); return; }

    for (i = 0; i < g_ruleCount; i++) {
        if (g_rules[i] == r) continue;
        if (g_rules[i]->port == port) { log_line(L"端口 %d 已被规则 #%d 使用", port, i + 1); return; }
        /* 允许同一个 TXT/入口 被多条规则引用（例如同一网关的精确规则 + 通配规则），
         * 只要求本地端口唯一；端口已经保证不会有两条一模一样的转发。 */
    }

    if (r) {
        /* 修改选中规则：端口或来源变了才重启监听线程 */
        BOOL needRestart = (r->port != port) || (lstrcmpiW(r->record, record) != 0);
        BOOL wasRunning = (r->thread != NULL);

        if (wasRunning && needRestart) rule_stop(r);
        lstrcpynW(r->record, record, 256);
        lstrcpynW(r->match, match, 256);
        r->kind = (!wcsncmp(record, L"http://", 7) || !wcsncmp(record, L"https://", 8)) ? 1 : 0;
        r->port = port;
        if (needRestart) {
            EnterCriticalSection(&g_lock);
            r->hasTarget = FALSE;
            r->targetHost[0] = 0;
            LeaveCriticalSection(&g_lock);
        }
        if (wasRunning && needRestart) rule_start(r);
        log_line(L"已修改规则: %s :%d%s%s", record, port,
                 match[0] ? L"  匹配 " : L"", match);
    } else {
        r = (RULE *)calloc(1, sizeof(RULE));
        if (!r) { log_line(L"内存不足"); return; }
        lstrcpynW(r->record, record, 256);
        lstrcpynW(r->match, match, 256);
        r->kind = (!wcsncmp(record, L"http://", 7) || !wcsncmp(record, L"https://", 8)) ? 1 : 0;
        r->port = port;
        r->enabled = TRUE;
        r->listenSock = INVALID_SOCKET;
        g_rules[g_ruleCount++] = r;
        log_line(L"已添加规则: %s -> 127.0.0.1:%d%s%s", record, port,
                 match[0] ? L"  匹配 " : L"", match);
    }

    if (rule_sort()) log_line(L"已按「精确域名 → 通配 → 仅端口」调整了规则顺序");

    g_selRule = rule_index_of(r);
    config_save();
    refresh_list();
    if (g_selRule >= 0) {
        SendMessageW(g_lbRules, LB_SETCURSEL, (WPARAM)g_selRule, 0);
        SendMessageW(g_lbRules, LB_SETCARETINDEX, (WPARAM)g_selRule, MAKELPARAM(FALSE, 0));
    }
    list_show_rule(g_selRule);
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
    g_selRule = -1;
    config_save();
    refresh_list();
    list_show_rule(-1);
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

/* Refresh the log view: follow new lines (pin to the bottom) unless the log
 * control has the keyboard focus, in which case the user is reading/scrolling
 * and the current top line plus selection are preserved. */
static void log_scroll_bottom(void)
{
    int len;

    if (!g_edLog) return;
    len = GetWindowTextLengthW(g_edLog);
    SendMessageW(g_edLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_edLog, EM_LINESCROLL, 0, (LPARAM)0x7FFF);
    g_logLastFirst = (int)SendMessageW(g_edLog, EM_GETFIRSTVISIBLELINE, 0, 0);
}

static int edit_visible_lines(HWND h)
{
    HDC dc = GetDC(h);
    TEXTMETRICW tm;
    RECT rc;
    HFONT old;
    int hh, lines;

    ZeroMemory(&tm, sizeof(tm));
    old = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    if (old) SelectObject(dc, old);
    GetTextMetricsW(dc, &tm);
    GetClientRect(h, &rc);
    if (old) SelectObject(dc, old);
    ReleaseDC(h, dc);

    hh = tm.tmHeight > 0 ? tm.tmHeight : 16;
    lines = (rc.bottom - rc.top) / hh;
    return lines > 0 ? lines : 1;
}

static void refresh_log_view(void)
{
    static int lastLen = -1;
    WCHAR *copy = NULL;
    int total, follow, first, now, selStart = 0, selEnd = 0;

    if (!g_edLog) return;
    EnterCriticalSection(&g_lock);
    if (g_logBuf) {
        size_t len = wcslen(g_logBuf);
        copy = (WCHAR *)malloc((len + 1) * sizeof(WCHAR));
        if (copy) wcscpy(copy, g_logBuf);
    }
    LeaveCriticalSection(&g_lock);
    if (!copy) return;

    total = (int)wcslen(copy);
    if (total == lastLen) {          /* nothing new */
        free(copy);
        return;
    }
    lastLen = total;

    first = (int)SendMessageW(g_edLog, EM_GETFIRSTVISIBLELINE, 0, 0);
    {
        int lines = (int)SendMessageW(g_edLog, EM_GETLINECOUNT, 0, 0);

        /* 用户自己往上滚了（首行比我们上次贴底的位置更靠前）→ 停止跟随 */
        if (g_logLastFirst >= 0 && first + 1 < g_logLastFirst) g_logFollow = FALSE;
        /* 又滚回底部 → 恢复跟随（留一点余量，可见行数只是估算） */
        if (!g_logFollow && first + edit_visible_lines(g_edLog) + 2 >= lines) g_logFollow = TRUE;

        follow = (GetFocus() != g_edLog) && g_logFollow;
    }
    SendMessageW(g_edLog, EM_GETSEL, (WPARAM)(DWORD_PTR)&selStart, (LPARAM)(DWORD_PTR)&selEnd);

    SetWindowTextW(g_edLog, copy);
    free(copy);

    if (follow) {
        log_scroll_bottom();          /* 光标放到末尾并滚到最后一行 */
    } else {
        /* 保持用户当前看的那一行（换文本会把视图复位到顶端），再恢复选区 */
        SendMessageW(g_edLog, EM_SETSEL, (WPARAM)selStart, (LPARAM)selEnd);
        now = (int)SendMessageW(g_edLog, EM_GETFIRSTVISIBLELINE, 0, 0);
        if (first != now) SendMessageW(g_edLog, EM_LINESCROLL, 0, (LPARAM)(first - now));
        g_logLastFirst = -1;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    /* explorer.exe 重启后会广播 TaskbarCreated，托盘图标要重新加回去 */
    if (g_msgTaskbarCreated != 0 && msg == g_msgTaskbarCreated) {
        g_trayShown = FALSE;
        if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = NULL; }
        tray_add();
        return 0;
    }

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

    case WM_SIZE:
        if (g_lbRules) layout_ui(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lParam;
        mm->ptMinTrackSize.x = ui(560);
        mm->ptMinTrackSize.y = ui(470);
        return 0;
    }

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
        case IDC_CK_RUN:
            g_autoRunOn = (SendMessageW(g_ckRun, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (autorun_set(g_autoRunOn)) {
                log_line(g_autoRunOn ? L"已设置开机自启（启动时缩到托盘）" : L"已取消开机自启");
            } else {
                g_autoRunOn = !g_autoRunOn;
                SendMessageW(g_ckRun, BM_SETCHECK, g_autoRunOn ? BST_CHECKED : BST_UNCHECKED, 0);
                log_line(L"设置开机自启失败（注册表不可写？）");
            }
            config_save();
            return 0;
        case IDC_CK_TRAY:
            g_closeToTray = (SendMessageW(g_ckTray, BM_GETCHECK, 0, 0) == BST_CHECKED);
            config_save();
            log_line(g_closeToTray ? L"关闭窗口时最小化到托盘" : L"关闭窗口时直接退出");
            return 0;
        case IDM_SHOW:
            show_main_window();
            return 0;
        case IDM_STARTALL:
            on_start_all();
            tray_update_tip();
            return 0;
        case IDM_STOPALL:
            on_stop_all();
            tray_update_tip();
            return 0;
        case IDM_AUTORUN:
            g_autoRunOn = !g_autoRunOn;
            autorun_set(g_autoRunOn);
            if (g_ckRun)
                SendMessageW(g_ckRun, BM_SETCHECK, g_autoRunOn ? BST_CHECKED : BST_UNCHECKED, 0);
            config_save();
            log_line(g_autoRunOn ? L"已设置开机自启（启动时缩到托盘）" : L"已取消开机自启");
            return 0;
        case IDM_QUIT:
            g_quitting = TRUE;
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case IDC_LB_RULES:
            if (HIWORD(wParam) == LBN_SELCHANGE) {
                on_select();
                return 0;
            }
            if (HIWORD(wParam) == LBN_DBLCLK) {
                int sel = selected_rule();
                if (sel >= 0) on_set_enabled(!g_rules[sel]->enabled);
            }
            return 0;
        }
        return 0;

    case WM_TRAYMSG:
        if (LOWORD(lParam) == WM_LBUTTONDBLCLK || LOWORD(lParam) == WM_LBUTTONUP) {
            show_main_window();
            return 0;
        }
        if (LOWORD(lParam) == WM_RBUTTONUP) {
            tray_menu();
            return 0;
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MINIMIZE) {
            hide_to_tray();
            return 0;
        }
        break;

    case WM_APP_LOG:
        refresh_log_view();
        return 0;

    case WM_APP_STATE:
        update_status_text();
        refresh_list();
        tray_update_tip();
        return 0;

    case WM_CLOSE:
        if (!g_quitting && g_closeToTray) {
            hide_to_tray();
            return 0;
        }
        on_stop_all();
        InterlockedExchange(&g_shutdown, 1);
        config_save();
        tray_remove();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_REFRESH);
        tray_remove();
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
    g_inst = inst;
    g_startMinimized = (cmdline && wcsstr(cmdline, L"-min")) ? TRUE : FALSE;

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
        _snwprintf(g_logFilePath, MAX_PATH - 1, L"%s\\txtmux.log", exe);
        _snwprintf(g_iniPath, MAX_PATH - 1, L"%s\\txtmux.ini", exe);
    }

    /* 单实例：已有实例在跑（可能正缩在托盘里）就把它的窗口叫到前面，
       否则重复双击 exe 会出现第二个进程抢同一个代理端口 */
    {
        HANDLE mtx = CreateMutexW(NULL, FALSE, L"Local\\txtmux_single_instance");
        if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND exist = FindWindowW(L"TxtMuxWnd", NULL);
            if (exist) {
                ShowWindow(exist, SW_SHOW);
                ShowWindow(exist, SW_RESTORE);
                SetForegroundWindow(exist);
            }
            return 0;
        }
    }

    {
        HDC screen = GetDC(NULL);
        if (screen) {
            g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
            ReleaseDC(NULL, screen);
        }
        if (g_dpi <= 0) g_dpi = 96;
    }

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"TxtMuxWnd";
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0,
                                 LR_DEFAULTSIZE | LR_DEFAULTCOLOR);
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                   LR_DEFAULTCOLOR);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (!RegisterClassExW(&wc)) return 1;

    hwnd = CreateWindowExW(0, L"TxtMuxWnd", APP_TITLE,
                           WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, ui(648), ui(520),
                           NULL, NULL, inst, NULL);
    if (!hwnd) return 1;

    config_load();
    log_init();
    g_autoRunOn = autorun_get();
    SetWindowTextW(g_ckAuto, L"启动时自动连接");
    SendMessageW(g_ckAuto, BM_SETCHECK, g_autoStart ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_ckRun)
        SendMessageW(g_ckRun, BM_SETCHECK, g_autoRunOn ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_ckTray)
        SendMessageW(g_ckTray, BM_SETCHECK, g_closeToTray ? BST_CHECKED : BST_UNCHECKED, 0);
    refresh_list();
    log_line(L"就绪：点列表里的规则可修改，改完点「添加/修改」（最多 %d 条）", MAX_RULES);
    if (g_sortChanged) log_line(L"规则已按「精确域名 → 通配 → 仅端口」自动排序");

    tray_add();                     /* 常驻托盘，最小化/关闭后还能从图标恢复 */
    if (!g_startMinimized) {
        ShowWindow(hwnd, show);
        UpdateWindow(hwnd);
    } else {
        log_line(L"以最小化方式启动（-min），托盘图标可恢复窗口");
    }

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
