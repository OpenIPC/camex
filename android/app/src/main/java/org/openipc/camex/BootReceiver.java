/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * BootReceiver.java — starts the tunnel by itself: after boot when "Start on boot" is on, and
 * after an update of the app when the tunnel ran before it (MY_PACKAGE_REPLACED)
 */
package org.openipc.camex;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.util.Log;

import org.openipc.camex.tunnel.CamexTunnel;

import java.util.Map;

public class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        SharedPreferences p = Prefs.of(context);
        String why;
        if (Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction())) {
            if (!p.getBoolean(Prefs.START_ON_BOOT, false)) return;
            why = "start on boot";
        } else if (Intent.ACTION_MY_PACKAGE_REPLACED.equals(intent.getAction())) {
            // an update stops the app and its tunnel: back as it was before it
            if (!p.getBoolean(Prefs.RUNNING, false)) return;
            why = "start after the update";
        } else {
            return;
        }
        Map<String, String> v = Prefs.load(context);
        // Saved settings only: nothing to ask in the background. No consentIntent() here: with
        // the consent given it would take the VPN from another app; startWhenFree() waits for
        // that VPN instead, and without the consent the state is NEEDS_CONSENT
        if (CamexTunnel.check(v) != null) {
            Log.i("camex", why + ": no settings yet");
            return;
        }
        TunnelLog.listen();
        TunnelLog.add(why);
        p.edit().putBoolean(Prefs.RUNNING, true).apply();
        CamexTunnel.startWhenFree(context, v);
    }
}
