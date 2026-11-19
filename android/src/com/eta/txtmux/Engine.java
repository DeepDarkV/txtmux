package com.eta.txtmux;

import android.content.Context;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * 转发引擎：规则监听器 + 本地代理（HTTP CONNECT / SOCKS5 / 明文 HTTP）+ PAC。
 * 行为对齐 Windows 版 txtmux.c：
 *   - 目标解析缓存 30 秒，失败沿用上次成功地址；
 *   - 代理只按「匹配域名」分流，没写匹配域名的规则只做端口转发；
 *   - 出站按目标自身的 A/AAAA 选择，IPv6 不被降级。
 */
public class Engine {

    public static final int MAX_RULES = 32;

    private static final int TTL_MS = 30000;
    private static final int FAIL_RETRY_MS = 5000;
    private static final int CONNECT_TIMEOUT = 8000;

    private static volatile Engine inst;

    public static Engine get() {
        Engine e = inst;
        if (e == null) {
            synchronized (Engine.class) {
                if (inst == null) inst = new Engine();
                e = inst;
            }
        }
        return e;
    }

    private volatile Context app;
    private volatile Cfg cfg;
    private final Object lock = new Object();

    private volatile ServerSocket proxySs;
    private volatile Thread proxyThread;
    private volatile boolean proxyRunning;
    private volatile boolean shutdown;

    private Engine() {
    }

    /* ---------------- 初始化 / 配置 ---------------- */

    public void attach(Context c) {
        if (app == null) app = c.getApplicationContext();
        if (cfg == null) {
            synchronized (lock) {
                if (cfg == null) {
                    cfg = Cfg.load(app);
                    Logx.init(app, cfg.logMaxMB);
                    sortRules(cfg.rules);
                }
            }
        }
    }

    public Cfg cfg() {
        return cfg;
    }

    public List<Rule> rules() {
        Cfg c = cfg;
        if (c == null) return new ArrayList<Rule>();
        synchronized (lock) {
            return new ArrayList<Rule>(c.rules);
        }
    }

    public void save() {
        Cfg c = cfg;
        if (c != null && app != null) c.save(app);
    }

    /** 精确域名 → 通配 → 仅端口（稳定排序，避免通配写在精确前面导致命中错） */
    static void sortRules(List<Rule> rules) {
        List<Rule> copy = new ArrayList<Rule>(rules);
        final int[] rank = new int[copy.size()];
        for (int i = 0; i < copy.size(); i++) {
            String m = copy.get(i).match;
            rank[i] = (m == null || m.isEmpty()) ? 2 : (m.indexOf('*') >= 0 ? 1 : 0);
        }
        Integer[] idx = new Integer[copy.size()];
        for (int i = 0; i < idx.length; i++) idx[i] = i;
        Arrays.sort(idx, new java.util.Comparator<Integer>() {
            @Override
            public int compare(Integer a, Integer b) {
                if (rank[a] != rank[b]) return rank[a] - rank[b];
                return a - b;
            }
        });
        for (int i = 0; i < idx.length; i++) rules.set(i, copy.get(idx[i]));
    }

    /* ---------------- 规则增删改 ---------------- */

    /** 返回 null 表示成功，否则是给用户看的错误文案 */
    public String addOrUpdateRule(int index, String src, String match, int port) {
        String s = src == null ? "" : src.trim();
        String m = match == null ? "" : match.trim();
        if (s.isEmpty()) return "来源不能为空";
        if (s.indexOf(' ') >= 0 || s.indexOf('\t') >= 0) return "来源不能包含空格";
        if (s.startsWith("http://") || s.startsWith("https://")) {
            if (Net.parseUrl(s) == null) return "入口 URL 不合法";
        } else if (s.indexOf('/') >= 0) {
            return "TXT 记录名不能包含 /";
        }
        if (port <= 0 || port > 65535) return "本地端口不合法（1-65535）";
        if (m.indexOf(' ') >= 0) return "匹配域名不能包含空格";

        synchronized (lock) {
            List<Rule> rs = cfg.rules;
            for (int i = 0; i < rs.size(); i++) {
                if (i == index) continue;
                if (rs.get(i).port == port) return "本地端口 " + port + " 已被第 " + (i + 1) + " 条规则占用";
            }
            Rule r;
            if (index >= 0 && index < rs.size()) {
                r = rs.get(index);
                boolean restart = r.running && r.port != port;
                if (restart) stopRule(r);
                r.src = s;
                r.match = m;
                r.port = port;
                if (restart) startRule(r);
            } else {
                if (rs.size() >= MAX_RULES) return "最多 " + MAX_RULES + " 条规则";
                r = new Rule();
                r.src = s;
                r.match = m;
                r.port = port;
                r.enabled = true;
                rs.add(r);
            }
            save();
            Logx.line("规则%s：%s :%d%s", index >= 0 ? "已修改" : "已新增", s, port,
                    m.isEmpty() ? "" : "（匹配 " + m + "）");
        }
        return null;
    }

