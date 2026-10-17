/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * MainActivity.java — camex VPN launcher for Android
 */
package org.openipc.camex;

import android.Manifest;
import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.net.VpnService;
import android.os.Build;
import android.os.Bundle;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

public class MainActivity extends Activity {
    private TextView statusView;
    private Button startButton, stopButton, settingsButton;
    private boolean running = false;

    // camex exited (bad key, server unreachable at start...): not connected
    private final BroadcastReceiver exitReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            updateStatus(false);
            int code = intent.getIntExtra("code", 0);
            if (code != 0) {
                Toast.makeText(MainActivity.this,
                    getString(R.string.err_exited, code), Toast.LENGTH_LONG).show();
            }
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        statusView = findViewById(R.id.status);
        startButton = findViewById(R.id.btn_start);
        stopButton = findViewById(R.id.btn_stop);
        settingsButton = findViewById(R.id.btn_settings);

        startButton.setOnClickListener(v -> startVpn());
        stopButton.setOnClickListener(v -> stopVpn());
        settingsButton.setOnClickListener(v -> openSettings());

        requestNotificationPermission();
        updateStatus(false);
        // Only our own service may send ACTION_EXITED: Android 13+ honours the
        // not-exported flag, older versions the signature permission of this app
        // (what ContextCompat.registerReceiver does)
        registerReceiver(exitReceiver, new IntentFilter(TunnelService.ACTION_EXITED),
            TunnelService.PERMISSION_INTERNAL, null, RECEIVER_NOT_EXPORTED);
    }

    private void openSettings() {
        startActivity(new Intent(this, SettingsActivity.class));
    }

    @Override
    protected void onDestroy() {
        unregisterReceiver(exitReceiver);
        super.onDestroy();
    }

    // Android 13+ needs runtime POST_NOTIFICATIONS for the foreground notification.
    private void requestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                    != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{ Manifest.permission.POST_NOTIFICATIONS }, 1);
        }
    }

    private void startVpn() {
        // Protocol v2 needs both keys: without them camex cannot start
        SharedPreferences prefs = getSharedPreferences("camex", MODE_PRIVATE);
        if (!SettingsActivity.isServerPubkey(prefs.getString("server_pubkey", ""))
                || prefs.getString("psk", "").isEmpty()) {
            Toast.makeText(this, R.string.err_keys_missing, Toast.LENGTH_LONG).show();
            openSettings();
            return;
        }
        Intent intent = VpnService.prepare(this);
        if (intent != null) {
            startActivityForResult(intent, 0);
        } else {
            onActivityResult(0, RESULT_OK, null);
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result == RESULT_OK) {
            Intent svc = new Intent(this, TunnelService.class);
            startForegroundService(svc);
            updateStatus(true);
        }
    }

    private void stopVpn() {
        // stopService() is not enough while the system holds the VPN binding
        startService(new Intent(this, TunnelService.class)
            .setAction(TunnelService.ACTION_STOP));
        updateStatus(false);
    }

    private void updateStatus(boolean on) {
        running = on;
        statusView.setText(on ? R.string.status_connected : R.string.status_disconnected);
        startButton.setEnabled(!on);
        stopButton.setEnabled(on);
        // Settings only editable while disconnected.
        settingsButton.setEnabled(!on);
    }
}
