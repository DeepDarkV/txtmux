package com.eta.txtmux;

import android.app.Activity;
import android.content.Intent;
import android.net.VpnService;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.List;

/** 界面：规则编辑/列表 + 代理设置 + 日志。纯代码布局，不依赖任何第三方库。 */
public class MainActivity extends Activity {

    private Engine eng;
    private Cfg cfg;

    private EditText edSrc, edMatch, edPort;
    private EditText edProxy, edBind, edUpstream;
    private CheckBox ckBlock, ckAuto;
    private LinearLayout listBox;
    private TextView statusView, logView, proxyHint, backHint, vpnHint;
    private Button btVpn;
    private static final int REQ_VPN = 21;
    private ScrollView logScroll;
    private Button btAdd;

    private int sel = -1;
    private String listSig = "";
    private String lastStatus = "";
    private String lastHint = "";
    private boolean followLog = true;

    private final Handler ui = new Handler(Looper.getMainLooper());

    private final Logx.Sink sink = new Logx.Sink() {
        @Override
        public void onLog(String line) {
            ui.post(new Runnable() {
                @Override
                public void run() {
                    appendLogLine(line);
                }
            });
        }
    };

    private final Runnable refresh = new Runnable() {
        @Override
        public void run() {
            try {
                eng.healthCheck();
                String st = eng.statusText();
                if (!st.equals(lastStatus)) {
                    lastStatus = st;
                    statusView.setText(st);
                }
                rebuildList();
                updateProxyHint();
                updateVpnHint();
            } catch (Throwable ignored) {
            }
            ui.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        eng = Engine.get();
        eng.attach(this);
        cfg = eng.cfg();
        Logx.init(this, cfg.logMaxMB);
        setContentView(buildUi());
        Logx.addSink(sink);
        String hist = Logx.tail(200);
        logView.setText(hist.isEmpty() ? Logx.text() : hist);
        logView.post(new Runnable() {
            @Override
            public void run() {
                logScroll.scrollTo(0, logView.getMeasuredHeight());
            }
        });
        loadEditors();
        askNotifyPermission();
        Cfg c0 = eng.cfg();
        Logx.line("界面已打开：%d 条规则，代理 %s，监听 %s",
                eng.rules().size(),
                c0.proxyPort > 0 ? String.valueOf(c0.proxyPort) : "未启用",
                c0.bind);
        ui.post(refresh);
        applyStartupState();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (backHint != null) updateBackHint();
        if (cfg != null) updateProxyHint();
    }

    @Override
    protected void onDestroy() {
        Logx.removeSink(sink);
        ui.removeCallbacksAndMessages(null);
        super.onDestroy();
    }

    /* ---------------- 布局 ---------------- */

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private TextView title(String text, float sp, int color) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(sp);
        t.setTextColor(color);
        return t;
    }

    private EditText field(String hint, String value, int inputType) {
        EditText e = new EditText(this);
        e.setHint(hint);
        e.setText(value == null ? "" : value);
        e.setTextSize(14);
        e.setSingleLine(true);
        if (inputType != 0) e.setInputType(inputType);
        return e;
    }

    private Button button(String text, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(13);
        b.setOnClickListener(l);
        return b;
    }

