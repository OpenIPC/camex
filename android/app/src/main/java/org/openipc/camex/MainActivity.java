/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * MainActivity.java — camex VPN launcher for Android, on the tunnel library (#172)
 */
package org.openipc.camex;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

import org.openipc.camex.tunnel.CamexTunnel;

import java.util.HashMap;
import java.util.Map;

public class MainActivity extends Activity {
    private TextView statusView;
    private Button startButton, stopButton, settingsButton;

    // The tunnel's state from the library (it reconnects by itself until Stop)
    private final CamexTunnel.Listener listener = this::showState;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        statusView = findViewById(R.id.status);
        startButton = findViewById(R.id.btn_start);
        stopButton = findViewById(R.id.btn_stop);
        settingsButton = findViewById(R.id.btn_settings);

        startButton.setOnClickListener(v -> startVpn());
        stopButton.setOnClickListener(v -> CamexTunnel.stop());
        settingsButton.setOnClickListener(v -> openSettings());

        CamexTunnel.setNotificationLook(getString(R.string.app_name), 0);
        requestNotificationPermission();
    }

    @Override
    protected void onStart() {
        super.onStart();
        CamexTunnel.addListener(listener);
    }

    @Override
    protected void onStop() {
        CamexTunnel.removeListener(listener);
        super.onStop();
    }

    private void openSettings() {
        startActivity(new Intent(this, SettingsActivity.class));
    }

    // Android 13+ needs runtime POST_NOTIFICATIONS for the foreground notification.
    private void requestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                    != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{ Manifest.permission.POST_NOTIFICATIONS }, 1);
        }
    }

    /** The saved settings as the library's values. */
    static Map<String, String> values(SharedPreferences p) {
        Map<String, String> v = new HashMap<>();
        v.put(CamexTunnel.SERVER, p.getString("server_host", "") + ":"
            + p.getInt("server_port", SettingsActivity.DEFAULT_PORT));
        v.put(CamexTunnel.NAME, p.getString("name", SettingsActivity.DEFAULT_NAME));
        v.put(CamexTunnel.TRANSPORT, "tcp".equals(p.getString("transport", "udp")) ? "TCP" : "UDP");
        v.put(CamexTunnel.PUBKEY, p.getString("server_pubkey", ""));
        v.put(CamexTunnel.PSK, p.getString("psk", ""));
        v.put(CamexTunnel.MTU, String.valueOf(p.getInt("mtu", SettingsActivity.DEFAULT_MTU)));
        v.put(CamexTunnel.KEEPALIVE,
            String.valueOf(p.getInt("keepalive", SettingsActivity.DEFAULT_KEEPALIVE)));
        v.put(CamexTunnel.SERVER_TIMEOUT,
            String.valueOf(p.getInt("server_timeout", SettingsActivity.DEFAULT_TIMEOUT)));
        return v;
    }

    private void startVpn() {
        // Protocol v2 needs both keys and a server: without them camex cannot start
        String[] bad = CamexTunnel.check(values(getSharedPreferences("camex", MODE_PRIVATE)));
        if (bad != null) {
            Toast.makeText(this, getString(R.string.err_settings, bad[0], bad[1]),
                Toast.LENGTH_LONG).show();
            openSettings();
            return;
        }
        Intent consent = CamexTunnel.consentIntent(this);
        if (consent != null) {
            startActivityForResult(consent, 0);
        } else {
            onActivityResult(0, RESULT_OK, null);
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result == RESULT_OK) {
            CamexTunnel.start(this, values(getSharedPreferences("camex", MODE_PRIVATE)));
        }
    }

    private void showState(CamexTunnel.State state, String address, String error) {
        boolean on = state == CamexTunnel.State.CONNECTING || state == CamexTunnel.State.CONNECTED;
        String text;
        switch (state) {
            case CONNECTED:
                text = getString(R.string.status_connected) + " · " + address;
                break;
            case CONNECTING:
                text = getString(R.string.status_connecting);
                break;
            case NEEDS_CONSENT:
                text = getString(R.string.status_needs_consent);
                break;
            case FAILED:
                text = getString(R.string.status_failed);
                break;
            default:
                text = getString(R.string.status_disconnected);
        }
        if (!error.isEmpty() && state != CamexTunnel.State.CONNECTED) text += "\n" + error;
        statusView.setText(text);
        startButton.setEnabled(!on);
        stopButton.setEnabled(on);
        // Settings only editable while disconnected.
        settingsButton.setEnabled(!on);
    }
}
