/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * TunnelService.java — Android VpnService wrapper that starts camex via JNI
 */
package org.openipc.camex;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.VpnService;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.util.Log;
import androidx.annotation.Keep;
import java.util.ArrayList;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class TunnelService extends VpnService {
    /** Intent action that stops the tunnel (see onStartCommand). */
    public static final String ACTION_STOP = "org.openipc.camex.STOP";
    /** Sent (to this app only) when camex has exited: the UI shows it. */
    public static final String ACTION_EXITED = "org.openipc.camex.EXITED";
    // Signature permission (AndroidManifest.xml) of the ACTION_EXITED receiver
    public static final String PERMISSION_INTERNAL = "org.openipc.camex.permission.INTERNAL";
    private static final String CHANNEL_ID = "camex_vpn";
    private static final int NOTIFY_ID = 1;
    private static final String TAG = "camex";
    private final ExecutorService executor = Executors.newSingleThreadExecutor();

    // A Stop can arrive while the start is still queued on the executor, and
    // a previous service instance may still be stopping its camex run. The
    // native run slot is process-wide: this instance holds it from
    // nativeAcquire() to nativeRelease(), and only while holding it may it
    // call nativeStop(), so a Stop never hits another instance's run and a
    // Stop before the run starts is not lost. Starts are numbered: a Stop
    // cancels every start issued before it, but not a later Start delivered
    // to the same instance before it is destroyed.
    private final Object lock = new Object();
    private long startGeneration = 0;       // guarded by lock
    private long stopGeneration = 0;        // guarded by lock
    private boolean acquired = false;       // guarded by lock

    // JNI: takes the run slot and arms camex; false if another run holds it
    private static native boolean nativeAcquire();
    // JNI: runs camex in-process, blocks until exit. tunFd < 0: camex calls
    // establishTun() once the tunnel parameters are known.
    private native int nativeRun(int tunFd, String[] argv);
    // JNI: gives the run slot back
    private static native void nativeRelease();
    // JNI: asks camex to stop; nativeRun() returns shortly after
    private static native void nativeStop();

    static {
        System.loadLibrary("camex");
    }

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            // While the VPN is up the system keeps the service bound, so
            // stopService() alone never reaches onDestroy(). Stop camex: it
            // closes the TUN fd, which tears the VPN down.
            requestStop();
            stopSelf();
            return START_NOT_STICKY;
        }

        // Android 14 (targetSdk 34) requires a foreground service type; VPN apps
        // use systemExempted, a type that exists only from Android 14; before it
        // the two-argument form takes the type from the manifest.
        Notification starting = buildNotification("Starting...");
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(NOTIFY_ID, starting,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_SYSTEM_EXEMPTED);
        } else {
            startForeground(NOTIFY_ID, starting);
        }

        final long generation;
        synchronized (lock) {
            generation = ++startGeneration;
        }
        executor.execute(() -> {
            try {
                // The VPN interface is created later, by establishTun(), when
                // camex knows the tunnel address, MTU and routes (in auto
                // mode they arrive in the server's CONFIG).

                // Build argv for camex from saved settings
                ArrayList<String> args = new ArrayList<>();
                args.add("camex");
                args.add("--mode"); args.add("client");
                args.add("--auto");
                args.add("--name"); args.add(getName());
                args.add("--server-host"); args.add(getServerHost());
                args.add("--port"); args.add(String.valueOf(getServerPort()));
                args.add("--transport"); args.add(getTransport());
                args.add("--mtu"); args.add(String.valueOf(getInt("mtu", 1500)));
                args.add("--keepalive"); args.add(String.valueOf(getInt("keepalive", 10)));
                args.add("--server-timeout");
                args.add(String.valueOf(getInt("server_timeout", 20)));
                // Protocol v2: always encrypted
                args.add("--server-pubkey"); args.add(getServerPubkey());
                args.add("--psk"); args.add(getPsk());
                String[] argv = args.toArray(new String[0]);

                if (!acquireRun(generation)) {
                    return;
                }
                try {
                    // Run camex in-process via JNI (blocks)
                    updateNotification("Running...");
                    int ret = nativeRun(-1, argv);
                    updateNotification("Exited (code " + ret + ")");
                    sendBroadcast(new Intent(ACTION_EXITED)
                        .setPackage(getPackageName())
                        .putExtra("code", ret));
                } finally {
                    synchronized (lock) {
                        acquired = false;
                        nativeRelease();
                    }
                }
            } catch (Exception e) {
                updateNotification("Error: " + e.getMessage());
            }
        });

        return START_STICKY;
    }

    /**
     * Takes the native run slot unless a Stop came after this start was
     * issued. Waits a few seconds for a previous run (an instance that is
     * still stopping).
     */
    private boolean acquireRun(long generation) throws InterruptedException {
        long deadline = SystemClock.elapsedRealtime() + 5000;
        while (true) {
            synchronized (lock) {
                if (generation <= stopGeneration) {
                    return false;
                }
                if (nativeAcquire()) {
                    acquired = true;
                    return true;
                }
            }
            if (SystemClock.elapsedRealtime() > deadline) {
                updateNotification("Previous tunnel is still running");
                return false;
            }
            Thread.sleep(200);
        }
    }

    private void requestStop() {
        synchronized (lock) {
            stopGeneration = startGeneration;
            if (acquired) {
                nativeStop();
            }
        }
    }

    /**
     * Called from native code (camex worker thread) once the tunnel address,
     * MTU and routes are known. Returns the TUN fd, owned by camex from now
     * on, or -1.
     */
    @Keep  // called only from native code: R8 must not remove or rename it
    private int establishTun(String localIp, int prefixLen, int mtu, String[] routes) {
        try {
            Builder builder = new Builder();
            builder.setSession(getString(R.string.app_name));
            builder.setMtu(mtu);
            builder.addAddress(localIp, prefixLen);
            // The tunnel subnet itself
            builder.addRoute(networkOf(localIp, prefixLen), prefixLen);
            boolean fullTunnel = getFullTunnel();
            // Routes from the server's CONFIG (or the command line). A bad
            // entry is skipped: on Linux it only fails that one route too.
            for (String cidr : routes) {
                int slash = cidr.indexOf('/');
                if (slash <= 0) {
                    continue;
                }
                try {
                    int prefix = Integer.parseInt(cidr.substring(slash + 1));
                    if (prefix == 0 && !fullTunnel) {
                        // A default route would override the split-tunnel setting
                        Log.i(TAG, "Full tunnel is off, skipping route " + cidr);
                        continue;
                    }
                    // addRoute() rejects addresses with host bits set
                    builder.addRoute(networkOf(cidr.substring(0, slash), prefix), prefix);
                } catch (IllegalArgumentException e) {
                    Log.w(TAG, "Skipping route " + cidr + ": " + e.getMessage());
                }
            }
            if (fullTunnel) {
                builder.addRoute("0.0.0.0", 0);
            }
            // Keep this app's own sockets (the connection to the camex
            // server) outside the tunnel, otherwise they loop through it.
            builder.addDisallowedApplication(getPackageName());

            ParcelFileDescriptor pfd = builder.establish();
            if (pfd == null) {
                updateNotification("Failed to establish VPN");
                return -1;
            }
            updateNotification("Connected: " + localIp + "/" + prefixLen);
            // camex owns the fd from now on and closes it on exit
            return pfd.detachFd();
        } catch (Exception e) {
            updateNotification("VPN setup error: " + e.getMessage());
            return -1;
        }
    }

    /** Network address of ip/prefixLen, e.g. 10.200.0.5/24 -> 10.200.0.0. */
    private static String networkOf(String ip, int prefixLen) {
        String[] p = ip.split("\\.");
        int addr = 0;
        for (String octet : p) {
            addr = (addr << 8) | (Integer.parseInt(octet) & 0xff);
        }
        int mask = prefixLen == 0 ? 0 : -1 << (32 - prefixLen);
        addr &= mask;
        return ((addr >>> 24) & 0xff) + "." + ((addr >>> 16) & 0xff) + "."
            + ((addr >>> 8) & 0xff) + "." + (addr & 0xff);
    }

    private boolean getFullTunnel() {
        // Off by default: only the tunnel subnet and CONFIG routes (#19)
        return getSharedPreferences("camex", MODE_PRIVATE)
            .getBoolean("full_tunnel", false);
    }

    private String getServerHost() {
        return getSharedPreferences("camex", MODE_PRIVATE)
            .getString("server_host", "vpn.example.org");
    }

    private int getServerPort() {
        return getSharedPreferences("camex", MODE_PRIVATE)
            .getInt("server_port", 5800);
    }

    private String getName() {
        return getSharedPreferences("camex", MODE_PRIVATE)
            .getString("name", "android-client");
    }

    private String getTransport() {
        // udp (default) or tcp
        return getSharedPreferences("camex", MODE_PRIVATE)
            .getString("transport", "udp");
    }

    private int getInt(String key, int def) {
        return getSharedPreferences("camex", MODE_PRIVATE).getInt(key, def);
    }

    private String getServerPubkey() {
        return getSharedPreferences("camex", MODE_PRIVATE)
            .getString("server_pubkey", "");
    }

    private String getPsk() {
        return getSharedPreferences("camex", MODE_PRIVATE)
            .getString("psk", "");
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        cleanup();
    }

    private void cleanup() {
        // camex owns the TUN fd and closes it itself; just ask it to stop.
        requestStop();
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel chan = new NotificationChannel(
                CHANNEL_ID, getString(R.string.app_name),
                NotificationManager.IMPORTANCE_LOW);
            NotificationManager mgr = getSystemService(NotificationManager.class);
            mgr.createNotificationChannel(chan);
        }
    }

    private Notification buildNotification(String text) {
        Intent intent = new Intent(this, MainActivity.class);
        PendingIntent pi = PendingIntent.getActivity(
            this, 0, intent,
            PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setContentIntent(pi)
            .setSmallIcon(android.R.drawable.ic_lock_lock)
            .build();
    }

    private void updateNotification(String text) {
        NotificationManager mgr = getSystemService(NotificationManager.class);
        mgr.notify(NOTIFY_ID, buildNotification(text));
    }
}
