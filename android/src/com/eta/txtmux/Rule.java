package com.eta.txtmux;

import java.net.ServerSocket;

/** 一条转发规则：来源（TXT 记录名 / 入口 URL）+ 本地端口 + 可选匹配域名。 */
public class Rule {

    /* ---- 配置（持久化） ---- */
    public String src = "";
    public String match = "";
    public int port;
    public boolean enabled = true;
    /** 上一次是否处于运行状态（点过全部启动/单独启动为 true，全部停止为 false） */
    public boolean active = true;

    /* ---- 运行时 ---- */
    final Object lock = new Object();
    volatile ServerSocket ss;
    volatile java.net.DatagramSocket udp;
    volatile Thread thread;
    volatile boolean running;
    volatile boolean stopFlag;
    volatile String targetHost = "";
    volatile int targetPort;
    volatile long targetAt;
    volatile boolean hasTarget;
    volatile boolean resolving;
    volatile long lastTryAt;
    volatile String lastError = "";

    public boolean isUrl() {
        return src.startsWith("http://") || src.startsWith("https://");
    }

    /** 日志/列表里显示的短名：URL 取主机名 */
    public String name() {
        if (isUrl()) {
            Net.Url u = Net.parseUrl(src);
            if (u != null) return u.host;
        }
        return src;
    }

    public String stateText() {
        boolean err = lastError != null && !lastError.isEmpty();
        if (running) {
            if (resolving) return "监听中 · 正在解析目标…";
            if (hasTarget) return "监听中 → " + targetHost + ":" + targetPort + (err ? "（最近一次解析失败）" : "");
            if (err) return "监听中 · 目标解析失败：" + lastError;
            return "监听中（还没有解析过目标，第一次连接时解析）";
        }
        if (err) return "已停止 · 上次失败：" + lastError;
        if (!enabled) return "已禁用";
        return "已停止";
    }
}
