package com.eta.txtmux;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** 开机自启（仅在配置里打开「开机自动运行」时生效）。 */
public class BootReceiver extends BroadcastReceiver {

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || !Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction())) return;
        try {
            Cfg cfg = Cfg.load(context);
            if (!cfg.autostart) return;
            if (cfg.proxyPort <= 0 && cfg.rules.isEmpty()) return;
            Intent sv = new Intent(context, MuxService.class).putExtra("from", "boot");
            if (android.os.Build.VERSION.SDK_INT >= 26) context.startForegroundService(sv);
            else context.startService(sv);
        } catch (Throwable ignored) {
        }
    }
}
