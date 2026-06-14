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
import android.app.Service;
import android.content.Intent;
import android.net.VpnService;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class TunnelService extends VpnService {
    private static final String CHANNEL_ID = "camex_vpn";
    private static final int NOTIFY_ID = 1;
    private ParcelFileDescriptor tunFd = null;
    private Thread camexThread = null;
    private volatile boolean running = false;
    private final ExecutorService executor = Executors.newSingleThreadExecutor();

    // JNI: runs camex in-process, blocks until exit
    private static native int nativeStart(int tunFd, String[] argv);

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
        startForeground(NOTIFY_ID, buildNotification("Starting..."));

        executor.execute(() -> {
            try {
                // Build VpnService TUN interface
                Builder builder = new Builder();
                builder.setSession("Camex VPN");
                builder.setMtu(1500);
                builder.addAddress("10.0.0.2", 24);
                builder.addRoute("0.0.0.0", 0);
                builder.setBlocking(true);
                tunFd = builder.establish();

                if (tunFd == null) {
                    updateNotification("Failed to establish VPN");
                    return;
                }

                // Build argv for camex
                String[] argv = {
                    "camex",
                    "--mode", "client",
                    "--auto",
                    "--name", "android-client",
                    "--server-host", getServerHost(),
                    "--port", String.valueOf(getServerPort())
                };

                // Run camex in-process via JNI (blocks)
                running = true;
                updateNotification("Running...");
                int ret = nativeStart(tunFd.getFd(), argv);
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

    private String getServerHost() {
        return getSharedPreferences("camex", MODE_PRIVATE)
            .getString("server_host", "vpn.example.org");
    }

    private int getServerPort() {
        return getSharedPreferences("camex", MODE_PRIVATE)
            .getInt("server_port", 5800);
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        cleanup();
    }

    private void cleanup() {
        // Force camex to stop by closing the TUN fd
        if (tunFd != null) {
            try { tunFd.close(); } catch (Exception ignored) {}
            tunFd = null;
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