    private LinearLayout row() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        return l;
    }

    private LinearLayout.LayoutParams weightLp(int weight) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, weight);
        lp.setMargins(dp(2), dp(2), dp(2), dp(2));
        return lp;
    }

    private View buildUi() {
        ScrollView root = new ScrollView(this);
        root.setFillViewport(true);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(12), dp(12), dp(12), dp(24));
        root.addView(col);

        col.addView(title("TXT / 入口 URL → 本地端口转发", 17, 0xff1565c0));
        statusView = title("", 13, 0xff333333);
        statusView.setPadding(0, dp(4), 0, dp(8));
        col.addView(statusView);

        /* ---- 编辑区 ---- */
        edSrc = field("TXT 记录名，或入口 URL（https://…）", "", 0);
        col.addView(edSrc);

        LinearLayout r1 = row();
        edMatch = field("匹配域名（可空，支持 * 通配）", "", 0);
        edPort = field("端口", "", InputType.TYPE_CLASS_NUMBER);
        r1.addView(edMatch, weightLp(3));
        r1.addView(edPort, weightLp(1));
        col.addView(r1);

        LinearLayout r2 = row();
        btAdd = button("添加", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                onAdd();
            }
        });
        r2.addView(btAdd, weightLp(1));
        r2.addView(button("清空", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                sel = -1;
                edSrc.setText("");
                edMatch.setText("");
                edPort.setText("");
                btAdd.setText("添加");
                rebuildList();
            }
        }), weightLp(1));
        r2.addView(button("删除", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                onDelete();
            }
        }), weightLp(1));
        col.addView(r2);

        /* ---- 规则列表 ---- */
        listBox = new LinearLayout(this);
        listBox.setOrientation(LinearLayout.VERTICAL);
        listBox.setPadding(dp(2), dp(6), dp(2), dp(6));
        col.addView(listBox);

        LinearLayout r3 = row();
        r3.addView(button("启用/停用", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                onToggle();
            }
        }), weightLp(1));
        r3.addView(button("全部启动", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startAll();
            }
        }), weightLp(1));
        r3.addView(button("全部停止", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                stopAll();
            }
        }), weightLp(1));
        col.addView(r3);

        col.addView(title("点一条规则可编辑，长按可启用/停用。\n匹配域名留空的规则 = 仅端口转发：添加或点「全部启动」后会监听 127.0.0.1:<端口>，客户端直接连这个端口即可（无需代理）。", 11, 0xff888888));

        /* ---- 代理设置 ---- */
        col.addView(title("本地代理（HTTP CONNECT / SOCKS5，可不开）", 14, 0xff1565c0));
        LinearLayout r4 = row();
        edProxy = field("代理端口（0=关闭）", "", InputType.TYPE_CLASS_NUMBER);
        edBind = field("监听地址", "127.0.0.1", 0);
        r4.addView(edProxy, weightLp(1));
        r4.addView(edBind, weightLp(2));
        col.addView(r4);

        edUpstream = field("上游代理 host:port（可空）", "", 0);
        col.addView(edUpstream);

        ckBlock = new CheckBox(this);
        ckBlock.setText("上游不可用时拒绝直连");
        ckBlock.setTextSize(13);
        col.addView(ckBlock);

        ckAuto = new CheckBox(this);
        ckAuto.setText("开机自动运行");
        ckAuto.setTextSize(13);
        col.addView(ckAuto);

        LinearLayout r5 = row();
        r5.addView(button("保存并应用代理设置", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                onSaveProxy();
            }
        }), weightLp(2));
        r5.addView(button("复制 PAC 地址", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                copyPac();
            }
        }), weightLp(1));
        col.addView(r5);

        LinearLayout r5a = row();
        btVpn = button("开启无感分流（VPN）", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                onVpnClick();
            }
        });
        r5a.addView(btVpn, weightLp(1));
        col.addView(r5a);

        vpnHint = title("", 11, 0xff888888);
        vpnHint.setPadding(0, 0, 0, dp(6));
        col.addView(vpnHint);

        LinearLayout r5b = row();
        r5b.addView(button("允许后台运行（关闭电池优化）", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                askIgnoreBattery();
            }
        }), weightLp(1));
        col.addView(r5b);

        backHint = title("", 11, 0xff888888);
        backHint.setPadding(0, 0, 0, dp(6));
        col.addView(backHint);

        proxyHint = title("", 11, 0xff888888);
        proxyHint.setPadding(0, 0, 0, dp(6));
        col.addView(proxyHint);

        /* ---- 日志 ---- */
        LinearLayout r6 = row();
        TextView lt = title("日志", 14, 0xff1565c0);
        r6.addView(lt, weightLp(1));
        r6.addView(button("复制日志", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                android.content.ClipboardManager cm =
                        (android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
                if (cm != null) {
                    cm.setPrimaryClip(android.content.ClipData.newPlainText("txtmux-log", Logx.text()));
                }
                Toast.makeText(MainActivity.this, "日志已复制", Toast.LENGTH_SHORT).show();
            }
        }), weightLp(1));
        r6.addView(button("清空日志", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Logx.clear();
                logView.setText("");
            }
        }), weightLp(1));
        col.addView(r6);

        logScroll = new ScrollView(this);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(220));
        logScroll.setLayoutParams(lp);
        logScroll.setBackgroundColor(0xff111418);
        logScroll.setSmoothScrollingEnabled(false);
        logScroll.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, android.view.MotionEvent e) {
                if (e.getActionMasked() == android.view.MotionEvent.ACTION_DOWN) {
                    v.getParent().requestDisallowInterceptTouchEvent(true);
                } else if (e.getActionMasked() == android.view.MotionEvent.ACTION_UP
                        || e.getActionMasked() == android.view.MotionEvent.ACTION_CANCEL) {
                    v.getParent().requestDisallowInterceptTouchEvent(false);
                }
                return false;
            }
        });
        logScroll.setOnScrollChangeListener(new View.OnScrollChangeListener() {
            @Override
            public void onScrollChange(View v, int x, int y, int ox, int oy) {
                followLog = isLogAtBottom();
            }
        });
        logView = new TextView(this);
        logView.setTextSize(10);
        logView.setTextColor(0xffd0d6dc);
        logView.setTypeface(android.graphics.Typeface.MONOSPACE);
        logView.setPadding(dp(6), dp(6), dp(6), dp(6));
        logView.setFocusable(false);
        logView.setFocusableInTouchMode(false);
        logScroll.addView(logView);
        col.addView(logScroll);

        return root;
    }

    private void updateProxyHint() {
        Cfg c = eng.cfg();
        if (c == null) return;
        String text;
        int color = 0xff888888;
        if (c.proxyPort > 0) {
            text = "PAC: http://" + c.bind + ":" + c.proxyPort + "/proxy.pac"
                    + "　代理: " + c.bind + ":" + c.proxyPort
                    + "\n安卓系统代理需手动设置：设置 → WLAN → 当前网络 → 代理 → 手动。";
        } else {
            boolean hasMatch = false;
            for (Rule r : eng.rules()) {
                if (r.match != null && !r.match.isEmpty()) {
                    hasMatch = true;
                    break;
                }
            }
            if (hasMatch) {
                color = 0xffb05000;
                text = "注意：有规则填了「匹配域名」，但代理端口为 0，"
                        + "代理分流不会生效（这些规则现在只做本地端口转发）。要用通配域名分流，先填代理端口并保存。";
            } else {
                text = "代理端口为 0 时只做本地端口转发。";
            }
        }
        if (lastHint.equals(text)) return;
        lastHint = text;
        proxyHint.setTextColor(color);
        proxyHint.setText(text);
    }

    /* ---------------- 交互 ---------------- */

    private void loadEditors() {
        Cfg c = eng.cfg();
        edProxy.setText(String.valueOf(c.proxyPort));
        edBind.setText(c.bind);
        edUpstream.setText(c.upstream);
        ckBlock.setChecked(c.upstreamBlock);
        ckAuto.setChecked(c.autostart);
        updateProxyHint();
    }

    private void onAdd() {
        String src = edSrc.getText().toString().trim();
        String match = edMatch.getText().toString().trim();
        int port = Net.parseInt(edPort.getText().toString());
        boolean wasRunning = eng.isAnythingRunning();
        String err = eng.addOrUpdateRule(sel, src, match, port);
        if (err != null) {
            Toast.makeText(this, err, Toast.LENGTH_SHORT).show();
            return;
        }
        Toast.makeText(this, sel >= 0 ? "已修改" : "已添加", Toast.LENGTH_SHORT).show();
        if (wasRunning) {
            MuxService.start(this);
            eng.startAll();
        }
        sel = -1;
        btAdd.setText("添加");
        edSrc.setText("");
        edMatch.setText("");
        edPort.setText("");
        listSig = "";
        rebuildList();
        statusView.setText(eng.statusText());
    }

    private void onDelete() {
        if (sel < 0) {
            Toast.makeText(this, "先点一条规则选中它", Toast.LENGTH_SHORT).show();
            return;
        }
        eng.removeRule(sel);
        sel = -1;
        btAdd.setText("添加");
        edSrc.setText("");
        edMatch.setText("");
        edPort.setText("");
        listSig = "";
        rebuildList();
        statusView.setText(eng.statusText());
    }

    private void onToggle() {
        if (sel < 0) {
            Toast.makeText(this, "先点一条规则选中它", Toast.LENGTH_SHORT).show();
            return;
        }
        toggleAt(sel);
    }

    private void toggleAt(int index) {
        List<Rule> rs = eng.rules();
        if (index < 0 || index >= rs.size()) return;
        Rule r = rs.get(index);
        boolean enable = !r.enabled;
        eng.setEnabled(index, enable);
        if (enable && eng.isAnythingRunning()) {
            eng.startRule(r);
        }
        Toast.makeText(this, (enable ? "已启用 " : "已停用 ") + r.name(), Toast.LENGTH_SHORT).show();
        listSig = "";
        rebuildList();
    }

    private void startAll() {
        MuxService.start(this);
        eng.startAll();
        statusView.setText(eng.statusText());
        Toast.makeText(this, "已请求启动", Toast.LENGTH_SHORT).show();
    }

    private void stopAll() {
        eng.stopAll();
        MuxService.stop(this);
        statusView.setText(eng.statusText());
        Toast.makeText(this, "已全部停止", Toast.LENGTH_SHORT).show();
    }

    private void onSaveProxy() {
        int port = Net.parseInt(edProxy.getText().toString());
        if (port < 0 || port > 65535) {
            Toast.makeText(this, "代理端口不合法", Toast.LENGTH_SHORT).show();
            return;
        }
        String bind = edBind.getText().toString().trim();
        if (bind.isEmpty()) bind = "127.0.0.1";
        String up = edUpstream.getText().toString().trim();
        if (!up.isEmpty()) {
            String probe = up;
            if (!probe.contains(":")) {
                Toast.makeText(this, "上游要写 host:port", Toast.LENGTH_SHORT).show();
                return;
            }
        }
        eng.saveProxy(bind, port, up, ckBlock.isChecked(), ckAuto.isChecked());
        Toast.makeText(this, "代理设置已保存", Toast.LENGTH_SHORT).show();
        loadEditors();
        statusView.setText(eng.statusText());
    }

    /** 打开软件时按每条规则的启用状态自动应用；有匹配域名规则则自动开无感分流 */
    private void applyStartupState() {
        boolean anyEnabled = false;
        boolean anyMatch = false;
        for (Rule r : eng.rules()) {
            if (!r.enabled) continue;
            anyEnabled = true;
            if (r.match != null && !r.match.isEmpty()) anyMatch = true;
        }
        if (!anyEnabled) return;
        MuxService.start(this);
        eng.startAll();
        Logx.line("按规则启用状态自动启动：%d 条规则", eng.rules().size());
        if (anyMatch && !VpnBridge.running) {
            VpnBridge.setWanted(this, true);
            Intent prep = VpnService.prepare(this);
            if (prep == null) startVpnNow();
            else startActivityForResult(prep, REQ_VPN);
        }
    }

    private void onVpnClick() {
        if (VpnBridge.running) {
            VpnBridge.setWanted(this, false);
            try {
                startService(new Intent(this, VpnBridge.class).setAction(VpnBridge.ACTION_STOP));
            } catch (Throwable ignored) {
            }
            Toast.makeText(this, "正在关闭无感分流", Toast.LENGTH_SHORT).show();
            ui.postDelayed(refresh, 600);
            return;
        }
        boolean hasMatch = false;
        for (Rule r : eng.rules()) {
            if (r.match != null && !r.match.isEmpty()) {
                hasMatch = true;
                break;
            }
        }
        if (!hasMatch) {
            Toast.makeText(this, "先给规则填「匹配域名」，否则没有东西可分流", Toast.LENGTH_LONG).show();
            return;
        }
        Intent prep = VpnService.prepare(this);
        if (prep != null) {
            startActivityForResult(prep, REQ_VPN);
        } else {
            startVpnNow();
        }
    }

    private void startVpnNow() {
        try {
            Intent i = new Intent(this, VpnBridge.class);
            if (Build.VERSION.SDK_INT >= 26) startForegroundService(i);
            else startService(i);
            Toast.makeText(this, "无感分流已请求开启", Toast.LENGTH_SHORT).show();
        } catch (Throwable t) {
            Toast.makeText(this, "开启失败：" + t.getMessage(), Toast.LENGTH_LONG).show();
        }
        ui.postDelayed(refresh, 800);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req == REQ_VPN) {
            if (res == RESULT_OK) startVpnNow();
            else Toast.makeText(this, "没有授权 VPN，无感分流不可用", Toast.LENGTH_LONG).show();
        }
    }

    private void updateVpnHint() {
        if (btVpn == null) return;
        boolean on = VpnBridge.running;
        btVpn.setText(on ? "关闭无感分流（VPN）" : "开启无感分流（VPN）");
        StringBuilder sb = new StringBuilder();
        sb.append(on ? "无感分流：已开启（VPN 只接管 198.18.0.0/15，其余流量不走它）"
                    : "无感分流：未开启。开启后不用设系统代理，命中「匹配域名」的访问会自动改写端口。");
        for (Rule r : eng.rules()) {
            if (r.match != null && !r.match.isEmpty()) {
                sb.append("\n分流域名：").append(r.match);
            }
        }
        String text = sb.toString();
        if (text.equals(lastVpnHint)) return;
        lastVpnHint = text;
        vpnHint.setTextColor(on ? 0xff2e7d32 : 0xff888888);
        vpnHint.setText(text);
    }

    private String lastVpnHint = "";

    private boolean ignoringBattery() {
        try {
            android.os.PowerManager pm = (android.os.PowerManager) getSystemService(POWER_SERVICE);
            return pm != null && pm.isIgnoringBatteryOptimizations(getPackageName());
        } catch (Throwable t) {
            return false;
        }
    }

    private void updateBackHint() {
        boolean ok = ignoringBattery();
        backHint.setText(ok
                ? "后台限制：已放行（系统不会冻结本应用）"
                : "后台限制：未放行。HyperOS/MIUI 可能冻结进程导致代理失效，建议点上一行放行；"
                  + "另可在系统设置里把本应用的「省电策略」设为「无限制」。");
        backHint.setTextColor(ok ? 0xff2e7d32 : 0xffb05000);
    }

    private void askIgnoreBattery() {
        if (ignoringBattery()) {
            Toast.makeText(this, "已经放行了，不用再设置", Toast.LENGTH_SHORT).show();
            updateBackHint();
            return;
        }
        try {
            Intent i = new Intent(android.provider.Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS);
            i.setData(android.net.Uri.parse("package:" + getPackageName()));
            startActivity(i);
        } catch (Throwable t) {
            try {
                startActivity(new Intent(android.provider.Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS));
            } catch (Throwable t2) {
                Toast.makeText(this, "无法打开系统设置，请手动把本应用设为「无限制」", Toast.LENGTH_LONG).show();
            }
        }
    }

    private void copyPac() {
        Cfg c = eng.cfg();
        if (c == null || c.proxyPort <= 0) {
            Toast.makeText(this, "代理端口为 0，没有 PAC", Toast.LENGTH_SHORT).show();
            return;
        }
        String url = "http://" + c.bind + ":" + c.proxyPort + "/proxy.pac";
        android.content.ClipboardManager cm =
                (android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        if (cm != null) cm.setPrimaryClip(android.content.ClipData.newPlainText("pac", url));
        Toast.makeText(this, "已复制 " + url, Toast.LENGTH_SHORT).show();
    }

    private void askNotifyPermission() {
        if (Build.VERSION.SDK_INT < 33) return;
        try {
            if (checkSelfPermission("android.permission.POST_NOTIFICATIONS")
                    != android.content.pm.PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{"android.permission.POST_NOTIFICATIONS"}, 1);
            }
        } catch (Throwable ignored) {
        }
    }

    /* ---------------- 列表 ---------------- */

    private void rebuildList() {
        List<Rule> rs = eng.rules();
        StringBuilder sig = new StringBuilder();
        for (int i = 0; i < rs.size(); i++) {
            Rule r = rs.get(i);
            sig.append(i).append('|').append(r.src).append('|').append(r.match).append('|')
                    .append(r.port).append('|').append(r.enabled).append('|').append(r.running)
                    .append('|').append(r.resolving).append('|')
                    .append(r.hasTarget ? r.targetHost + ":" + r.targetPort : "-")
                    .append('|').append(r.lastError).append('\n');
        }
        String s = sig.toString();
        if (s.equals(listSig)) return;
        listSig = s;

        listBox.removeAllViews();
        if (rs.isEmpty()) {
            TextView t = title("还没有规则。上面填来源和端口，点「添加」。", 12, 0xff999999);
            t.setPadding(dp(6), dp(8), dp(6), dp(8));
            listBox.addView(t);
            return;
        }
        for (int i = 0; i < rs.size(); i++) {
            final int idx = i;
            final Rule r = rs.get(i);
            LinearLayout item = new LinearLayout(this);
            item.setOrientation(LinearLayout.VERTICAL);
            item.setPadding(dp(8), dp(6), dp(8), dp(6));
            if (idx == sel) item.setBackgroundColor(0x2200a0ff);

            TextView l1 = new TextView(this);
            l1.setTextSize(13);
            l1.setTextColor(0xff111111);
            l1.setText((i + 1) + ". " + r.src + "  :" + r.port
                    + (r.match.isEmpty() ? "" : "　匹配 " + r.match));

            TextView l2 = new TextView(this);
            l2.setTextSize(12);
            boolean err2 = r.lastError != null && !r.lastError.isEmpty();
            int color;
            if (r.running && r.hasTarget && !err2) color = 0xff2e7d32;
            else if (r.running && err2) color = 0xffb05000;
            else if (r.running) color = 0xff1565c0;
            else if (!r.enabled) color = 0xffb05000;
            else color = 0xff777777;
            l2.setTextColor(color);
            l2.setText(r.stateText());

            item.addView(l1);
            item.addView(l2);
            item.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    sel = idx;
                    edSrc.setText(r.src);
                    edMatch.setText(r.match);
                    edPort.setText(String.valueOf(r.port));
                    btAdd.setText("保存修改");
                    listSig = "";
                    rebuildList();
                }
            });
            item.setOnLongClickListener(new View.OnLongClickListener() {
                @Override
                public boolean onLongClick(View v) {
                    toggleAt(idx);
                    return true;
                }
            });
            listBox.addView(item);
        }
    }

    private boolean isLogAtBottom() {
        if (logScroll.getChildCount() == 0) return true;
        View child = logScroll.getChildAt(0);
        int diff = child.getBottom() - (logScroll.getHeight() + logScroll.getScrollY());
        return diff <= dp(8);
    }

    private static String tailLines(String text, int keep) {
        String[] ls = text.split("\n");
        int from = Math.max(0, ls.length - keep);
        StringBuilder sb = new StringBuilder();
        for (int i = from; i < ls.length; i++) sb.append(ls[i]).append('\n');
        return sb.toString();
    }

    private void appendLogLine(String line) {
        if (logView.length() > 30000) {
            logView.setText(tailLines(Logx.text(), 300));
        } else {
            logView.append(line + "\n");
        }
        if (followLog) {
            logView.post(new Runnable() {
                @Override
                public void run() {
                    logScroll.scrollTo(0, logView.getMeasuredHeight());
                }
            });
        }
    }
}
