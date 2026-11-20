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
import android.os.Process;
import android.system.OsConstants;
import android.util.Log;

import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.ByteArrayOutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;

import org.openipc.camex.tunnel.CamexTunnel.State;

/**
 * camex runs in-process (libcamex.so, camex_main) on a worker thread of its own; the TUN
 * device comes from {@link #establishTun} once the server's CONFIG gives the address, MTU and
 * routes. The native code calls establishTun and VpnService.protect() by name.
 *
 * <p>camex reconnects to its server by itself while it runs (with its own back-off); when it
 * exits, the worker starts it again after a back-off of 1 s doubling to 60 s, cut short when a
 * network comes up. Only {@link CamexTunnel#stop}, a revoked consent, another app's VPN or the
 * system ends it; another app's VPN leaves the tunnel WAITING ({@link OtherVpn}).
 */
public final class CamexVpnService extends VpnService {
    static final String ACTION_START = "org.openipc.camex.tunnel.START";
    static final String EXTRA_ARGV = "argv";
    static final String EXTRA_TOKEN = "token";
    private static final String TAG = "camex";
    private static final String CHANNEL = "camex_tunnel";
    private static final long BACKOFF_MIN_MS = 1000;
    private static final long BACKOFF_MAX_MS = 60000;
    // How often the server session is looked at while the tunnel is up
    private static final long SESSION_POLL_MS = 2000;

    private static native boolean nativeAcquire();

    private native int nativeRun(String[] argv);

    private static native void nativeRelease();

    private static native void nativeStop();

    private static native boolean nativeEstablished();

    static {
        System.loadLibrary("camex");
    }

    private static volatile CamexVpnService instance;
    // This service's VPN is up (it holds the TUN): up to Android 9 the only way to tell it
    // from another app's
    private static volatile boolean vpnUp;

    /** Whether the tunnel's own VPN is up now. */
    static boolean vpnUp() {
        return vpnUp;
    }

    // The TUN of the VPN, held from its establish() to a stop, a revoke or the next TUN:
    // camex gets a copy, so the VPN stays while camex reconnects or starts again, and the app
    // that holds it does not lose it to an app waiting for it (guarded by lock)
    private ParcelFileDescriptor heldTun;

    private void holdTun(ParcelFileDescriptor pfd) {
        ParcelFileDescriptor old = heldTun;
        heldTun = pfd;
        vpnUp = pfd != null;
        if (old != null) {
            try {
                old.close();
            } catch (IOException ignored) {
                // closed anyway
            }
        }
    }

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
    private String tunAddress = "";     // the address of this run's TUN, "" before it is up
    private long runGeneration = -1;    // the start whose values camex runs with now
    private int lastStartId;            // the latest start command delivered
    private long runToken;              // CamexTunnel's token of the values camex runs with
    private boolean ownNotification;    // the notification shown is the app's own
    private Thread worker;

    private ConnectivityManager.NetworkCallback networkCallback;
    private boolean foreground;
    private final android.os.Handler main = new android.os.Handler(android.os.Looper.getMainLooper());
    private final Runnable sessionPoll = this::pollSession;

