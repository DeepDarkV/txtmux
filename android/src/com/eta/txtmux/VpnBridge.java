package com.eta.txtmux;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.VpnService;
import android.os.Build;
import android.os.ParcelFileDescriptor;

import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Random;

/**
 * 无感分流：VpnService 只用 DNS 假 IP 拿到域名，再把命中「匹配域名」的连接
 * 转给我们自己的 TCP 栈 → 解析 TXT/307 得到真实端点 → 直连。
 *
 * 关键点：
 *   - VPN 只接管 198.18.0.0/15（假 IP）和它自己的 DNS 地址，其它流量完全不走 VPN；
 *   - 只有命中规则的域名才会被分到假 IP，所以不会被无关流量拖慢；
 *   - 出站 socket 全部 protect()，避免自己的流量再被 VPN 抓回来。
 */
public class VpnBridge extends VpnService {

    public static final String ACTION_START = "com.eta.txtmux.VPN_START";
    public static final String ACTION_STOP = "com.eta.txtmux.VPN_STOP";
    public static final int NOTIF_ID = 1002;
    private static final String CHANNEL = "txtmux_vpn";

    private static final int MTU = 1500;
    private static final int FAKE_PREFIX = 15;
    private static final String FAKE_NET = "198.18.0.0";
    /** VPN 自己的地址（客户端连出去的源地址就是这个） */
    private static final int SELF_IP = ip2int("198.18.0.1");
    /** DNS 服务器地址：必须与 VPN 地址不同，否则查询会被内核当本地地址处理，永远进不了 tun */
    private static final String DNS_IP = "198.18.0.2";
    private static final int FAKE_BASE = ip2int("198.18.1.1");
    private static final int FAKE_LAST = ip2int("198.18.255.254");
    private static final int TTL = 60;
    private static final int MSS_DATA = 1400;

    static volatile boolean running = false;
    /** 是否是「被别的 VPN 顶掉」而停的（只有这种情况才自动恢复） */
    static volatile boolean suspendedByOther = false;
    static final String PREFS = "txtmux_vpn";

    public static void setWanted(android.content.Context c, boolean v) {
        try {
            c.getSharedPreferences(PREFS, 0).edit().putBoolean("want", v).apply();
        } catch (Throwable ignored) {
        }
    }

    public static boolean wanted(android.content.Context c) {
        try {
            return c.getSharedPreferences(PREFS, 0).getBoolean("want", false);
        } catch (Throwable ignored) {
            return false;
        }
    }

    private ParcelFileDescriptor pfd;
    private FileInputStream tin;
    private FileOutputStream tout;
    private Thread loop;
    private Thread retx;
    private volatile boolean stop;
    private volatile String ourIface = "";
    private volatile long vpnUpAt = 0;
    private volatile boolean stopping = false;
    private android.net.ConnectivityManager.NetworkCallback netCb;

    private final Map<Integer, String> fakeToDomain = new HashMap<Integer, String>();
    private final Map<String, Integer> domainToFake = new HashMap<String, Integer>();
    private int fakeNext = FAKE_BASE;

    private final Map<String, TcpConn> conns = new HashMap<String, TcpConn>();
    private final Random rnd = new Random();

    /* ---------------- 生命周期 ---------------- */

