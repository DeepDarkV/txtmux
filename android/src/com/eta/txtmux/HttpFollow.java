package com.eta.txtmux;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.security.cert.X509Certificate;
import java.util.Locale;

import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLException;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509TrustManager;

/**
 * 请求入口 URL 并跟随 3xx，返回最终 URL（对应 Windows 版的 WinHTTP 跟随）。
 * 直连、不经过系统代理，避免代理指向自己时形成自指循环。
 */
public final class HttpFollow {

    private static final int TIMEOUT = 8000;
    private static final int MAX_HEAD = 16384;

    private HttpFollow() {
    }

    public static String finalUrl(String url) throws IOException {
        String cur = url;
        for (int hop = 0; hop < 8; hop++) {
            Net.Url u = Net.parseUrl(cur);
            if (u == null) throw new IOException("非法 URL: " + cur);
            int[] status = new int[1];
            String loc = fetch(u, status);
            if (status[0] >= 300 && status[0] < 400 && loc != null && !loc.isEmpty()) {
                cur = resolve(cur, loc);
                continue;
            }
            if (status[0] == 0) throw new IOException("没有收到响应");
            return cur;
        }
        throw new IOException("跳转次数过多");
    }

    static String resolve(String base, String loc) {
        String l = loc.trim();
        String lower = l.toLowerCase(Locale.US);
        if (lower.startsWith("http://") || lower.startsWith("https://")) return l;
        Net.Url b = Net.parseUrl(base);
        if (b == null) return l;
        if (l.startsWith("//")) return b.scheme + ":" + l;
        String origin = b.scheme + "://" + b.hostHeader();
        if (l.startsWith("/")) return origin + l;
        String path = b.path;
        int slash = path.lastIndexOf('/');
        String dir = slash >= 0 ? path.substring(0, slash + 1) : "/";
        return origin + dir + l;
    }

    private static String fetch(Net.Url u, int[] status) throws IOException {
        try {
            return fetch0(u, status, false);
        } catch (SSLException e) {
            Logx.line("WARN 证书校验失败(%s)：%s，改为忽略证书重试", u.host, e.getMessage());
            return fetch0(u, status, true);
        }
    }

    private static String fetch0(Net.Url u, int[] status, boolean insecure) throws IOException {
        Socket raw;
        Socket s;
        raw = Net.connectAny(u.host, u.port, TIMEOUT);
        raw.setSoTimeout(TIMEOUT);
        s = raw;
        try {
            if (u.secure()) {
                SSLSocketFactory f = insecure ? insecureFactory() : (SSLSocketFactory) SSLSocketFactory.getDefault();
                SSLSocket ssl = (SSLSocket) f.createSocket(raw, u.host, u.port, true);
                if (!insecure) {
                    SSLParameters p = ssl.getSSLParameters();
                    p.setEndpointIdentificationAlgorithm("HTTPS");
                    ssl.setSSLParameters(p);
                }
                ssl.startHandshake();
                s = ssl;
            }
            String req = "GET " + u.path + " HTTP/1.1\r\n"
                    + "Host: " + u.hostHeader() + "\r\n"
                    + "User-Agent: txtmux-android/1.0\r\n"
                    + "Accept: */*\r\n"
                    + "Connection: close\r\n\r\n";
            OutputStream os = s.getOutputStream();
            os.write(req.getBytes(StandardCharsets.ISO_8859_1));
            os.flush();
            String head = readHead(s.getInputStream());
            status[0] = parseStatus(head);
            return parseLocation(head);
        } finally {
            try {
                s.close();
            } catch (Exception ignored) {
            }
            try {
                raw.close();
            } catch (Exception ignored) {
            }
        }
    }

    private static String readHead(InputStream in) throws IOException {
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        int b;
        int win = 0;
        while (bo.size() < MAX_HEAD) {
            b = in.read();
            if (b < 0) break;
            bo.write(b);
            win = ((win << 8) | (b & 0xff)) & 0xffffffff;
            if (win == 0x0d0a0d0a || win == 0x0a0a) break;
        }
        return new String(bo.toByteArray(), StandardCharsets.ISO_8859_1);
    }

    private static int parseStatus(String head) {
        if (head == null || head.length() < 12) return 0;
        int sp = head.indexOf(' ');
        if (sp < 0 || sp + 4 > head.length()) return 0;
        return Net.parseInt(head.substring(sp + 1, Math.min(head.length(), sp + 4)));
    }

    private static String parseLocation(String head) {
        if (head == null) return null;
        String[] lines = head.split("\r?\n");
        for (int i = 1; i < lines.length; i++) {
            String l = lines[i];
            int c = l.indexOf(':');
            if (c <= 0) continue;
            if (l.substring(0, c).trim().equalsIgnoreCase("Location")) {
                return l.substring(c + 1).trim();
            }
        }
        return null;
    }

    private static SSLSocketFactory insecureFactory() {
        try {
            SSLContext ctx = SSLContext.getInstance("TLS");
            ctx.init(null, new TrustManager[]{new X509TrustManager() {
                @Override
                public void checkClientTrusted(X509Certificate[] chain, String authType) {
                }

                @Override
                public void checkServerTrusted(X509Certificate[] chain, String authType) {
                }

                @Override
                public X509Certificate[] getAcceptedIssuers() {
                    return new X509Certificate[0];
                }
            }}, new java.security.SecureRandom());
            return ctx.getSocketFactory();
        } catch (Exception e) {
            return (SSLSocketFactory) SSLSocketFactory.getDefault();
        }
    }
}