    /** From CamexTunnel.stop(): stop the run and the reconnects. */
    static void stopTunnel() {
        CamexVpnService s = instance;
        if (s != null) {
            s.requestStop(false);
            // not when a newer start is queued already: it comes and runs
            s.stopSelf(s.lastStartId);
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
            // the app's own notification shows the final state before it is let go
            if (s.foreground && s.ownNotification) s.notifyState();
            s.leaveForeground();
            s.stopSelf(s.lastStartId);
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        watchNetworks();
        main.postDelayed(sessionPoll, SESSION_POLL_MS);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        instance = this;
        lastStartId = startId;
        // startForegroundService() needs startForeground() every time
        goForeground();
        String[] a = intent != null && ACTION_START.equals(intent.getAction())
                ? intent.getStringArrayExtra(EXTRA_ARGV) : null;
        if (a != null && intent.getLongExtra(EXTRA_TOKEN, -1) != CamexTunnel.token()) {
            // a start the app has stopped or replaced since: drop it
            Log.i(TAG, "a stale start is dropped");
            synchronized (lock) {
                if (!stopped) return START_NOT_STICKY;
            }
            leaveForeground();
            stopSelf(startId);   // a later start already queued still runs
            return START_NOT_STICKY;
        }
        if (a == null) {
            // a restart by the system or an always-on VPN: the library stores no values
            Log.w(TAG, "started without values; the app starts the tunnel");
            synchronized (lock) {
                if (!stopped) return START_NOT_STICKY;   // a run is under way: keep it
            }
            CamexTunnel.post(State.FAILED, "", "started without values (the app starts the tunnel)");
            stopSelf(startId);
            return START_NOT_STICKY;
        }
        synchronized (lock) {
            argv = a;
            runToken = intent.getLongExtra(EXTRA_TOKEN, -1);
            generation++;
            stopped = false;
            tunAddress = "";   // the running TUN is the old values' one
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
                    if (stopped) {
                        worker = null;   // in the same lock: a start now makes a new worker
                        return;
                    }
                    a = argv;
                    gen = generation;
                    runGeneration = gen;
                    upThisRun = false;
                    tunAddress = "";
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
                    tunAddress = "";
                    if (stopped) {
                        worker = null;
                        return;
                    }
                    if (gen != generation) {
                        backoff = BACKOFF_MIN_MS;   // new values: at once
                        continue;
                    }
                    if (consentLost) {
                        stopped = true;
                        worker = null;
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

    // No VPN consent while camex runs: it stops, the state is NEEDS_CONSENT; in one lock and
    // only for the current run and start, so a newer start keeps its own state
    private void lostConsent() {
        synchronized (lock) {
            if (stopped || runGeneration != generation || runToken != CamexTunnel.token()) {
                return;
            }
            stopped = true;
            consentLost = true;
            if (acquired) nativeStop();
            holdTun(null);
            lock.notifyAll();
            if (instance == this) CamexTunnel.post(State.NEEDS_CONSENT, "", "VPN consent needed");
        }
    }

    // Another app's VPN is up, or took the one this run made: the tunnel stops and waits for
    // it to go away (only for the current run and start, as lostConsent)
    private void yieldVpn(boolean sawOther, boolean ownJustWent, String why) {
        long token;
        synchronized (lock) {
            if (stopped || instance != this || runToken != CamexTunnel.token()) return;
            token = runToken;
        }
        // waitFor checks the token again in its own lock: a user's start() meanwhile stands
        CamexTunnel.waitFor(this, token, sawOther, ownJustWent, why);
    }

    /** Stops camex and the reconnects; consentLost: the state is NEEDS_CONSENT. */
    private void requestStop(boolean lostConsent) {
        synchronized (lock) {
            stopped = true;
            consentLost |= lostConsent;
            if (acquired) nativeStop();
            holdTun(null);   // the VPN goes
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
     * Called from native code (the camex thread) for a new TCP connection from the tunnel to
     * this app's web port (#187): whether this app owns the listener the kernel would hand it
     * to. Another app's server on the port is refused (camex answers with an RST), and so is
     * the connection when the check fails. Addresses are IPv4 in host byte order. camex keeps
     * a refusal for a moment (a flood of SYNs asks once), never an acceptance.
     */
    private boolean ownsListener(int dst, int dport, int src, int sport) {
        int me = Process.myUid();
        try {
            if (Build.VERSION.SDK_INT >= 29) {
                // The active VPN app may ask; the kernel finds the listener for a new tuple
                ConnectivityManager cm = (ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
                return cm.getConnectionOwnerUid(OsConstants.IPPROTO_TCP,
                    new InetSocketAddress(ipv4(dst), dport),
                    new InetSocketAddress(ipv4(src), sport)) == me;
            }
            // Up to Android 9 the tables are readable; one not read refuses
            return Listeners.ownedBy(dport, dst, me, read("/proc/net/tcp"), read("/proc/net/tcp6"));
        } catch (Exception e) {
            // camex logs the refusal (a port at most once in a while); the reason once a minute
            long now = android.os.SystemClock.elapsedRealtime();
            if (now - ownerErrorLogged > 60000) {
                ownerErrorLogged = now;
                Log.w(TAG, "port " + dport + ": owner unknown, refused", e);
            }
            return false;
        }
    }

    private long ownerErrorLogged = -120000;

    private static InetAddress ipv4(int a) throws IOException {
        return InetAddress.getByAddress(new byte[]{
            (byte) (a >>> 24), (byte) (a >>> 16), (byte) (a >>> 8), (byte) a});
    }

    /** A /proc file, or null when it cannot be read. */
    private static String read(String path) {
        try (InputStream in = new FileInputStream(path)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] b = new byte[8192];
            int n;
            while ((n = in.read(b)) > 0) out.write(b, 0, n);
            return out.toString("US-ASCII");
        } catch (IOException e) {
            return null;
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
            synchronized (lock) {
                // a run that was stopped or replaced makes no VPN
                if (stopped || runGeneration != generation) return -1;
                // the old TUN goes now; until this one is up there is no address to report
                tunAddress = "";
            }
            if (!Values.routeAllowed(prefixLen)) {
                // a /0 tunnel subnet would be a default route, a wide one nearly (#175)
                Log.w(TAG, "refusing the tunnel subnet /" + prefixLen);
                post(State.CONNECTING, "", "the server gave the tunnel subnet /" + prefixLen);
                return -1;
            }
            // prepare() of an app with the consent takes the VPN from the app that holds it:
            // not while another app's is up (a reconnect would take it back)
            if (OtherVpn.active(this)) {
                yieldVpn(true, false, "another app's VPN is on: the tunnel waits for it to end");
                return -1;
            }
            if (VpnService.prepare(this) != null) {
                // camex would keep asking on every handshake: stop it, and tell the app
                lostConsent();
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
                    if (!Values.routeAllowed(prefix)) {
                        // never a default route, nor one wider than a /8 (#175)
                        Log.w(TAG, "skipping route " + cidr + ": wider than /"
                                + Values.ROUTE_PREFIX_MIN);
                        continue;
                    }
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
                // not prepared any more: another app asked for the VPN a moment ago
                yieldVpn(OtherVpn.active(this), false, "another app took the VPN: the tunnel waits for it to end");
                return -1;
            }
            synchronized (lock) {
                if (stopped || runGeneration != generation) {
                    // stopped or replaced meanwhile: this VPN goes at once
                    try {
                        pfd.close();
                    } catch (java.io.IOException ignored) {
                        // closed anyway
                    }
                    return -1;
                }
                upThisRun = true;
                tunAddress = localIp;
                // camex owns a copy; this one keeps the VPN until the next TUN or a stop
                ParcelFileDescriptor copy;
                try {
                    copy = pfd.dup();
                } catch (IOException e) {
                    holdTun(pfd);
                    post(State.CONNECTING, "", "VPN setup failed: " + e.getClass().getSimpleName());
                    return -1;
                }
                holdTun(pfd);
                post(State.CONNECTED, localIp, "");
                return copy.detachFd();
            }
        } catch (RuntimeException e) {
            Log.e(TAG, "establish", e);
            post(State.CONNECTING, "", "VPN setup failed: " + e.getClass().getSimpleName());
            return -1;
        }
    }

    /**
     * While the TUN is up camex may lose its server and reconnect by itself (the TUN stays):
     * CONNECTING then, CONNECTED again once its session is back.
     */
    private void pollSession() {
        String addr;
        synchronized (lock) {
            addr = stopped ? "" : tunAddress;
        }
        if (!addr.isEmpty()) {
            boolean up = nativeEstablished();
            State st = CamexTunnel.state();
            if (st == State.CONNECTED && !up) {
                post(State.CONNECTING, "", "no answer from the server, reconnecting");
            } else if (st == State.CONNECTING && up) {
                post(State.CONNECTED, addr, "");
            }
        }
        main.postDelayed(sessionPoll, SESSION_POLL_MS);
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
        boolean own = ownNotification;
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
            if (n != null) {
                ownNotification = true;
                return n;
            }
        }
        ownNotification = false;
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
        // another VPN app took the VPN, or the user turned it off: the tunnel waits for the
        // other VPN to go away and starts again then; after a user's turn-off no VPN comes
        // and goes, and the app's START brings it back
        boolean current;
        synchronized (lock) {
            current = !stopped && instance == this && runToken == CamexTunnel.token();
        }
        synchronized (lock) {
            holdTun(null);   // the system has taken the VPN already
        }
        if (current) {
            yieldVpn(false, true, "VPN taken by another app or turned off: the tunnel waits for it to end");
        } else {
            requestStop(false);
            stopSelf(lastStartId);
        }
    }

    @Override
    public void onDestroy() {
        main.removeCallbacks(sessionPoll);
        boolean running;
        synchronized (lock) {
            running = !stopped;
        }
        requestStop(false);
        // the system ended the service with no stop(): the state says so
        if (running && instance == this) {
            CamexTunnel.post(State.STOPPED, "", "the system stopped the tunnel service");
        }
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
