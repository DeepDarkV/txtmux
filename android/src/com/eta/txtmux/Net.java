package com.eta.txtmux;

import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;

/**
 * 纯工具：URL 解析、host:port 解析、域名通配匹配。
 * 与 Windows 版 txtmux.c 的 parse_url / parse_endpoint / host_match 等价。
 */
public final class Net {

    private Net() {
    }

    /** http(s)://host[:port][/path] */
    public static final class Url {
        public String scheme = "http";
        public String host = "";
        public int port = 80;
        public String path = "/";

        public boolean secure() {
            return "https".equalsIgnoreCase(scheme);
        }

        public String hostHeader() {
            boolean def = (secure() ? 443 : 80) == port;
            return def ? host : (host + ":" + port);
        }
    }

    public static Url parseUrl(String url) {
        if (url == null) return null;
        String s = url.trim();
        String lower = s.toLowerCase();
        boolean secure;
        int skip;
        if (lower.startsWith("https://")) {
            secure = true;
            skip = 8;
        } else if (lower.startsWith("http://")) {
            secure = false;
            skip = 7;
        } else {
            return null;
        }
        Url u = new Url();
        u.scheme = secure ? "https" : "http";
        u.port = secure ? 443 : 80;
        String rest = s.substring(skip);

        int i = 0;
        while (i < rest.length()) {
            char c = rest.charAt(i);
            if (c == '/' || c == '?' || c == '#' || c == ':') break;
            i++;
        }
        u.host = rest.substring(0, i).trim();
        if (u.host.isEmpty()) return null;

        if (i < rest.length() && rest.charAt(i) == ':') {
            i++;
            int start = i;
            while (i < rest.length() && rest.charAt(i) >= '0' && rest.charAt(i) <= '9' && i - start < 5) i++;
            if (i == start) return null;
            int p;
            try {
                p = Integer.parseInt(rest.substring(start, i));
            } catch (NumberFormatException e) {
                return null;
            }
            if (p <= 0 || p > 65535) return null;
            u.port = p;
        }
        u.path = (i < rest.length() && rest.charAt(i) == '/') ? rest.substring(i) : "/";
        return u;
    }

    /** "host:port" / "[v6]:port" */
    public static final class Endpoint {
        public String host = "";
        public int port;
    }

    public static Endpoint parseEndpoint(String raw) {
        if (raw == null) return null;
        String s = raw.trim();
        if (s.isEmpty()) return null;
        Endpoint ep = new Endpoint();
        if (s.charAt(0) == '[') {
            int close = s.indexOf(']');
            if (close < 0) return null;
            ep.host = s.substring(1, close);
            if (close + 1 >= s.length() || s.charAt(close + 1) != ':') return null;
            ep.port = parseInt(s.substring(close + 2));
        } else {
            int c = s.indexOf(':');
            if (c <= 0 || c != s.lastIndexOf(':')) return null;  /* 没端口或裸 IPv6 */
            ep.host = s.substring(0, c).trim();
            ep.port = parseInt(s.substring(c + 1));
        }
        if (ep.host.isEmpty() || ep.port <= 0 || ep.port > 65535) return null;
        return ep;
    }

    public static int parseInt(String s) {
        if (s == null) return 0;
        s = s.trim();
        if (s.isEmpty()) return 0;
        try {
            long v = Long.parseLong(s);
            if (v < 0 || v > Integer.MAX_VALUE) return 0;
            return (int) v;
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    /** pattern 支持 *（可跨点），大小写不敏感 */    public static boolean hostMatch(String pattern, String host) {
        return match(pattern == null ? "" : pattern, host == null ? "" : host);
    }

    private static boolean match(String p, String h) {
        int pi = 0;
        int hi = 0;
        while (pi < p.length()) {
            char c = p.charAt(pi);
            if (c == '*') {
                pi++;
                if (pi == p.length()) return true;
                for (int k = hi; k <= h.length(); k++) {
                    if (match(p.substring(pi), h.substring(k))) return true;
                }
                return false;
            }
            if (hi >= h.length()) return false;
            if (Character.toLowerCase(c) != Character.toLowerCase(h.charAt(hi))) return false;
            pi++;
            hi++;
        }
        return hi == h.length();
    }

    /* ---------- 建连：多地址同时尝试（IPv6 黑洞时不必等超时） ---------- */

    public static Socket connectAny(String host, int port, int timeoutMs) throws IOException {
        InetAddress[] addrs = InetAddress.getAllByName(host);
        if (addrs == null || addrs.length == 0) throw new IOException("解析 " + host + " 失败");
        if (addrs.length == 1) return connectOne(addrs[0], port, timeoutMs);

        final java.util.concurrent.CountDownLatch latch = new java.util.concurrent.CountDownLatch(1);
        final Socket[] win = new Socket[1];
        final java.util.concurrent.atomic.AtomicReference<IOException> last =
                new java.util.concurrent.atomic.AtomicReference<IOException>();
        for (final InetAddress a : addrs) {
            Thread t = new Thread(new Runnable() {
                @Override
                public void run() {
                    try {
                        Socket s = connectOne(a, port, timeoutMs);
                        synchronized (win) {
                            if (win[0] == null) {
                                win[0] = s;
                                latch.countDown();
                            } else {
                                try {
                                    s.close();
                                } catch (Exception ignored) {
                                }
                            }
                        }
                    } catch (IOException e) {
                        last.set(e);
                    }
                }
            }, "txtmux-connect");
            t.setDaemon(true);
            t.start();
        }
        try {
            latch.await(timeoutMs + 500L, java.util.concurrent.TimeUnit.MILLISECONDS);
        } catch (InterruptedException ignored) {
        }
        Socket s;
        synchronized (win) {
            s = win[0];
        }
        if (s != null) return s;
        IOException e = last.get();
        throw e != null ? e : new IOException("无法连接 " + host + ":" + port);
    }

    static Socket connectOne(InetAddress addr, int port, int timeoutMs) throws IOException {
        Socket s = new Socket();
        try {
            s.setTcpNoDelay(true);
            s.connect(new InetSocketAddress(addr, port), timeoutMs);
            return s;
        } catch (IOException e) {
            try {
                s.close();
            } catch (Exception ignored) {
            }
            throw e;
        }
    }
}
