package com.eta.txtmux;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkProperties;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Random;

/**
 * TXT 记录查询：先用系统 DNS 走 UDP 53，失败再走 DoH（POST + wire format）。
 * 只取第一条非空的 TXT 值，多段字符串会拼接（与 Windows 版一致）。
 */
public final class Dns {

    private static final int TIMEOUT = 2500;
    private static final int MAX_PACKET = 4096;

    private Dns() {
    }

    public static String txt(Context ctx, String name) {
        byte[] query = buildQuery(name);
        if (query == null) return null;
        for (String server : servers(ctx)) {
            String v = udp(server, query);
            if (v != null) return v;
        }
        String[] doh = {
                "https://doh.pub/dns-query",
                "https://223.5.5.5/dns-query",
                "https://1.1.1.1/dns-query"
        };
        for (String url : doh) {
            String v = dohPost(url, query);
            if (v != null) return v;
        }
        return null;
    }

    public static List<String> servers(Context ctx) {
        LinkedHashSet<String> out = new LinkedHashSet<String>();
        try {
            ConnectivityManager cm = (ConnectivityManager) ctx.getSystemService(Context.CONNECTIVITY_SERVICE);
            if (cm != null) {
                LinkProperties lp = cm.getLinkProperties(cm.getActiveNetwork());
                if (lp != null) {
                    List<InetAddress> list = lp.getDnsServers();
                    for (InetAddress a : list) {
                        String h = a.getHostAddress();
                        if (h != null && !h.isEmpty()) out.add(h);
                    }
                }
            }
        } catch (Throwable ignored) {
        }
        out.add("223.5.5.5");
        out.add("114.114.114.114");
        out.add("8.8.8.8");
        return new ArrayList<String>(out);
    }

    /* ---------- 报文 ---------- */

    static byte[] buildQuery(String name) {
        if (name == null) return null;
        String n = name.trim();
        while (n.endsWith(".")) n = n.substring(0, n.length() - 1);
        if (n.isEmpty()) return null;
        for (int i = 0; i < n.length(); i++) {
            char c = n.charAt(i);
            if (c > 0x7f) return null;     /* 不做 IDN punycode */
        }
        byte[] labels = n.getBytes(StandardCharsets.US_ASCII);
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        int id = new Random().nextInt(65536);
        bo.write((id >> 8) & 0xff);
        bo.write(id & 0xff);
        bo.write(0x01);
        bo.write(0x00);          /* RD */
        bo.write(0);
        bo.write(1);             /* QDCOUNT = 1 */
        bo.write(0);
        bo.write(0);             /* ANCOUNT */
        bo.write(0);
        bo.write(0);             /* NSCOUNT */
        bo.write(0);
        bo.write(0);             /* ARCOUNT */
        int start = 0;
        for (int i = 0; i <= labels.length; i++) {
            if (i == labels.length || labels[i] == '.') {
                int len = i - start;
                if (len <= 0 || len > 63) return null;
                bo.write(len);
                bo.write(labels, start, len);
                start = i + 1;
            }
        }
        bo.write(0);
        bo.write(0);
        bo.write(16);            /* TYPE = TXT */
        bo.write(0);
        bo.write(1);             /* CLASS = IN */
        return bo.toByteArray();
    }

    private static String udp(String server, byte[] query) {
        DatagramSocket ds = null;
        try {
            InetAddress addr = InetAddress.getByName(server);
            ds = new DatagramSocket();
            ds.setSoTimeout(TIMEOUT);
            ds.send(new DatagramPacket(query, query.length, new InetSocketAddress(addr, 53)));
            byte[] buf = new byte[MAX_PACKET];
            long deadline = System.currentTimeMillis() + TIMEOUT;
            while (System.currentTimeMillis() < deadline) {
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                ds.receive(p);
                if (p.getLength() < 12) continue;
                if (buf[0] != query[0] || buf[1] != query[1]) continue;   /* ID 不匹配 */
                String v = parseTxt(buf, p.getLength());
                if (v != null) return v;
                return null;
            }
        } catch (Throwable ignored) {
        } finally {
            if (ds != null) {
                try {
                    ds.close();
                } catch (Exception ignored) {
                }
            }
        }
        return null;
    }

    private static String dohPost(String url, byte[] query) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(4000);
            c.setReadTimeout(5000);
            c.setRequestMethod("POST");
            c.setRequestProperty("Content-Type", "application/dns-message");
            c.setRequestProperty("Accept", "application/dns-message");
            c.setDoOutput(true);
            c.setFixedLengthStreamingMode(query.length);
            OutputStream os = c.getOutputStream();
            os.write(query);
            os.flush();
            os.close();
            if (c.getResponseCode() != 200) return null;
            InputStream in = c.getInputStream();
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            byte[] buf = new byte[2048];
            int n;
            while ((n = in.read(buf)) > 0 && bo.size() < MAX_PACKET * 4) bo.write(buf, 0, n);
            in.close();
            byte[] d = bo.toByteArray();
            return parseTxt(d, d.length);
        } catch (Throwable ignored) {
            return null;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    static String parseTxt(byte[] d, int len) {
        if (d == null || len < 12) return null;
        int flags = ((d[2] & 0xff) << 8) | (d[3] & 0xff);
        if ((flags & 0x000f) != 0) return null;                 /* RCODE != 0 */
        int qd = u16(d, 4);
        int an = u16(d, 6);
        int pos = 12;
        try {
            for (int i = 0; i < qd; i++) {
                pos = skipName(d, pos, len);
                pos += 4;
            }
            for (int i = 0; i < an; i++) {
                pos = skipName(d, pos, len);
                if (pos + 10 > len) return null;
                int type = u16(d, pos);
                pos += 8;                                        /* TYPE(2)+CLASS(2)+TTL(4) */
                int rdlen = u16(d, pos);
                pos += 2;
                if (pos + rdlen > len) return null;
                if (type == 16) {
                    StringBuilder sb = new StringBuilder();
                    int p = pos;
                    int end = pos + rdlen;
                    while (p < end) {
                        int l = d[p] & 0xff;
                        p++;
                        if (l == 0 || p + l > end) break;
                        sb.append(new String(d, p, l, StandardCharsets.UTF_8));
                        p += l;
                    }
                    String v = sb.toString().trim();
                    if (!v.isEmpty()) return v;
                }
                pos += rdlen;
            }
        } catch (Throwable ignored) {
        }
        return null;
    }

    private static int u16(byte[] d, int pos) {
        if (pos + 1 >= d.length) return 0;
        return ((d[pos] & 0xff) << 8) | (d[pos + 1] & 0xff);
    }

    private static int skipName(byte[] d, int pos, int len) {
        while (pos < len) {
            int l = d[pos] & 0xff;
            if (l == 0) return pos + 1;
            if ((l & 0xc0) == 0xc0) return pos + 2;
            pos += 1 + l;
        }
        return pos;
    }
}
