package com.eta.txtmux;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/** 前台服务：只负责“活着 + 通知栏”，真正的转发在 Engine 里。 */
public class MuxService extends Service {

    public static final String CHANNEL = "txtmux_run";
    public static final int NOTIF_ID = 1001;
    public static final String ACTION_STOP = "com.eta.txtmux.action.STOP";

    static volatile boolean running = false;
    private android.os.PowerManager.WakeLock wakeLock;

    public static void start(Context c) {
        Intent i = new Intent(c, MuxService.class);
        try {
            if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i);
            else c.startService(i);
        } catch (Throwable t) {
            Logx.line("WARN 启动前台服务失败: %s", t.getMessage());
        }
    }

    public static void stop(Context c) {
        try {
            c.stopService(new Intent(c, MuxService.class));
        } catch (Throwable ignored) {
        }
    }

    /** 状态变化时刷新通知（服务没在跑就直接返回） */
    public static void update(Context c) {
        if (!running) return;
        try {
            NotificationManager nm = (NotificationManager) c.getSystemService(NOTIFICATION_SERVICE);
            if (nm != null) nm.notify(NOTIF_ID, buildNotif(c));
        } catch (Throwable ignored) {
        }
    }

    private static Notification buildNotif(Context c) {
        Intent open = new Intent(c, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        int piFlags = Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0;
        PendingIntent content = PendingIntent.getActivity(c, 0, open, piFlags);
        Intent stop = new Intent(c, MuxService.class).setAction(ACTION_STOP);
        PendingIntent stopPi = PendingIntent.getService(c, 1, stop, piFlags);

        Notification.Builder b = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(c, CHANNEL)
                : new Notification.Builder(c);
        b.setSmallIcon(android.R.drawable.stat_notify_sync)
                .setContentTitle("txtmux 运行中")
                .setContentText(Engine.get().statusText())
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setContentIntent(content);
        b.addAction(new Notification.Action.Builder(null, "全部停止", stopPi).build());
        return b.build();
    }

    @Override
    public IBinder onBind(Intent i) {
        return null;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        Engine.get().attach(this);
        try {
            android.os.PowerManager pm = (android.os.PowerManager) getSystemService(POWER_SERVICE);
            if (pm != null) {
                wakeLock = pm.newWakeLock(android.os.PowerManager.PARTIAL_WAKE_LOCK, "txtmux:run");
                wakeLock.setReferenceCounted(false);
            }
        } catch (Throwable ignored) {
        }
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm != null && Build.VERSION.SDK_INT >= 26) {
            NotificationChannel ch = new NotificationChannel(CHANNEL, "运行状态",
                    NotificationManager.IMPORTANCE_LOW);
            ch.setShowBadge(false);
            nm.createNotificationChannel(ch);
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent == null ? null : intent.getAction();
        try {
            Notification n = buildNotif(this);
            if (Build.VERSION.SDK_INT >= 34) {
                startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            } else {
                startForeground(NOTIF_ID, n);
            }
            running = true;
            if (wakeLock != null && !wakeLock.isHeld()) {
                wakeLock.acquire();
            }
        } catch (Throwable t) {
            Logx.line("WARN startForeground 失败: %s", t.getMessage());
        }

        if (ACTION_STOP.equals(action)) {
            Logx.line("收到通知栏「全部停止」");
            Engine.get().stopAll();
            running = false;
            stopSelf();
            return START_NOT_STICKY;
        }
        if (intent != null && "boot".equals(intent.getStringExtra("from"))) {
            Cfg c = Engine.get().cfg();
            if (c != null && c.autostart) {
                Logx.line("开机自动运行");
                Engine.get().startAll();
            }
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        running = false;
        if (wakeLock != null && wakeLock.isHeld()) {
            try {
                wakeLock.release();
            } catch (Throwable ignored) {
            }
        }
    }
}
