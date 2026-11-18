package com.eta.txtmux;

import java.net.ServerSocket;

/** 一条转发规则：来源（TXT 记录名 / 入口 URL）+ 本地端口 + 可选匹配域名。 */
public class Rule {

    /* ---- 配置（持久化） ---- */
    public String src = "";
    public String match = "";
    public int port;
    public boolean enabled = true;

    /* ---- 运行时 ---- */
    final Object lock = new Object();
    volatile ServerSocket ss;
    volatile Thread thread;
    volatile boolean running;
    volatile boolean stopFlag;
    volatile String targetHost = "";
    volatile int targetPort;
    volatile long targetAt;
    volatile boolean hasTarget;
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
        if (running) {
            if (hasTarget) return "运行中 → " + targetHost + ":" + targetPort;
            return "运行中（尚未解析到目标）";
        }
        if (!lastError.isEmpty()) return "失败：" + lastError;
        if (!enabled) return "已禁用";
        return "已停止";
    }
}
