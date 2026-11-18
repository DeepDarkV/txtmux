package com.eta.txtmux;

import android.content.Context;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.List;

/** 配置：files/txtmux.json（对应 Windows 版的 txtmux.ini）。 */
public class Cfg {

    public String bind = "127.0.0.1";
    public int proxyPort = 0;              /* 0 = 不开代理 */
    public String upstream = "";           /* 上游代理 host:port */
    public boolean upstreamBlock = false;  /* 上游不可用时拒绝直连 */
    public boolean autostart = false;
    public int logMaxMB = 16;
    public final java.util.List<Rule> rules = new java.util.ArrayList<Rule>();

    public static File file(Context c) {
        return new File(c.getFilesDir(), "txtmux.json");
    }

    public static Cfg load(Context c) {
        Cfg cfg = new Cfg();
        File f = file(c);
        if (!f.isFile()) return cfg;
        try {
            String s = readAll(f);
            JSONObject o = new JSONObject(s);
            String b = o.optString("bind", "127.0.0.1").trim();
            if (!b.isEmpty()) cfg.bind = b;
            int pp = o.optInt("proxy", 0);
            cfg.proxyPort = (pp < 0 || pp > 65535) ? 0 : pp;
            cfg.upstream = o.optString("upstream", "").trim();
            cfg.upstreamBlock = o.optBoolean("upstreamBlock", false);
            cfg.autostart = o.optBoolean("autostart", false);
            cfg.logMaxMB = o.optInt("logMaxMB", 16);
            JSONArray arr = o.optJSONArray("rules");
            if (arr != null) {
                for (int i = 0; i < arr.length() && cfg.rules.size() < Engine.MAX_RULES; i++) {
                    JSONObject r = arr.optJSONObject(i);
                    if (r == null) continue;
                    Rule ru = new Rule();
                    ru.src = r.optString("src", "").trim();
                    ru.match = r.optString("match", "").trim();
                    ru.port = r.optInt("port", 0);
                    ru.enabled = r.optBoolean("enabled", true);
                    if (ru.src.isEmpty() || ru.port <= 0 || ru.port > 65535) continue;
                    cfg.rules.add(ru);
                }
            }
        } catch (Exception e) {
            Logx.line("WARN 读取配置失败: %s", e.getMessage());
        }
        return cfg;
    }

    public void save(Context c) {
        try {
            JSONObject o = new JSONObject();
            o.put("bind", bind);
            o.put("proxy", proxyPort);
            o.put("upstream", upstream);
            o.put("upstreamBlock", upstreamBlock);
            o.put("autostart", autostart);
            o.put("logMaxMB", logMaxMB);
            JSONArray arr = new JSONArray();
            List<Rule> rs = rules;
            for (int i = 0; i < rs.size(); i++) {
                Rule r = rs.get(i);
                JSONObject x = new JSONObject();
                x.put("src", r.src);
                x.put("match", r.match);
                x.put("port", r.port);
                x.put("enabled", r.enabled);
                arr.put(x);
            }
            o.put("rules", arr);
            File f = file(c);
            File tmp = new File(f.getPath() + ".tmp");
            writeAll(tmp, o.toString(2));
            if (!tmp.renameTo(f)) {
                f.delete();
                if (!tmp.renameTo(f)) {
                    Logx.line("WARN 配置保存后改名失败");
                }
            }
        } catch (Exception e) {
            Logx.line("WARN 保存配置失败: %s", e.getMessage());
        }
    }

    static String readAll(File f) throws Exception {
        FileInputStream in = new FileInputStream(f);
        try {
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) > 0) bo.write(buf, 0, n);
            return new String(bo.toByteArray(), StandardCharsets.UTF_8);
        } finally {
            try {
                in.close();
            } catch (Exception ignored) {
            }
        }
    }

    static void writeAll(File f, String s) throws Exception {
        FileOutputStream out = new FileOutputStream(f, false);
        try {
            out.write(s.getBytes(StandardCharsets.UTF_8));
            out.flush();
        } finally {
            try {
                out.close();
            } catch (Exception ignored) {
            }
        }
    }
}
