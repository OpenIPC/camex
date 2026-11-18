/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * BootReceiver.java — starts the tunnel after boot when "Start on boot" is on
 */
package org.openipc.camex;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

import org.openipc.camex.tunnel.CamexTunnel;

import java.util.Map;

public class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (!Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction())) return;
        if (!Prefs.of(context).getBoolean(Prefs.START_ON_BOOT, false)) return;
        Map<String, String> v = Prefs.load(context);
        // Saved settings and the VPN consent given before: nothing to ask at boot
        if (CamexTunnel.check(v) != null || CamexTunnel.consentIntent(context) != null) {
            Log.i("camex", "start on boot: no settings or no VPN consent yet");
            return;
        }
        TunnelLog.listen();
        TunnelLog.add("start on boot");
        CamexTunnel.start(context, v);
    }
}