    public void removeRule(int index) {
        synchronized (lock) {
            if (index < 0 || index >= cfg.rules.size()) return;
            Rule r = cfg.rules.remove(index);
            stopRule(r);
            save();
            Logx.line("已删除规则：%s :%d", r.src, r.port);
        }
    }

    public void setEnabled(int index, boolean enabled) {
        synchronized (lock) {
            if (index < 0 || index >= cfg.rules.size()) return;
            Rule r = cfg.rules.get(index);
            r.enabled = enabled;
            if (!enabled) stopRule(r);
            save();
            Logx.line("规则 %s 已%s", r.name(), enabled ? "启用" : "停用");
        }
    }

    /** 保存代理/全局设置；代理原本在跑就按新参数重启 */
    public void saveProxy(String bind, int port, String upstream, boolean block, boolean autostart) {
        Cfg c = cfg;
        if (c == null) return;
        boolean was = proxyRunning;
        stopProxy();
        c.bind = (bind == null || bind.trim().isEmpty()) ? "127.0.0.1" : bind.trim();
        c.proxyPort = (port < 0 || port > 65535) ? 0 : port;
        c.upstream = upstream == null ? "" : upstream.trim();
        c.upstreamBlock = block;
        c.autostart = autostart;
        save();
        Logx.line("代理设置已保存：%s:%d%s%s", c.bind, c.proxyPort,
                c.upstream.isEmpty() ? "" : "，上游 " + c.upstream,
                c.autostart ? "，开机自启" : "");
        if (was && c.proxyPort > 0) startProxy();
    }

    /* ---------------- 启动 / 停止 ---------------- */

    public void startAll() {
        Cfg c = cfg;
        if (c == null) return;
        for (Rule r : rules()) {
            if (r.enabled) startRule(r);
        }
        startProxy();
    }

    public void stopAll() {
        for (Rule r : rules()) stopRule(r);
        stopProxy();
    }

    /** 把 running 与实际套接字/线程对齐，避免界面显示"运行中"实际已断 */
    public void healthCheck() {
        for (Rule r : rules()) {
            Thread t = r.thread;
            if (t == null) {
                if (r.running) {
                    r.running = false;
                    Logx.line("[%s] 状态修正：已停止", r.name());
                }
                continue;
            }
            ServerSocket ss = r.ss;
            boolean alive = t.isAlive();
            boolean bound = ss != null && !ss.isClosed();
            if (!alive || !bound) {
                r.running = false;
                r.thread = null;
                if (ss != null) {
                    try {
                        ss.close();
                    } catch (Exception ignored) {
                    }
                }
                r.ss = null;
                r.lastError = "监听已中断";
                Logx.line("[%s] 监听已中断，状态修正为已停止", r.name());
            }
        }
        if (proxyThread == null && proxyRunning) {
            proxyRunning = false;
            Logx.line("代理状态修正：已停止");
        }
    }

    public boolean isAnythingRunning() {
        for (Rule r : rules()) if (r.running) return true;
        return proxyRunning;
    }

