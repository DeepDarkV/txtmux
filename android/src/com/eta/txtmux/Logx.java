package com.eta.txtmux;

import android.content.Context;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.CopyOnWriteArrayList;

/** 日志：内存环形副本 + 落盘（按天归档、总量上限，与 Windows 版一致）。 */
public final class Logx {

    public interface Sink {
        void onLog(String line);
    }

    private static final int MAX_CHARS = 20000;
    private static final List<Sink> sinks = new CopyOnWriteArrayList<Sink>();
    private static final StringBuilder buf = new StringBuilder();
    private static final SimpleDateFormat HMS = new SimpleDateFormat("HH:mm:ss", Locale.US);
    private static final SimpleDateFormat YMD = new SimpleDateFormat("yyyyMMdd", Locale.US);

    private static File dir;
    private static File active;
    private static String day = "";
    private static int maxMB = 16;

    private Logx() {
    }

    public static synchronized void init(Context c, int maxMb) {
        if (dir != null) {
            maxMB = maxMb;
            return;
        }
        dir = c.getFilesDir();
        active = new File(dir, "txtmux.log");
        maxMB = maxMb;
        ensureToday();
        prune();
    }

    public static synchronized void setMaxMB(int mb) {
        maxMB = mb;
    }

    public static void addSink(Sink s) {
        if (s != null && !sinks.contains(s)) sinks.add(s);
    }

    public static void removeSink(Sink s) {
        sinks.remove(s);
    }

    public static synchronized String text() {
        return buf.toString();
    }

    public static synchronized void clear() {
        buf.setLength(0);
    }

    public static File activeFile() {
        return active;
    }

    public static void line(String fmt, Object... args) {
        String msg;
        if (args == null || args.length == 0) {
            msg = fmt;
        } else {
            try {
                msg = String.format(Locale.US, fmt, args);
            } catch (Exception e) {
                msg = fmt;
            }
        }
        String stamped = HMS.format(new Date()) + " " + msg;
        synchronized (Logx.class) {
            buf.append(stamped).append('\n');
            if (buf.length() > MAX_CHARS + 2000) {
                buf.delete(0, buf.length() - MAX_CHARS);
            }
        }
        writeFile(stamped);
        for (Sink s : sinks) {
            try {
                s.onLog(stamped);
            } catch (Exception ignored) {
            }
        }
    }

    private static synchronized void writeFile(String line) {
        if (active == null) return;
        try {
            ensureToday();
            FileOutputStream out = new FileOutputStream(active, true);
            try {
                out.write((line + "\r\n").getBytes(StandardCharsets.UTF_8));
                out.flush();
            } finally {
                try {
                    out.close();
                } catch (Exception ignored) {
                }
            }
        } catch (Exception ignored) {
        }
    }

    private static void ensureToday() {
        String today = YMD.format(new Date());
        if (day.isEmpty()) {
            /* 上次退出留下的旧日志：跨天就先归档 */
            if (active != null && active.isFile()) {
                long t = active.lastModified();
                String fd = YMD.format(new Date(t));
                if (!fd.equals(today)) rotate(fd);
            }
            day = today;
            return;
        }
        if (today.equals(day)) return;
        rotate(day);
        day = today;
        prune();
    }

    private static void rotate(String date) {
        if (active == null || !active.isFile()) return;
        for (int n = 0; n < 1000; n++) {
            String name = (n == 0) ? ("txtmux-" + date + ".log") : ("txtmux-" + date + "-" + n + ".log");
            File target = new File(dir, name);
            if (!target.exists()) {
                if (!active.renameTo(target)) {
                    /* 改名失败：直接清空旧内容，避免继续混写 */
                    active.delete();
                }
                return;
            }
        }
    }

    private static void prune() {
        if (dir == null || maxMB <= 0) return;
        long limit = (long) maxMB * 1024L * 1024L;
        File[] all = dir.listFiles();
        if (all == null) return;
        List<File> olds = new ArrayList<File>();
        long total = (active != null && active.isFile()) ? active.length() : 0;
        for (File f : all) {
            String n = f.getName();
            if (n.startsWith("txtmux-") && n.endsWith(".log") && f.isFile()) {
                olds.add(f);
                total += f.length();
            }
        }
        /* 文件名按 YYYYMMDD 排序即按日期，最旧的先删 */
        Collections.sort(olds, new java.util.Comparator<File>() {
            @Override
            public int compare(File a, File b) {
                return a.getName().compareTo(b.getName());
            }
        });
        for (File f : olds) {
            if (total <= limit) break;
            long len = f.length();
            if (f.delete()) total -= len;
        }
    }
}
