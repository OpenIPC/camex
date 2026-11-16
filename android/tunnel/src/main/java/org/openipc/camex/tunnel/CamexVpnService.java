/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * CamexVpnService.java — the VPN service of the camex tunnel library (#170): camex in-process,
 * reconnects until stopped, the app inside its own VPN
 */
package org.openipc.camex.tunnel;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.VpnService;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.system.OsConstants;
import android.util.Log;

import org.openipc.camex.tunnel.CamexTunnel.State;

/**
 * camex runs in-process (libcamex.so, camex_main) on a worker thread of its own; the TUN
 * device comes from {@link #establishTun} once the server's CONFIG gives the address, MTU and
 * routes. The native code calls establishTun and VpnService.protect() by name.
 *
 * <p>camex reconnects to its server by itself while it runs (with its own back-off); when it
 * exits, the worker starts it again after a back-off of 1 s doubling to 60 s, cut short when a
 * network comes up. Only {@link CamexTunnel#stop}, a revoked consent or the system ends it.
 */
public final class CamexVpnService extends VpnService {
    static final String ACTION_START = "org.openipc.camex.tunnel.START";
    static final String EXTRA_ARGV = "argv";
    private static final String TAG = "camex";
    private static final String CHANNEL = "camex_tunnel";
    private static final long BACKOFF_MIN_MS = 1000;
    private static final long BACKOFF_MAX_MS = 60000;

    private static native boolean nativeAcquire();

    private native int nativeRun(String[] argv);

    private static native void nativeRelease();

    private static native void nativeStop();

    static {
        System.loadLibrary("camex");
    }

    private static volatile CamexVpnService instance;

    // The worker's orders, guarded by lock: argv of the latest start, its number, and whether
    // the tunnel is to stop
    private final Object lock = new Object();
    private String[] argv;
    private long generation;
    private boolean stopped = true;
    private boolean acquired;           // this instance holds the native run slot
    private boolean nudged;             // a network came up: retry now
    private boolean consentLost;
    private boolean upThisRun;
    private Thread worker;

    private ConnectivityManager.NetworkCallback networkCallback;
    private boolean foreground;

    /** From CamexTunnel.stop(): stop the run and the reconnects. */
    static void stopTunnel() {
        CamexVpnService s = instance;
        if (s != null) {
            s.requestStop(false);
            s.stopSelf();
        }
    }

    /** On the main thread after every state change: the notification follows it. */
    static void onState() {
        CamexVpnService s = instance;
        if (s == null) return;
        State st = CamexTunnel.state();
        if (st == State.CONNECTING || st == State.CONNECTED) {
            if (s.foreground) s.notifyState();
        } else {
            synchronized (s.lock) {
                if (!s.stopped) return;   // a newer start runs: its state follows
            }
            s.leaveForeground();
            s.stopSelf();
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        watchNetworks();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        instance = this;
        // startForegroundService() needs startForeground() every time
        goForeground();
        String[] a = intent != null && ACTION_START.equals(intent.getAction())
                ? intent.getStringArrayExtra(EXTRA_ARGV) : null;
        if (a == null) {
            // a restart by the system or an always-on VPN: the library stores no values
            Log.w(TAG, "started without values; the app starts the tunnel");
            synchronized (lock) {
                if (!stopped) return START_NOT_STICKY;   // a run is under way: keep it
            }
            CamexTunnel.post(State.FAILED, "", "started without values (the app starts the tunnel)");
            stopSelf();
            return START_NOT_STICKY;
        }
        synchronized (lock) {
            argv = a;
            generation++;
            stopped = false;
            consentLost = false;
            // a running camex ends, and the worker starts it with the new values
            if (acquired) nativeStop();
            lock.notifyAll();
            if (worker == null) {
                worker = new Thread(this::work, "camex");
                worker.start();
            }
        }
        CamexTunnel.post(State.CONNECTING, "", "");
        return START_NOT_STICKY;
    }

    /** The worker: camex again and again until a stop. */
    private void work() {
        long backoff = BACKOFF_MIN_MS;
        try {
            while (true) {
                String[] a;
                long gen;
                synchronized (lock) {
                    if (stopped) return;
                    a = argv;
                    gen = generation;
                    upThisRun = false;
                }
                if (!acquire(gen)) continue;
                int ret;
                try {
                    ret = nativeRun(a);
                } finally {
                    synchronized (lock) {
                        acquired = false;
                        nativeRelease();
                    }
                }
                String why;
                synchronized (lock) {
                    if (stopped) return;
                    if (gen != generation) {
                        backoff = BACKOFF_MIN_MS;   // new values: at once
                        continue;
                    }
                    if (consentLost) {
                        stopped = true;
                        post(State.NEEDS_CONSENT, "", "VPN consent needed");
                        return;
                    }
                    if (upThisRun) backoff = BACKOFF_MIN_MS;
                    why = "camex exited (code " + ret + "), retrying in " + backoff / 1000 + " s";
                    post(State.CONNECTING, "", why);
                    Log.i(TAG, why);
                    nudged = false;
                    long until = System.currentTimeMillis() + backoff;
                    long left;
                    while (!stopped && gen == generation && !nudged
                            && (left = until - System.currentTimeMillis()) > 0) {
                        lock.wait(left);
                    }
                }
                backoff = Math.min(backoff * 2, BACKOFF_MAX_MS);
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } finally {
            synchronized (lock) {
                if (worker == Thread.currentThread()) worker = null;
            }
        }
    }

    /** Takes the native run slot for this start; waits while an older run still holds it. */
    private boolean acquire(long gen) throws InterruptedException {
        while (true) {
            synchronized (lock) {
                if (stopped || gen != generation) return false;
                if (nativeAcquire()) {
                    acquired = true;
                    return true;
                }
                lock.wait(200);
            }
        }
    }

    /** Stops camex and the reconnects; consentLost: the state is NEEDS_CONSENT. */
    private void requestStop(boolean lostConsent) {
        synchronized (lock) {
            stopped = true;
            consentLost |= lostConsent;
            if (acquired) nativeStop();
            lock.notifyAll();
        }
    }

    // A state from the worker, unless another instance runs the tunnel now; after a stop only
    // NEEDS_CONSENT (a stop has posted its own state already)
    private void post(State s, String addr, String error) {
        synchronized (lock) {
            if (instance != this || (stopped && s != State.NEEDS_CONSENT)) return;
            CamexTunnel.post(s, addr, error);
        }
    }

    /**
     * Called from native code (the camex thread) once the tunnel address, MTU and routes are
     * known: the TUN fd, owned by camex from now on, or -1. Only the tunnel subnet and the
     * server's routes go through it; a default route is never taken (the app needs the other
     * clients, not the whole traffic). The app is not excluded: its sockets reach the tunnel,
     * and camex's own are protected from it.
     */
    private int establishTun(String localIp, int prefixLen, int mtu, String[] routes) {
        try {
            if (VpnService.prepare(this) != null) {
                synchronized (lock) {
                    consentLost = true;
                }
                return -1;
            }
            Builder b = new Builder()
                    .setSession(String.valueOf(CamexTunnel.notificationTitle))
                    .setMtu(mtu)
                    .addAddress(localIp, prefixLen)
                    .addRoute(Values.networkOf(localIp, prefixLen), prefixLen);
            for (String cidr : routes) {
                int slash = cidr.indexOf('/');
                if (slash <= 0) continue;
                try {
                    int prefix = Integer.parseInt(cidr.substring(slash + 1));
                    if (prefix <= 0) continue;   // never a default route
                    b.addRoute(Values.networkOf(cidr.substring(0, slash), prefix), prefix);
                } catch (IllegalArgumentException e) {
                    Log.w(TAG, "skipping route " + cidr + ": " + e.getMessage());
                }
            }
            // The tunnel is IPv4 only. Without this the app's IPv6 would be blocked while the
            // VPN is up (a VPN without an IPv6 address or route gets an unreachable ::/0)
            b.allowFamily(OsConstants.AF_INET6);
            if (Build.VERSION.SDK_INT >= 29) b.setMetered(false);
            ParcelFileDescriptor pfd = b.establish();
            if (pfd == null) {
                // not prepared any more
                synchronized (lock) {
                    consentLost = true;
                }
                return -1;
            }
            synchronized (lock) {
                upThisRun = true;
            }
            post(State.CONNECTED, localIp, "");
            return pfd.detachFd();
        } catch (RuntimeException e) {
            Log.e(TAG, "establish", e);
            post(State.CONNECTING, "", "VPN setup failed: " + e.getClass().getSimpleName());
            return -1;
        }
    }

    // A network that comes up cuts the back-off short (camex itself reconnects while it runs)
    private void watchNetworks() {
        ConnectivityManager cm = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        if (cm == null) return;
        // the default request leaves VPNs out (NOT_VPN)
        NetworkRequest req = new NetworkRequest.Builder()
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .build();
        networkCallback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network network) {
                synchronized (lock) {
                    nudged = true;
                    lock.notifyAll();
                }
            }
        };
        try {
            cm.registerNetworkCallback(req, networkCallback);
        } catch (RuntimeException e) {
            Log.w(TAG, "no network callback: " + e.getMessage());
            networkCallback = null;
        }
    }

    private void goForeground() {
        Notification n = notification();
        int id = CamexTunnel.notificationId;
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(id, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(id, n);
        }
        foreground = true;
    }

    @SuppressWarnings("deprecation")   // stopForeground(boolean) before API 24
    private void leaveForeground() {
        if (!foreground) return;
        foreground = false;
        boolean own = CamexTunnel.notificationFactory != null;
        if (Build.VERSION.SDK_INT >= 24) {
            // the app's own notification (maybe its own service's) stays
            stopForeground(own ? Service.STOP_FOREGROUND_DETACH : Service.STOP_FOREGROUND_REMOVE);
        } else {
            stopForeground(!own);
        }
    }

    private void notifyState() {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        try {
            nm.notify(CamexTunnel.notificationId, notification());
        } catch (RuntimeException e) {
            Log.w(TAG, "notification: " + e.getMessage());
        }
    }

    @SuppressWarnings("deprecation")   // Notification.Builder(Context) before Android 8
    private Notification notification() {
        State s = CamexTunnel.state();
        String addr = CamexTunnel.address();
        CamexTunnel.NotificationFactory f = CamexTunnel.notificationFactory;
        if (f != null) {
            Notification n = f.create(this, s, addr, CamexTunnel.lastError());
            if (n != null) return n;
        }
        CharSequence title = CamexTunnel.notificationTitle;
        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
            nm.createNotificationChannel(new NotificationChannel(CHANNEL, title,
                    NotificationManager.IMPORTANCE_LOW));
            b = new Notification.Builder(this, CHANNEL);
        } else {
            b = new Notification.Builder(this);
        }
        // the notification opens the app (the library does not know its activities)
        Intent open = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (open == null && Build.VERSION.SDK_INT >= 21) {
            open = getPackageManager().getLeanbackLaunchIntentForPackage(getPackageName());
        }
        if (open != null) {
            b.setContentIntent(PendingIntent.getActivity(this, 0, open,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE));
        }
        String text = s == State.CONNECTED ? "Connected" + (addr.isEmpty() ? "" : " · " + addr)
                : "Connecting";
        int icon = CamexTunnel.notificationIcon;
        return b.setSmallIcon(icon != 0 ? icon : R.drawable.camex_tunnel_notify)
                .setContentTitle(title)
                .setContentText(text)
                .setOngoing(true)
                .build();
    }

    @Override
    public void onRevoke() {
        // the user turned the VPN off, or another VPN app took over: no consent any more
        requestStop(true);
        post(State.NEEDS_CONSENT, "", "VPN consent revoked or taken by another VPN");
        stopSelf();
    }

    @Override
    public void onDestroy() {
        requestStop(false);
        if (networkCallback != null) {
            try {
                ((ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE))
                        .unregisterNetworkCallback(networkCallback);
            } catch (RuntimeException ignored) {
                // already gone
            }
        }
        if (instance == this) instance = null;
        super.onDestroy();
    }
}