    public void startRule(final Rule r) {
        if (r.thread != null) return;
        r.stopFlag = false;
        r.lastError = "";
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                acceptLoop(r);
            }
        }, "txtmux-rule-" + r.port);
        r.thread = t;
        t.start();
        /* 启动就解析一次：列表上直接显示真实目标或失败原因，不用等第一个连接 */
        new Thread(new Runnable() {
            @Override
            public void run() {
                Target tg = targetFor(r);
                if (tg == null) {
                    Logx.line("[%s] 暂时没有可用目标：%s", r.name(), r.lastError);
                } else {
                    Logx.line("[%s] 目标 %s:%d", r.name(), tg.host, tg.port);
                }
            }
        }, "txtmux-pre-" + r.port).start();
    }

    public void stopRule(Rule r) {
        if (r.thread == null) return;
        r.stopFlag = true;
        ServerSocket ss = r.ss;
        if (ss != null) {
            try {
                ss.close();
            } catch (Exception ignored) {
            }
        }
        Thread t = r.thread;
        try {
            t.join(2000);
        } catch (InterruptedException ignored) {
        }
        r.thread = null;
        r.ss = null;
        r.running = false;
    }

    private void acceptLoop(Rule r) {
        ServerSocket ss = null;
        String bind = cfg.bind;
        try {
            ss = new ServerSocket();
            ss.setReuseAddress(true);
            ss.bind(new InetSocketAddress(bindAddress(bind), r.port), 16);
            ss.setSoTimeout(200);
        } catch (IOException e) {
            Logx.line("[%s] 监听 %s:%d 失败（%s）", r.name(), bind, r.port, e.getMessage());
            r.lastError = "端口被占用或被拒绝";
            r.running = false;
            r.thread = null;
            try {
                if (ss != null) ss.close();
            } catch (Exception ignored) {
            }
            return;
        }
        r.ss = ss;
        r.running = true;
        Logx.line("[%s] 已监听 %s:%d", r.name(), bind, r.port);

        while (!r.stopFlag) {
            Socket client = null;
            try {
                client = ss.accept();
            } catch (SocketTimeoutException e) {
                continue;
            } catch (IOException e) {
                break;
            }
            if (client == null) continue;
            final Socket c = client;
            new Thread(new Runnable() {
                @Override
                public void run() {
                    serveClient(r, c);
                }
            }, "txtmux-cli-" + r.port).start();
        }
        try {
            if (ss != null) ss.close();
        } catch (Exception ignored) {
        }
        r.ss = null;
        r.running = false;
        r.thread = null;
        Logx.line("[%s] 已停止监听 %d", r.name(), r.port);
    }

    private void serveClient(Rule r, Socket client) {
        Socket up = null;
        try {
            client.setTcpNoDelay(true);
            Target t = targetFor(r);
            if (t == null) {
                Logx.line("[%s] 没有可用目标，丢弃连接", r.name());
                return;
            }
            try {
                up = dial(t.host, t.port);
            } catch (IOException e) {
                Logx.line("[%s] 连接 %s:%d 失败（%s）", r.name(), t.host, t.port, e.getMessage());
                return;
            }
            bridge(client, up);
        } catch (IOException e) {
            Logx.line("[%s] 连接异常（%s）", r.name(), e.getMessage());
        } finally {
            if (up == null) {
                close(client);
            }
        }
    }

    /* ---------------- 目标解析 ---------------- */

    static final class Target {
        final String host;
        final int port;

        Target(String h, int p) {
            host = h;
            port = p;
        }
    }

    private Target targetFor(Rule r) {
        synchronized (r.lock) {
            long now = System.currentTimeMillis();
            if (r.hasTarget && now - r.targetAt < TTL_MS) {
                return new Target(r.targetHost, r.targetPort);
            }
            /* 刚失败过就先不重试，免得每个连接都等满超时 */
            if (!r.hasTarget && r.lastTryAt != 0 && now - r.lastTryAt < FAIL_RETRY_MS) {
                return null;
            }
            r.resolving = true;
            Target t;
            try {
                t = resolveRule(r);
            } finally {
                r.resolving = false;
            }
            r.lastTryAt = now;
            if (t != null) {
                boolean changed = !r.hasTarget || !t.host.equalsIgnoreCase(r.targetHost) || t.port != r.targetPort;
                r.targetHost = t.host;
                r.targetPort = t.port;
                r.targetAt = now;
                r.hasTarget = true;
                r.lastError = "";
                if (changed) Logx.line("%s -> %s:%d", r.src, t.host, t.port);
                return t;
            }
            if (r.lastError == null || r.lastError.isEmpty()) r.lastError = "没有解析出可用地址";
            if (r.hasTarget) {
                r.lastError = "解析失败，暂时沿用 " + r.targetHost + ":" + r.targetPort;
                Logx.line("WARN 解析 %s 失败，沿用 %s:%d", r.src, r.targetHost, r.targetPort);
                return new Target(r.targetHost, r.targetPort);
            }
            return null;
        }
    }

    private Target resolveRule(Rule r) {
        if (r.isUrl()) {
            try {
                String finalUrl = HttpFollow.finalUrl(r.src);
                Net.Url u = Net.parseUrl(finalUrl);
                if (u == null) {
                    r.lastError = "跳转后的 URL 不合法";
                    Logx.line("WARN 跟随跳转后的 URL 不合法: %s", finalUrl);
                    return null;
                }
                if (u.port != 443 && u.port != 80) Logx.line("跟随跳转: %s", finalUrl);
                return new Target(u.host, u.port);
            } catch (Exception e) {
                r.lastError = "跟随跳转失败：" + e.getMessage();
                Logx.line("WARN 跟随跳转失败: %s（%s）", r.src, e.getMessage());
                return null;
            }
        }
        String value = Dns.txt(app, r.src);
        if (value == null || value.isEmpty()) {
            r.lastError = "TXT 查询失败";
            Logx.line("WARN TXT %s 查询失败", r.src);
            return null;
        }
        Net.Endpoint ep = Net.parseEndpoint(value);
        if (ep == null) {
            r.lastError = "TXT 不是 host:port（" + value + "）";
            Logx.line("WARN TXT %s 不是 host:port（%s）", r.src, value);
            return null;
        }
        return new Target(ep.host, ep.port);
    }

    /* ---------------- 建连 ---------------- */

    static Socket dial(String host, int port) throws IOException {
        return Net.connectAny(host, port, CONNECT_TIMEOUT);
    }

    /** 供 VPN 分流使用：按客户端访问的域名找规则（只认「匹配域名」） */
    public Rule ruleForHost(String host) {
        return findRuleForHost(host);
    }

    /** 供 VPN 分流使用：返回规则当前目标（含 30 秒缓存与失败回退） */
    public Target targetForRule(Rule r) {
        return targetFor(r);
    }

    /** 供 VPN 分流使用：直接连 host:port，多地址并发，可选 protect（避免回环进 VPN） */
    public Socket dialRaw(String host, int port, java.util.function.BooleanSupplier protect) {
        try {
            Socket s = Net.connectAny(host, port, CONNECT_TIMEOUT);
            if (protect != null && protect.getAsBoolean()) {
                /* 调用方负责 protect（VpnService 实例） */
            }
            return s;
        } catch (IOException e) {
            Logx.line("[VPN] 连接 %s:%d 失败（%s）", host, port, e.getMessage());
            return null;
        }
    }

    private Rule findRuleForHost(String host) {
        for (Rule r : rules()) {
            if (r.match != null && !r.match.isEmpty() && Net.hostMatch(r.match, host)) return r;
        }
        return null;
    }

    private Socket dialFor(String host, int port) {
        Rule r = findRuleForHost(host);
        if (r != null) {
            Target t = targetFor(r);
            if (t != null) {
                Logx.line("[代理] %s:%d -> %s:%d", host, port, t.host, t.port);
                try {
                    return dial(t.host, t.port);
                } catch (IOException e) {
                    Logx.line("[代理] 连接 %s:%d 失败（%s）", t.host, t.port, e.getMessage());
                    return null;
                }
            }
        }
        Socket viaUpstream = dialUpstream(host, port);
        if (viaUpstream != null) return viaUpstream;
        Cfg c = cfg;
        if (c == null) return null;
        if (!c.upstream.isEmpty() && c.upstreamBlock) {
            Logx.line("[代理] 上游不可用且设置为拒绝直连，放弃 %s:%d", host, port);
            return null;
        }
        if (!c.upstream.isEmpty()) Logx.line("WARN 上游 %s 不可用，改为直连 %s:%d", c.upstream, host, port);
        try {
            return dial(host, port);
        } catch (IOException e) {
            Logx.line("[代理] 连接 %s:%d 失败（%s）", host, port, e.getMessage());
            return null;
        }
    }

    private Socket dialUpstream(String host, int port) {
        Cfg c = cfg;
        if (c == null || c.upstream.isEmpty()) return null;
        String up = c.upstream.trim();
        String lower = up.toLowerCase(Locale.US);
        if (lower.startsWith("http://")) up = up.substring(7);
        else if (lower.startsWith("socks5://")) up = up.substring(9);
        String uh = up;
        int uport;
        int colon = up.lastIndexOf(':');
        if (colon > 0 && up.indexOf(':') == colon) {
            uh = up.substring(0, colon).trim();
            uport = Net.parseInt(up.substring(colon + 1));
        } else {
            uport = Net.parseInt(up);
        }
        if (uh.isEmpty()) uh = "127.0.0.1";
        if (uport <= 0 || uport > 65535) return null;
        Socket s;
        try {
            s = dial(uh, uport);
        } catch (IOException e) {
            return null;
        }
        try {
            String req = "CONNECT " + host + ":" + port + " HTTP/1.1\r\n"
                    + "Host: " + host + ":" + port + "\r\n"
                    + "Proxy-Connection: keep-alive\r\n\r\n";
            s.setSoTimeout(CONNECT_TIMEOUT);
            s.getOutputStream().write(req.getBytes(StandardCharsets.ISO_8859_1));
            s.getOutputStream().flush();
            String head = readHead(s.getInputStream(), 4096);
            if (head == null || head.indexOf(" 200") < 0) {
                Logx.line("[代理] 上游 %s 拒绝隧道 %s:%d", c.upstream, host, port);
                close(s);
                return null;
            }
            s.setSoTimeout(0);
            Logx.line("[代理] %s:%d -> 上游 %s", host, port, c.upstream);
            return s;
        } catch (IOException e) {
            close(s);
            return null;
        }
    }

    /* ---------------- 代理服务 ---------------- */

    public void startProxy() {
        Cfg c = cfg;
        if (c == null) return;
        if (c.proxyPort <= 0) {
            Logx.line("代理: 未配置端口，跳过");
            return;
        }
        if (proxyThread != null) return;
        shutdown = false;
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                proxyLoop();
            }
        }, "txtmux-proxy");
        proxyThread = t;
        t.start();
    }

    public void stopProxy() {
        if (proxyThread == null) return;
        shutdown = true;
        ServerSocket ss = proxySs;
        if (ss != null) {
            try {
                ss.close();
            } catch (Exception ignored) {
            }
        }
        try {
            proxyThread.join(2000);
        } catch (InterruptedException ignored) {
        }
        proxyThread = null;
        proxyRunning = false;
    }

    public boolean proxyRunning() {
        return proxyRunning;
    }

    private void proxyLoop() {
        Cfg c = cfg;
        int port = c.proxyPort;
        String bind = c.bind;
        ServerSocket ss = null;
        try {
            ss = new ServerSocket();
            ss.setReuseAddress(true);
            ss.bind(new InetSocketAddress(bindAddress(bind), port), 32);
            ss.setSoTimeout(200);
        } catch (IOException e) {
            Logx.line("代理: 监听 %s:%d 失败（%s）", bind, port, e.getMessage());
            proxyThread = null;
            proxyRunning = false;
            try {
                if (ss != null) ss.close();
            } catch (Exception ignored) {
            }
            return;
        }
        proxySs = ss;
        proxyRunning = true;
        Logx.line("代理已就绪: %s:%d（HTTP CONNECT / SOCKS5，PAC http://%s:%d/proxy.pac）", bind, port, bind, port);

        while (!shutdown && !ss.isClosed()) {
            Socket client = null;
            try {
                client = ss.accept();
            } catch (SocketTimeoutException e) {
                continue;
            } catch (IOException e) {
                break;
            }
            if (client == null) continue;
            final Socket cc = client;
            new Thread(new Runnable() {
                @Override
                public void run() {
                    handleProxy(cc);
                }
            }, "txtmux-px").start();
        }
        try {
            if (ss != null) ss.close();
        } catch (Exception ignored) {
        }
        proxySs = null;
        proxyRunning = false;
        proxyThread = null;
        Logx.line("代理已停止");
    }

    private void handleProxy(Socket client) {
        boolean tunneled = false;
        try {
            client.setSoTimeout(15000);
            client.setTcpNoDelay(true);
            InputStream in = new BufferedInputStream(client.getInputStream());
            OutputStream out = client.getOutputStream();
            int first = in.read();
            if (first < 0) return;
            if (first == 0x05) {
                tunneled = socks5(client, in, out);
            } else {
                tunneled = httpProxy(client, first, in, out);
            }
        } catch (IOException e) {
            Logx.line("[代理] 连接异常（%s）", e.getMessage());
        } finally {
            if (!tunneled) close(client);
        }
    }

    /* ---- SOCKS5（只支持 no-auth + CONNECT） ---- */
    private boolean socks5(Socket client, InputStream in, OutputStream out) throws IOException {
        int nm = in.read();
        if (nm < 0) return false;
        boolean offered = false;
        for (int i = 0; i < nm; i++) {
            int m = in.read();
            if (m < 0) return false;
            if (m == 0x00) offered = true;
        }
        out.write(new byte[]{0x05, (byte) (offered ? 0x00 : 0xff)});
        out.flush();
        if (!offered) return false;

        byte[] head = readExact(in, 4);
        if (head == null) return false;
        if (head[0] != 0x05 || head[1] != 0x01) return false;   /* 只支持 CONNECT */
        String host;
        if (head[3] == 0x01) {                                  /* IPv4 */
            byte[] ip = readExact(in, 4);
            if (ip == null) return false;
            host = String.format(Locale.US, "%d.%d.%d.%d", ip[0] & 0xff, ip[1] & 0xff, ip[2] & 0xff, ip[3] & 0xff);
        } else if (head[3] == 0x03) {                            /* 域名 */
            int len = in.read();
            if (len <= 0) return false;
            byte[] name = readExact(in, len);
            if (name == null) return false;
            host = new String(name, StandardCharsets.ISO_8859_1);
        } else {
            Logx.line("[代理] SOCKS5 目标为 IPv6，暂不支持");
            return false;
        }
        byte[] pb = readExact(in, 2);
        if (pb == null) return false;
        int port = ((pb[0] & 0xff) << 8) | (pb[1] & 0xff);

        Logx.line("[proxy] request %s:%d", host, port);
        Socket up = dialFor(host, port);
        if (up == null) {
            out.write(new byte[]{0x05, 0x01, 0x00, 0x01, 0, 0, 0, 0, 0, 0});
            out.flush();
            return false;
        }
        out.write(new byte[]{0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0});
        out.flush();
        client.setSoTimeout(0);
        bridge(client, up);
        return true;
    }

    /* ---- HTTP 代理 ---- */
    private boolean httpProxy(Socket client, int first, InputStream in, OutputStream out) throws IOException {
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        bo.write(first);
        int win = first & 0xff;
        while (bo.size() < 16384) {
            int b = in.read();
            if (b < 0) break;
            bo.write(b);
            win = ((win << 8) | (b & 0xff)) & 0xffffffff;
            if (win == 0x0d0a0d0a || win == 0x0a0a) break;
        }
        byte[] raw = bo.toByteArray();
        String head = new String(raw, StandardCharsets.ISO_8859_1);
        int eol = head.indexOf('\n');
        if (eol < 0) eol = head.length();
        String line = head.substring(0, eol).trim();
        int sp1 = line.indexOf(' ');
        if (sp1 <= 0) return false;
        String method = line.substring(0, sp1);
        String target = line.substring(sp1 + 1).trim();
        int sp2 = target.indexOf(' ');
        if (sp2 > 0) target = target.substring(0, sp2);

        if (method.equalsIgnoreCase("CONNECT")) {
            String host;
            int port;
            if (target.startsWith("[")) {
                int close = target.indexOf(']');
                if (close < 0) return false;
                host = target.substring(1, close);
                int c = target.indexOf(':', close);
                port = c > 0 ? Net.parseInt(target.substring(c + 1)) : 0;
            } else {
                int c = target.lastIndexOf(':');
                if (c <= 0) return false;
                host = target.substring(0, c);
                port = Net.parseInt(target.substring(c + 1));
            }
            if (host.isEmpty() || port <= 0) return false;
            Logx.line("[proxy] request %s:%d", host, port);
            Socket up = dialFor(host, port);
            if (up == null) {
                writeAll(out, "HTTP/1.1 502 Bad Gateway\r\n\r\n");
                return false;
            }
            writeAll(out, "HTTP/1.1 200 Connection established\r\n\r\n");
            client.setSoTimeout(0);
            bridge(client, up);
            return true;
        }

        if (method.equalsIgnoreCase("GET") && (target.indexOf("/proxy.pac") >= 0 || target.equals("/"))) {
            sendPac(out);
            return false;
        }

        if (target.toLowerCase(Locale.US).startsWith("http://")) {
            forwardPlainHttp(client, raw, method, target, out);
            return false;
        }

        Logx.line("[proxy] rejected: unsupported request (405)");
        writeAll(out, "HTTP/1.1 405 Method Not Allowed\r\n\r\n");
        return false;
    }

    private void forwardPlainHttp(Socket client, byte[] raw, String method, String url, OutputStream out) throws IOException {
        Net.Url u = Net.parseUrl(url);
        if (u == null || u.secure()) {
            writeAll(out, "HTTP/1.1 405 Method Not Allowed\r\n\r\n");
            return;
        }
        Cfg c = cfg;
        if (c != null && u.port == c.proxyPort
                && (u.host.equals("127.0.0.1") || u.host.equalsIgnoreCase("localhost"))) {
            writeAll(out, "HTTP/1.1 403 Forbidden\r\n\r\n");
            return;
        }
        byte[] sep = "\r\n\r\n".getBytes(StandardCharsets.ISO_8859_1);
        int idx = indexOf(raw, sep);
        if (idx < 0) {
            writeAll(out, "HTTP/1.1 400 Bad Request\r\n\r\n");
            return;
        }
        String head = new String(raw, 0, idx, StandardCharsets.ISO_8859_1);
        byte[] body = Arrays.copyOfRange(raw, idx + 4, raw.length);
        String[] lines = head.split("\r\n");
        StringBuilder sb = new StringBuilder();
        sb.append(method).append(' ').append(u.path).append(" HTTP/1.1\r\n");
        for (int i = 1; i < lines.length; i++) {
            String low = lines[i].toLowerCase(Locale.US);
            if (low.startsWith("proxy-connection:") || low.startsWith("connection:")) continue;
            if (lines[i].isEmpty()) continue;
            sb.append(lines[i]).append("\r\n");
        }
        sb.append("Connection: close\r\n\r\n");

        Logx.line("[proxy] request(HTTP) %s:%d", u.host, u.port);
        Socket up = dialFor(u.host, u.port);
        if (up == null) {
            writeAll(out, "HTTP/1.1 502 Bad Gateway\r\n\r\n");
            return;
        }
        OutputStream uo = up.getOutputStream();
        uo.write(sb.toString().getBytes(StandardCharsets.ISO_8859_1));
        if (body.length > 0) uo.write(body);
        uo.flush();
        client.setSoTimeout(0);
        bridge(client, up);
    }

    private void sendPac(OutputStream out) throws IOException {
        Cfg c = cfg;
        int proxyPort = c == null ? 0 : c.proxyPort;
        String fallback = (c == null || c.upstream.isEmpty()) ? "DIRECT" : ("PROXY " + c.upstream + "; DIRECT");
        StringBuilder sb = new StringBuilder();
        sb.append("function FindProxyForURL(url, host) {\r\n");
        for (Rule r : rules()) {
            if (r.match == null || r.match.isEmpty()) continue;
            if (r.match.indexOf('*') >= 0) {
                sb.append("  if (shExpMatch(host, \"").append(r.match)
                        .append("\")) return \"PROXY 127.0.0.1:").append(proxyPort).append(";\";\r\n");
            } else {
                sb.append("  if (host == \"").append(r.match)
                        .append("\") return \"PROXY 127.0.0.1:").append(proxyPort).append(";\";\r\n");
            }
        }
        sb.append("  return \"").append(fallback).append("\";\r\n}\r\n");
        byte[] body = sb.toString().getBytes(StandardCharsets.UTF_8);
        StringBuilder head = new StringBuilder();
        head.append("HTTP/1.1 200 OK\r\n")
                .append("Content-Type: application/x-ns-proxy-autoconfig\r\n")
                .append("Content-Length: ").append(body.length).append("\r\n")
                .append("Connection: close\r\n\r\n");
        out.write(head.toString().getBytes(StandardCharsets.ISO_8859_1));
        out.write(body);
        out.flush();
    }

    /* ---------------- 通用工具 ---------------- */

    private InetAddress bindAddress(String bind) throws IOException {
        if (bind == null || bind.isEmpty() || bind.equals("0.0.0.0")) return null;
        try {
            return InetAddress.getByName(bind);
        } catch (Exception e) {
            return InetAddress.getByName("127.0.0.1");
        }
    }

    private static int indexOf(byte[] data, byte[] pat) {
        outer:
        for (int i = 0; i + pat.length <= data.length; i++) {
            for (int j = 0; j < pat.length; j++) {
                if (data[i + j] != pat[j]) continue outer;
            }
            return i;
        }
        return -1;
    }

    private static byte[] readExact(InputStream in, int len) throws IOException {
        byte[] b = new byte[len];
        int got = 0;
        while (got < len) {
            int n = in.read(b, got, len - got);
            if (n <= 0) return null;
            got += n;
        }
        return b;
    }

    private static String readHead(InputStream in, int max) throws IOException {
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        int win = 0;
        while (bo.size() < max) {
            int b = in.read();
            if (b < 0) break;
            bo.write(b);
            win = ((win << 8) | (b & 0xff)) & 0xffffffff;
            if (win == 0x0d0a0d0a || win == 0x0a0a) break;
        }
        if (bo.size() == 0) return null;
        return new String(bo.toByteArray(), StandardCharsets.ISO_8859_1);
    }

    private static void writeAll(OutputStream out, String s) {
        try {
            out.write(s.getBytes(StandardCharsets.ISO_8859_1));
            out.flush();
        } catch (Exception ignored) {
        }
    }

    static void bridge(final Socket a, final Socket b) {
        Thread t1 = new Thread(new Runnable() {
            @Override
            public void run() {
                copyStream(a, b);
            }
        }, "txtmux-cp");
        Thread t2 = new Thread(new Runnable() {
            @Override
            public void run() {
                copyStream(b, a);
            }
        }, "txtmux-cp");
        t1.start();
        t2.start();
        try {
            t1.join();
        } catch (InterruptedException ignored) {
        }
        try {
            t2.join();
        } catch (InterruptedException ignored) {
        }
        close(a);
        close(b);
    }

    private static void copyStream(Socket from, Socket to) {
        byte[] buf = new byte[8192];
        try {
            InputStream in = from.getInputStream();
            OutputStream out = to.getOutputStream();
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                out.flush();
            }
        } catch (Exception ignored) {
        } finally {
            try {
                to.shutdownOutput();
            } catch (Exception ignored) {
            }
        }
    }

    static void close(Socket s) {
        if (s == null) return;
        try {
            s.close();
        } catch (Exception ignored) {
        }
    }

    /* ---------------- 状态 ---------------- */

    public String statusText() {
        Cfg c = cfg;
        if (c == null) return "未初始化";
        int total = 0;
        int running = 0;
        for (Rule r : rules()) {
            total++;
            if (r.running) running++;
        }
        StringBuilder sb = new StringBuilder();
        sb.append("转发 ").append(running).append('/').append(total).append(" 运行");
        if (c.proxyPort > 0) {
            sb.append(" · 代理 ").append(c.bind).append(':').append(c.proxyPort)
                    .append(proxyRunning ? " 运行中" : " 已停止");
        } else {
            sb.append(" · 代理未启用");
        }
        return sb.toString();
    }
}