    @Override
    public void onCreate() {
        super.onCreate();
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm != null && Build.VERSION.SDK_INT >= 26) {
            NotificationChannel ch = new NotificationChannel(CHANNEL, "无感分流",
                    NotificationManager.IMPORTANCE_LOW);
            nm.createNotificationChannel(ch);
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent == null ? null : intent.getAction();
        if (ACTION_STOP.equals(action)) {
            stopVpn();
            return START_NOT_STICKY;
        }
        try {
            Notification.Builder b = Build.VERSION.SDK_INT >= 26
                    ? new Notification.Builder(this, CHANNEL) : new Notification.Builder(this);
            Intent open = new Intent(this, MainActivity.class)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
            int piFlags = Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0;
            b.setSmallIcon(android.R.drawable.stat_notify_sync)
                    .setContentTitle("txtmux 无感分流")
                    .setContentText("按域名改写端口中（VPN 仅接管假 IP 段）")
                    .setOngoing(true)
                    .setOnlyAlertOnce(true)
                    .setContentIntent(PendingIntent.getActivity(this, 0, open, piFlags));
            if (Build.VERSION.SDK_INT >= 34) {
                startForeground(NOTIF_ID, b.build(), ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            } else {
                startForeground(NOTIF_ID, b.build());
            }
        } catch (Throwable t) {
            Logx.line("WARN VPN 前台服务失败: %s", t.getMessage());
        }
        if (!running) startTun();
        return START_STICKY;
    }

    @Override
    public void onRevoke() {
        Logx.line("VPN 授权被系统收回（通常是有别的 VPN 接管）");
        suspendedByOther = true;      /* 记下来：这是被顶掉，不是用户手动关的 */
        stopVpn();
        super.onRevoke();
    }

    @Override
    public void onDestroy() {
        stopVpn();
        super.onDestroy();
    }

    /** 别的 VPN 一接管默认网络，就只关掉无感分流，端口转发保留 */
    private void watchDefaultNetwork() {
        try {
            android.net.ConnectivityManager cm =
                    (android.net.ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
            if (cm == null) return;
            android.net.LinkProperties lp = cm.getLinkProperties(cm.getActiveNetwork());
            if (lp != null && lp.getInterfaceName() != null) ourIface = lp.getInterfaceName();
            vpnUpAt = System.currentTimeMillis();
            if (netCb != null) return;
            netCb = new android.net.ConnectivityManager.NetworkCallback() {
                @Override
                public void onCapabilitiesChanged(android.net.Network network,
                                                  android.net.NetworkCapabilities caps) {
                    if (!running) return;
                    if (!caps.hasTransport(android.net.NetworkCapabilities.TRANSPORT_VPN)) return;
                    String ifn = "";
                    try {
                        android.net.ConnectivityManager cm2 =
                                (android.net.ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
                        android.net.LinkProperties p = cm2.getLinkProperties(network);
                        if (p != null) ifn = p.getInterfaceName() == null ? "" : p.getInterfaceName();
                    } catch (Throwable ignored) {
                    }
                    if (ifn.equals(ourIface)) return;                          /* 接口名对得上，是我们自己 */
                    if (System.currentTimeMillis() - vpnUpAt < 8000) {          /* 刚建立：认领为自己的 tun */
                        ourIface = ifn;
                        return;
                    }
                    suspendedByOther = true;
                    Logx.line("检测到其它 VPN 建立连接（%s），自动关闭无感分流，端口转发保持不变", ifn);
                    new android.os.Handler(android.os.Looper.getMainLooper()).post(new Runnable() {
                        @Override
                        public void run() {
                            stopVpn();
                        }
                    });
                }
            };
            cm.registerDefaultNetworkCallback(netCb);
        } catch (Throwable t) {
            Logx.line("WARN 监听默认网络失败: %s", t.getMessage());
        }
    }

    private void stopVpn() {
        if (stopping) return;
        stopping = true;
        stop = true;
        running = false;
        closeAllConns();
        try {
            if (loop != null) loop.interrupt();
        } catch (Throwable ignored) {
        }
        try {
            if (tin != null) tin.close();
        } catch (Throwable ignored) {
        }
        try {
            if (tout != null) tout.close();
        } catch (Throwable ignored) {
        }
        try {
            if (pfd != null) pfd.close();
        } catch (Throwable ignored) {
        }
        pfd = null;
        tin = null;
        tout = null;
        try {
            if (netCb != null) {
                android.net.ConnectivityManager cm =
                        (android.net.ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
                if (cm != null) cm.unregisterNetworkCallback(netCb);
            }
        } catch (Throwable ignored) {
        }
        netCb = null;
        ourIface = "";
        try {
            if (Engine.get().isAnythingRunning()) MuxService.start(this);
        } catch (Throwable ignored) {
        }
        if (Build.VERSION.SDK_INT >= 24) stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
        Logx.line("无感分流已停止");
    }

    private void startTun() {
        try {
            Builder b = new Builder();
            b.setSession("txtmux");
            b.setMtu(MTU);
            b.addAddress("198.18.0.1", 32);
            b.addRoute(FAKE_NET, FAKE_PREFIX);
            b.addDnsServer(DNS_IP);
            if (Build.VERSION.SDK_INT >= 29) b.setMetered(false);
            pfd = b.establish();
            if (pfd == null) {
                Logx.line("WARN VPN 建立失败（没有授权？）");
                stopSelf();
                return;
            }
            tin = new FileInputStream(pfd.getFileDescriptor());
            tout = new FileOutputStream(pfd.getFileDescriptor());
            stop = false;
            stopping = false;
            running = true;
            loop = new Thread(new Runnable() {
                @Override
                public void run() {
                    packetLoop();
                }
            }, "txtmux-tun");
            loop.start();
            retx = new Thread(new Runnable() {
                @Override
                public void run() {
                    retransmitLoop();
                }
            }, "txtmux-retx");
            retx.start();
            setWanted(this, true);
            suspendedByOther = false;
            Logx.line("无感分流已开启：命中「匹配域名」的访问会被自动改写到真实端点");
            /* 保证端口转发进程不因为 VPN 结束而被回收 */
            MuxService.start(this);
            watchDefaultNetwork();
        } catch (Throwable t) {
            Logx.line("WARN VPN 启动异常: %s", t.getMessage());
            stopSelf();
        }
    }

    /* ---------------- 收包 ---------------- */

    private void packetLoop() {
        byte[] buf = new byte[MTU + 512];
        while (!stop) {
            int n;
            try {
                n = tin.read(buf);
            } catch (Throwable t) {
                if (!stop) {
                    Logx.line("VPN 收包线程异常（%s），自动关闭以免断网", t.getMessage());
                    stopVpn();
                }
                return;
            }
            if (n <= 0) continue;
            try {
                dispatch(buf, n);
            } catch (Throwable t) {
                /* 单包异常不影响整体 */
            }
        }
    }

    private void dispatch(byte[] p, int n) {
        if (n < 20 || (p[0] & 0xf0) != 0x40) return;
        int ihl = (p[0] & 0x0f) * 4;
        int total = u16(p, 2);
        if (total > n) total = n;
        int proto = p[9] & 0xff;
        int srcIp = s32(p, 12);
        int dstIp = s32(p, 16);
        if (proto == 17) {
            udp(p, ihl, total, srcIp, dstIp);
        } else if (proto == 6) {
            tcp(p, ihl, total, srcIp, dstIp);
        }
    }

    /* ---------------- DNS（假 IP） ---------------- */

    private void udp(byte[] p, int ihl, int total, int srcIp, int dstIp) {
        int sport = u16(p, ihl);
        int dport = u16(p, ihl + 2);
        if (dport != 53) return;
        int ulen = u16(p, ihl + 4);
        int off = ihl + 8;
        int len = Math.min(ulen - 8, total - off);
        if (len <= 0) return;
        byte[] q = new byte[len];
        System.arraycopy(p, off, q, 0, len);
        byte[] resp = answerDns(q);
        if (resp != null) sendUdp(dstIp, srcIp, dport, sport, resp);
    }

    private byte[] answerDns(byte[] q) {
        if (q.length < 12) return null;
        int qd = u16(q, 4);
        if (qd < 1) return null;
        /* 取第一个问题的名字 */
        int pos = 12;
        StringBuilder name = new StringBuilder();
        while (pos < q.length) {
            int l = q[pos] & 0xff;
            if (l == 0) {
                pos++;
                break;
            }
            if ((l & 0xc0) != 0) {
                pos += 2;
                break;
            }
            if (pos + 1 + l > q.length) return null;
            if (name.length() > 0) name.append('.');
            name.append(new String(q, pos + 1, l, java.nio.charset.StandardCharsets.ISO_8859_1));
            pos += 1 + l;
        }
        if (pos + 4 > q.length) return null;
        int qtype = u16(q, pos);
        int qend = pos + 4;
        String host = name.toString().toLowerCase(Locale.US);
        Rule rule = ruleFor(host);
        if (rule == null) return forwardDns(q, q.length);
        if (qtype != 1) {
            /* 只给 A；AAAA 回 NODATA，逼客户端走 IPv4 */
            return dnsNoData(q, qend);
        }
        int ip = fakeFor(host);
        if (ip == 0) return dnsNoData(q, qend);
        Logx.line("[VPN] DNS %s -> %d.%d.%d.%d", host,
                (ip >>> 24) & 0xff, (ip >>> 16) & 0xff, (ip >>> 8) & 0xff, ip & 0xff);
        byte[] r = new byte[qend + 16];
        System.arraycopy(q, 0, r, 0, qend);
        r[2] = (byte) 0x81;
        r[3] = (byte) 0x80;
        r[6] = 0;
        r[7] = 1;      /* ANCOUNT = 1 */
        r[8] = 0;
        r[9] = 0;
        r[10] = 0;
        r[11] = 0;
        int o = qend;
        r[o++] = (byte) 0xc0;
        r[o++] = 0x0c;                 /* 指向问题里的名字 */
        r[o++] = 0;
        r[o++] = 1;                    /* TYPE A */
        r[o++] = 0;
        r[o++] = 1;                    /* CLASS IN */
        r[o++] = 0;
        r[o++] = 0;
        r[o++] = 0;
        r[o++] = (byte) TTL;
        r[o++] = 0;
        r[o++] = 4;
        r[o++] = (byte) ((ip >>> 24) & 0xff);
        r[o++] = (byte) ((ip >>> 16) & 0xff);
        r[o++] = (byte) ((ip >>> 8) & 0xff);
        r[o] = (byte) (ip & 0xff);
        return r;
    }

    private byte[] dnsNoData(byte[] q, int qend) {
        byte[] r = new byte[qend];
        System.arraycopy(q, 0, r, 0, qend);
        r[2] = (byte) 0x81;
        r[3] = (byte) 0x80;
        for (int i = 6; i < 12; i++) r[i] = 0;
        return r;
    }

    private byte[] forwardDns(byte[] q, int len) {
        DatagramSocket ds = null;
        try {
            ds = new DatagramSocket();
            protect(ds);
            ds.setSoTimeout(1200);
            for (String s : Dns.servers(this)) {
                try {
                    InetAddress a = InetAddress.getByName(s);
                    ds.send(new DatagramPacket(q, len, new InetSocketAddress(a, 53)));
                    byte[] buf = new byte[4096];
                    DatagramPacket in = new DatagramPacket(buf, buf.length);
                    ds.receive(in);
                    byte[] out = new byte[in.getLength()];
                    System.arraycopy(buf, 0, out, 0, in.getLength());
                    return out;
                } catch (IOException ignored) {
                    /* 换下一个 DNS */
                }
            }
        } catch (Throwable ignored) {
        } finally {
            if (ds != null) ds.close();
        }
        return servFail(q, len);
    }

    /** 上游都失败时回 SERVFAIL，别让 App 干等 */
    private byte[] servFail(byte[] q, int len) {
        byte[] r = new byte[len];
        System.arraycopy(q, 0, r, 0, len);
        r[2] = (byte) 0x81;
        r[3] = (byte) 0x82;      /* QR=1, RA=1, RCODE=2 */
        return r;
    }

    private Rule ruleFor(String host) {
        try {
            return Engine.get().ruleForHost(host);
        } catch (Throwable t) {
            return null;
        }
    }

    private int fakeFor(String host) {
        synchronized (fakeToDomain) {
            Integer ip = domainToFake.get(host);
            if (ip != null) return ip;
            if (fakeNext > FAKE_LAST) fakeNext = FAKE_BASE;
            int candidate = fakeNext++;
            if (candidate == SELF_IP) candidate = fakeNext++;
            fakeToDomain.put(candidate, host);
            domainToFake.put(host, candidate);
            return candidate;
        }
    }

    private String domainFor(int ip) {
        synchronized (fakeToDomain) {
            return fakeToDomain.get(ip);
        }
    }

    private void sendUdp(int srcIp, int dstIp, int sport, int dport, byte[] payload) {
        byte[] pkt = new byte[20 + 8 + payload.length];
        pkt[0] = 0x45;
        int total = pkt.length;
        pkt[2] = (byte) (total >> 8);
        pkt[3] = (byte) total;
        pkt[8] = 64;
        pkt[9] = 17;
        s32(pkt, 12, srcIp);
        s32(pkt, 16, dstIp);
        checksum(pkt, 0, 20);
        int o = 20;
        pkt[o++] = (byte) (sport >> 8);
        pkt[o++] = (byte) sport;
        pkt[o++] = (byte) (dport >> 8);
        pkt[o++] = (byte) dport;
        int ulen = 8 + payload.length;
        pkt[o++] = (byte) (ulen >> 8);
        pkt[o++] = (byte) ulen;
        pkt[o++] = 0;
        pkt[o++] = 0;
        System.arraycopy(payload, 0, pkt, o, payload.length);
        /* UDP 校验和可省略（0） */
        write(pkt, pkt.length);
    }

    /* ---------------- TCP 栈 ---------------- */

    private static final class TcpConn {
        String key;
        String host;
        int clientIp, clientPort, dstIp, dstPort;
        Socket remote;
        InputStream rin;
        OutputStream rout;
        int sndNxt, sndUna, rcvNxt;
        int clientWnd = 65535;
        volatile boolean closed;
        volatile boolean finSent;
        final List<byte[]> pending = new ArrayList<byte[]>();   /* 已发未确认的段 */
        final List<Integer> pendingSeq = new ArrayList<Integer>();
        volatile long lastAck = System.currentTimeMillis();
        Thread reader;
    }

    private void tcp(byte[] p, int ihl, int total, int srcIp, int dstIp) {
        int sport = u16(p, ihl);
        int dport = u16(p, ihl + 2);
        int seq = s32(p, ihl + 4);
        int ack = s32(p, ihl + 8);
        int dataOff = ((p[ihl + 12] & 0xf0) >> 4) * 4;
        int flags = p[ihl + 13] & 0xff;
        int wnd = u16(p, ihl + 14);
        int payOff = ihl + dataOff;
        int payLen = total - payOff;
        if (payLen < 0) payLen = 0;

        String key = srcIp + ":" + sport + ">" + dstIp + ":" + dport;
        TcpConn c;
        synchronized (conns) {
            c = conns.get(key);
        }

        boolean syn = (flags & 0x02) != 0;
        boolean rst = (flags & 0x04) != 0;

        if (syn) {
            if (c != null) {
                /* 重传的 SYN：重发 SYN-ACK */
                sendSeg(c, c.sndNxt - 1, c.rcvNxt, 0x12, null, 0, 0, 65535);
                return;
            }
            String host = domainFor(dstIp);
            if (host == null) {
                /* 不属于假 IP 段，直接 RST */
                sendRst(srcIp, dstIp, sport, dport, seq + 1, 0);
                return;
            }
            Rule rule = ruleFor(host);
            if (rule == null) {
                sendRst(srcIp, dstIp, sport, dport, seq + 1, 0);
                return;
            }
            Engine.Target target = Engine.get().targetForRule(rule);
            if (target == null) {
                Logx.line("[VPN] %s 没有可用目标，拒绝连接", host);
                sendRst(srcIp, dstIp, sport, dport, seq + 1, 0);
                return;
            }
            Socket s = connect(target.host, target.port);
            if (s == null) {
                sendRst(srcIp, dstIp, sport, dport, seq + 1, 0);
                return;
            }
            c = new TcpConn();
            c.key = key;
            c.host = host;
            c.clientIp = srcIp;
            c.clientPort = sport;
            c.dstIp = dstIp;
            c.dstPort = dport;
            c.remote = s;
            c.rcvNxt = seq + 1;
            c.sndNxt = (rnd.nextInt() & 0x7fffffff);
            c.sndUna = c.sndNxt;
            try {
                c.rin = s.getInputStream();
                c.rout = s.getOutputStream();
            } catch (IOException e) {
                closeConn(c);
                sendRst(srcIp, dstIp, sport, dport, seq + 1, 0);
                return;
            }
            synchronized (conns) {
                conns.put(key, c);
            }
            Logx.line("[VPN] %s:%d -> %s:%d", host, dport, target.host, target.port);
            sendSeg(c, c.sndNxt, c.rcvNxt, 0x12, null, 0, 0, wnd);   /* SYN-ACK（带 MSS） */
            c.sndNxt++;
            c.sndUna = c.sndNxt;
            startReader(c);
            return;
        }

        if (c == null) {
            if (rst) return;
            sendRst(dstIp, srcIp, dport, sport, ack, seq + payLen);
            return;
        }

        if (rst) {
            closeConn(c);
            return;
        }
        c.lastAck = System.currentTimeMillis();
        if (wnd > 0) c.clientWnd = wnd;

        if ((flags & 0x10) != 0) {
            /* ACK：滑动发送窗口 */
            synchronized (c.pending) {
                while (!c.pending.isEmpty() && c.pendingSeq.get(0) <= ack) {
                    int s0 = c.pendingSeq.get(0);
                    int l0 = c.pending.get(0).length;
                    if (s0 + l0 <= ack) {
                        c.pending.remove(0);
                        c.pendingSeq.remove(0);
                    } else {
                        break;
                    }
                }
            }
            if (gt(ack, c.sndUna)) c.sndUna = ack;
        }

        if (payLen > 0) {
            if (seq == c.rcvNxt) {
                try {
                    c.rout.write(p, payOff, payLen);
                    c.rout.flush();
                } catch (IOException e) {
                    closeConn(c);
                    sendRst(dstIp, srcIp, dport, sport, c.rcvNxt, c.sndNxt);
                    return;
                }
                c.rcvNxt += payLen;
                sendSeg(c, c.sndNxt, c.rcvNxt, 0x10, null, 0, 0, 65535);
            } else {
                /* 乱序或重复：回 ACK 让对端重传 */
                sendSeg(c, c.sndNxt, c.rcvNxt, 0x10, null, 0, 0, 65535);
            }
        } else if ((flags & 0x01) != 0) {
            c.rcvNxt++;
            sendSeg(c, c.sndNxt, c.rcvNxt, 0x10, null, 0, 0, 65535);
            try {
                c.remote.shutdownOutput();
            } catch (Exception ignored) {
            }
            if (c.finSent) closeConn(c);
        }
    }

    private Socket connect(String host, int port) {
        try {
            Socket s = Net.connectAny(host, port, 8000);
            if (!protect(s)) {
                Logx.line("WARN VPN 出站 socket 无法 protect，可能回环");
            }
            s.setTcpNoDelay(true);
            return s;
        } catch (IOException e) {
            Logx.line("[VPN] 连接 %s:%d 失败（%s）", host, port, e.getMessage());
            return null;
        }
    }

    private void startReader(final TcpConn c) {
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                byte[] buf = new byte[MSS_DATA];
                try {
                    int n;
                    while (!c.closed && (n = c.rin.read(buf)) > 0) {
                        int off = 0;
                        while (off < n && !c.closed) {
                            int chunk = Math.min(MSS_DATA, n - off);
                            while (!c.closed && inflight(c) > 60000) {
                                Thread.sleep(10);
                            }
                            byte[] seg = new byte[chunk];
                            System.arraycopy(buf, off, seg, 0, chunk);
                            int seq = c.sndNxt;
                            synchronized (c.pending) {
                                c.pending.add(seg);
                                c.pendingSeq.add(seq);
                            }
                            sendSeg(c, seq, c.rcvNxt, 0x18, seg, 0, chunk, 65535);
                            c.sndNxt += chunk;
                            off += chunk;
                        }
                    }
                } catch (Throwable ignored) {
                }
                if (!c.closed) {
                    c.finSent = true;
                    sendSeg(c, c.sndNxt, c.rcvNxt, 0x11, null, 0, 0, 65535);
                    c.sndNxt++;
                    waitAckThenClose(c);
                }
            }
        }, "txtmux-tcp-" + c.clientPort);
        c.reader = t;
        t.start();
    }

    private void waitAckThenClose(final TcpConn c) {
        new Thread(new Runnable() {
            @Override
            public void run() {
                long deadline = System.currentTimeMillis() + 10000;
                while (!c.closed && System.currentTimeMillis() < deadline) {
                    synchronized (c.pending) {
                        if (c.pending.isEmpty()) break;
                    }
                    try {
                        Thread.sleep(50);
                    } catch (InterruptedException e) {
                        break;
                    }
                }
                closeConn(c);
            }
        }, "txtmux-fin").start();
    }

    private int inflight(TcpConn c) {
        return c.sndNxt - c.sndUna;
    }

    private void retransmitLoop() {
        while (!stop) {
            try {
                Thread.sleep(250);
            } catch (InterruptedException e) {
                return;
            }
            long now = System.currentTimeMillis();
            List<TcpConn> list;
            synchronized (conns) {
                list = new ArrayList<TcpConn>(conns.values());
            }
            for (TcpConn c : list) {
                if (c.closed) continue;
                if (now - c.lastAck > 600000L) {
                    closeConn(c);
                    continue;
                }
                byte[] seg = null;
                int seq = 0;
                synchronized (c.pending) {
                    if (!c.pending.isEmpty() && now - c.lastAck > 400) {
                        seg = c.pending.get(0);
                        seq = c.pendingSeq.get(0);
                    }
                }
                if (seg != null) {
                    sendSeg(c, seq, c.rcvNxt, 0x18, seg, 0, seg.length, 65535);
                }
            }
        }
    }

    private void closeConn(TcpConn c) {
        if (c.closed) return;
        c.closed = true;
        synchronized (conns) {
            conns.remove(c.key);
        }
        try {
            if (c.remote != null) c.remote.close();
        } catch (Exception ignored) {
        }
    }

    private void closeAllConns() {
        List<TcpConn> list;
        synchronized (conns) {
            list = new ArrayList<TcpConn>(conns.values());
        }
        for (TcpConn c : list) closeConn(c);
    }

    /* ---------------- 发包 ---------------- */

    private void sendSeg(TcpConn c, int seq, int ack, int flags, byte[] data, int off, int len, int wnd) {
        boolean syn = (flags & 0x02) != 0;
        int optLen = syn ? 4 : 0;
        int hdr = 20 + optLen;
        byte[] pkt = new byte[20 + hdr + len];
        pkt[0] = 0x45;
        int total = pkt.length;
        pkt[2] = (byte) (total >> 8);
        pkt[3] = (byte) total;
        pkt[8] = 64;
        pkt[9] = 6;
        s32(pkt, 12, c.dstIp);      /* 源 = 假 IP（客户端看到的目标） */
        s32(pkt, 16, c.clientIp);
        checksum(pkt, 0, 20);
        int o = 20;
        pkt[o++] = (byte) (c.dstPort >> 8);
        pkt[o++] = (byte) c.dstPort;
        pkt[o++] = (byte) (c.clientPort >> 8);
        pkt[o++] = (byte) c.clientPort;
        s32(pkt, o, seq);
        o += 4;
        s32(pkt, o, ack);
        o += 4;
        pkt[o++] = (byte) ((hdr / 4) << 4);
        pkt[o++] = (byte) flags;
        pkt[o++] = (byte) (wnd >> 8);
        pkt[o++] = (byte) wnd;
        pkt[o++] = 0;
        pkt[o++] = 0;
        pkt[o++] = 0;
        pkt[o++] = 0;
        if (syn) {
            pkt[o++] = 2;               /* MSS */
            pkt[o++] = 4;
            pkt[o++] = (byte) (MSS_DATA >> 8);
            pkt[o++] = (byte) MSS_DATA;
        }
        if (len > 0 && data != null) {
            System.arraycopy(data, off, pkt, o, len);
        }
        int tcpLen = hdr + len;
        tcpChecksum(pkt, 20, tcpLen, c.dstIp, c.clientIp);
        write(pkt, pkt.length);
    }

    private void sendRst(int srcIp, int dstIp, int sport, int dport, int seq, int ack) {
        byte[] pkt = new byte[40];
        pkt[0] = 0x45;
        pkt[2] = 0;
        pkt[3] = 40;
        pkt[8] = 64;
        pkt[9] = 6;
        s32(pkt, 12, srcIp);
        s32(pkt, 16, dstIp);
        checksum(pkt, 0, 20);
        int o = 20;
        pkt[o++] = (byte) (sport >> 8);
        pkt[o++] = (byte) sport;
        pkt[o++] = (byte) (dport >> 8);
        pkt[o++] = (byte) dport;
        s32(pkt, o, seq);
        o += 4;
        s32(pkt, o, ack);
        o += 4;
        pkt[o++] = (byte) (5 << 4);
        pkt[o++] = 0x14;   /* RST|ACK */
        pkt[o++] = 0;
        pkt[o++] = 0;
        pkt[o++] = 0;
        pkt[o++] = 0;
        pkt[o++] = 0;
        pkt[o++] = 0;
        tcpChecksum(pkt, 20, 20, srcIp, dstIp);
        write(pkt, pkt.length);
    }

    private void write(byte[] pkt, int len) {
        try {
            tout.write(pkt, 0, len);
            tout.flush();
        } catch (Throwable ignored) {
        }
    }

    /* ---------------- 工具 ---------------- */

    private static int u16(byte[] b, int o) {
        return ((b[o] & 0xff) << 8) | (b[o + 1] & 0xff);
    }

    private static int s32(byte[] b, int o) {
        return ((b[o] & 0xff) << 24) | ((b[o + 1] & 0xff) << 16)
                | ((b[o + 2] & 0xff) << 8) | (b[o + 3] & 0xff);
    }

    private static void s32(byte[] b, int o, int v) {
        b[o] = (byte) ((v >>> 24) & 0xff);
        b[o + 1] = (byte) ((v >>> 16) & 0xff);
        b[o + 2] = (byte) ((v >>> 8) & 0xff);
        b[o + 3] = (byte) (v & 0xff);
    }

    private static int ip2int(String s) {
        try {
            byte[] a = InetAddress.getByName(s).getAddress();
            return s32(a, 0);
        } catch (Exception e) {
            return 0;
        }
    }

    private static boolean gt(int a, int b) {
        return a - b > 0;
    }

    private static void checksum(byte[] b, int off, int len) {
        int sum = 0;
        for (int i = 0; i < len; i += 2) {
            int hi = b[off + i] & 0xff;
            int lo = (i + 1 < len) ? (b[off + i + 1] & 0xff) : 0;
            sum += (hi << 8) | lo;
        }
        while ((sum >>> 16) != 0) sum = (sum & 0xffff) + (sum >>> 16);
        int c = ~sum & 0xffff;
        b[off + 10] = (byte) (c >> 8);
        b[off + 11] = (byte) c;
    }

    private static void tcpChecksum(byte[] pkt, int off, int len, int srcIp, int dstIp) {
        int sum = 0;
        sum += (srcIp >>> 16) & 0xffff;
        sum += srcIp & 0xffff;
        sum += (dstIp >>> 16) & 0xffff;
        sum += dstIp & 0xffff;
        sum += 6;
        sum += len;
        for (int i = 0; i < len; i += 2) {
            int hi = pkt[off + i] & 0xff;
            int lo = (i + 1 < len) ? (pkt[off + i + 1] & 0xff) : 0;
            sum += (hi << 8) | lo;
        }
        while ((sum >>> 16) != 0) sum = (sum & 0xffff) + (sum >>> 16);
        int c = ~sum & 0xffff;
        pkt[off + 16] = (byte) (c >> 8);
        pkt[off + 17] = (byte) c;
    }
}
