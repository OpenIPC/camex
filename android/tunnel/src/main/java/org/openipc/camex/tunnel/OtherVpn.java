/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * OtherVpn.java — another app's VPN on the device: the tunnel yields to it and comes back
 * when it goes away
 */
package org.openipc.camex.tunnel;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.util.Log;

import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * Android holds one VPN at a time, and VpnService.prepare() of an app that has the consent
 * already takes the VPN from the app that holds it, without a word. So the tunnel calls it
 * only when no other app's VPN is up, or when the user asks ({@link CamexTunnel#start}).
 *
 * <p>While it waits ({@link CamexTunnel.State#WAITING}) a network callback watches the VPNs:
 * once the other VPN it saw goes away, the tunnel starts again with the values it waited with.
 * A VPN the user turned off in the system settings, with no other VPN after it, is not
 * followed by a start: then the app's START brings the tunnel back.
 */
final class OtherVpn {
    private static final String TAG = "camex";
    // Up to Android 9 the VPN's owner is not known: after a revoke the tunnel's own VPN may
    // still be on the list for a moment, so the watch starts after it
    private static final long OWN_VPN_GONE_MS = 3500;
    // While the callback has not told of a VPN on the list (gone before the watch, or not told
    // at all), the watch looks at the list again this often
    private static final long RECHECK_MS = 2000;
    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    // The watch, on the main thread only
    private static ConnectivityManager.NetworkCallback callback;
    private static ConnectivityManager watched;
    private static final Set<Network> seen = new HashSet<>();
    private static long watchToken;
    private static Runnable pending;

    private OtherVpn() { }

    /**
     * Whether a VPN of another app is up now (the tunnel's own is not counted). A list that
     * cannot be read counts as one: the tunnel waits rather than take a VPN it cannot see.
     */
    @SuppressWarnings("deprecation")   // getAllNetworks(): the one call from API 21 to 36
    static boolean active(Context context) {
        ConnectivityManager cm = (ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (cm == null) return false;
        try {
            for (Network n : cm.getAllNetworks()) {
                if (foreign(cm.getNetworkCapabilities(n))) return true;
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "the VPNs cannot be read: " + e.getMessage());
            return true;
        }
        return false;
    }

    /** A VPN network that is not the tunnel's own. */
    private static boolean foreign(NetworkCapabilities caps) {
        if (caps == null || !caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN)) return false;
        // Android 10+ gives the owner's uid to the owner only: another app's VPN shows none
        if (Build.VERSION.SDK_INT >= 29) return caps.getOwnerUid() != Process.myUid();
        return !CamexVpnService.vpnUp();
    }

    /**
     * Waits for the other VPN to go away, then starts the tunnel with these values, unless a
     * start or a stop came meanwhile (the token moved on). Main thread or not.
     *
     * @param sawOther another VPN was seen up: if it is gone before the watch sees it, the
     *                 tunnel starts all the same; false after a revoke, where the user may
     *                 have turned the tunnel's own VPN off
     * @param ownJustWent the tunnel's own VPN went a moment ago (a revoke)
     */
    static void watch(Context context, Map<String, String> values, long token, boolean sawOther,
                      boolean ownJustWent) {
        final Context app = context.getApplicationContext();
        Runnable arm = () -> arm(app, values, token, sawOther);
        MAIN.post(() -> {
            // an older wait posted late must not end a newer one's watch
            if (CamexTunnel.token() != token) return;
            unwatch();
            if (ownJustWent && Build.VERSION.SDK_INT < 29) {
                pending = arm;
                MAIN.postDelayed(arm, OWN_VPN_GONE_MS);
            } else {
                arm.run();
            }
        });
    }

    /** Ends the watch (a start or a stop). Main thread or not. */
    static void cancel() {
        if (Looper.myLooper() == Looper.getMainLooper()) unwatch();
        else MAIN.post(OtherVpn::unwatch);
    }

    private static void arm(Context app, Map<String, String> values, long token, boolean sawOther) {
        pending = null;
        if (CamexTunnel.token() != token) return;   // started or stopped meanwhile
        ConnectivityManager cm = (ConnectivityManager) app.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (cm == null) return;
        NetworkRequest req = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_VPN)
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN)
                .build();
        watchToken = token;
        seen.clear();
        // The VPNs up now: before Android 8 the callback tells of them by onAvailable only
        seed(cm);
        callback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network network) {
                MAIN.post(() -> {
                    if (callback != this) return;
                    try {
                        if (foreign(cm.getNetworkCapabilities(network))) seen.add(network);
                    } catch (RuntimeException ignored) {
                        // gone meanwhile
                    }
                });
            }

            @Override
            public void onCapabilitiesChanged(Network network, NetworkCapabilities caps) {
                MAIN.post(() -> {
                    if (callback == this && foreign(caps)) seen.add(network);
                });
            }

            @Override
            public void onLost(Network network) {
                MAIN.post(() -> {
                    if (callback != this || !seen.remove(network) || !seen.isEmpty()) return;
                    unwatch();
                    if (CamexTunnel.token() != watchToken
                            || CamexTunnel.state() != CamexTunnel.State.WAITING) return;
                    Log.i(TAG, "the other VPN went away: the tunnel starts again");
                    CamexTunnel.startWhenFree(app, values);
                });
            }
        };
        try {
            cm.registerNetworkCallback(req, callback);
            watched = cm;
        } catch (RuntimeException e) {
            // no watch: the app's START brings the tunnel back
            Log.w(TAG, "no VPN watch: " + e.getMessage());
            callback = null;
            return;
        }
        if (sawOther) recheck(app, cm, values, token, callback);
    }

    /**
     * A VPN seen up that the callback has not told of: gone before the watch began (the
     * tunnel starts), or still on the list (the watch looks again later).
     */
    private static void recheck(Context app, ConnectivityManager cm, Map<String, String> values,
                                long token, ConnectivityManager.NetworkCallback cb) {
        pending = new Runnable() {
            @Override
            public void run() {
                pending = null;
                if (callback != cb) return;
                // a VPN gone between the look at the list and the callback has no onLost
                for (java.util.Iterator<Network> it = seen.iterator(); it.hasNext(); ) {
                    Network n = it.next();
                    NetworkCapabilities caps;
                    try {
                        caps = cm.getNetworkCapabilities(n);
                    } catch (RuntimeException e) {
                        caps = null;
                    }
                    if (caps == null) it.remove();
                }
                if (!seen.isEmpty()) return;   // the callback follows it
                if (active(app)) {
                    pending = this;
                    MAIN.postDelayed(this, RECHECK_MS);
                    return;
                }
                unwatch();
                if (CamexTunnel.token() != token
                        || CamexTunnel.state() != CamexTunnel.State.WAITING) return;
                Log.i(TAG, "the other VPN is gone: the tunnel starts again");
                CamexTunnel.startWhenFree(app, values);
            }
        };
        MAIN.postDelayed(pending, RECHECK_MS);
    }

    @SuppressWarnings("deprecation")   // getAllNetworks(): the one call from API 21 to 36
    private static void seed(ConnectivityManager cm) {
        try {
            for (Network n : cm.getAllNetworks()) {
                if (foreign(cm.getNetworkCapabilities(n))) seen.add(n);
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "the VPNs cannot be read: " + e.getMessage());
        }
    }

    private static void unwatch() {
        if (pending != null) {
            MAIN.removeCallbacks(pending);
            pending = null;
        }
        if (callback != null && watched != null) {
            try {
                watched.unregisterNetworkCallback(callback);
            } catch (RuntimeException ignored) {
                // gone already
            }
        }
        callback = null;
        watched = null;
        seen.clear();
    }
}
