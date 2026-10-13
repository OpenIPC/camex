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
import android.text.TextUtils;
import androidx.core.app.ServiceCompat;
import java.util.ArrayList;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class TunnelService extends VpnService {
    private static final String CHANNEL_ID = "camex_vpn";
    private static final int NOTIFY_ID = 1;
    private Thread camexThread = null;
    private volatile boolean running = false;
    private final ExecutorService executor = Executors.newSingleThreadExecutor();

    // JNI: runs camex in-process, blocks until exit. tunFd < 0: camex calls
    // establishTun() once the tunnel parameters are known.
    private native int nativeStart(int tunFd, String[] argv);
    // JNI: asks camex to stop; nativeStart() returns shortly after
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
        // Android 14 (targetSdk 34) requires a foreground service type; VPN apps
        // use systemExempted. ServiceCompat picks the right overload per API.
        ServiceCompat.startForeground(this, NOTIFY_ID, buildNotification("Starting..."),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_SYSTEM_EXEMPTED);

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
                if (getEncrypt()) {
                    args.add("--encrypt");
                    String psk = getPsk();
                    if (!TextUtils.isEmpty(psk)) {
                        args.add("--psk"); args.add(psk);
                    }
                }
                String[] argv = args.toArray(new String[0]);

                // Run camex in-process via JNI (blocks)
                running = true;
                updateNotification("Running...");
                int ret = nativeStart(-1, argv);
                updateNotification("Exited (code " + ret + ")");
            } catch (Exception e) {
                updateNotification("Error: " + e.getMessage());
            } finally {
                running = false;
                cleanup();
            }
        });

        return START_STICKY;
    }

    /**
     * Called from native code (camex worker thread) once the tunnel address,
     * MTU and routes are known. Returns the TUN fd, owned by camex from now
     * on, or -1.
     */
    @SuppressWarnings("unused")
    private int establishTun(String localIp, int prefixLen, int mtu, String[] routes) {
        try {
            Builder builder = new Builder();
            builder.setSession("Camex VPN");
            builder.setMtu(mtu);
            builder.addAddress(localIp, prefixLen);
            // The tunnel subnet itself
            builder.addRoute(networkOf(localIp, prefixLen), prefixLen);
            // Routes from the server's CONFIG (or the command line)
            for (String cidr : routes) {
                int slash = cidr.indexOf('/');
                if (slash > 0) {
                    builder.addRoute(cidr.substring(0, slash),
                        Integer.parseInt(cidr.substring(slash + 1)));
                }
            }
            if (getFullTunnel()) {
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
        // Route all traffic through the tunnel (previous behaviour).
        return getSharedPreferences("camex", MODE_PRIVATE)
            .getBoolean("full_tunnel", true);
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

    private boolean getEncrypt() {
        return getSharedPreferences("camex", MODE_PRIVATE)
            .getBoolean("encrypt", false);
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
        if (running) {
            nativeStop();
        }
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel chan = new NotificationChannel(
                CHANNEL_ID, "Camex VPN",
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
            .setContentTitle("Camex VPN")
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
