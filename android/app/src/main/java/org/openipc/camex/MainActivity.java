/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * MainActivity.java — camex VPN launcher for Android
 */
package org.openipc.camex;

import android.content.Intent;
import android.net.VpnService;
import android.os.Bundle;
import android.widget.Button;
import android.widget.TextView;
import androidx.appcompat.app.AppCompatActivity;

public class MainActivity extends AppCompatActivity {
    private TextView statusView;
    private Button startButton, stopButton;
    private boolean running = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        statusView = findViewById(R.id.status);
        startButton = findViewById(R.id.btn_start);
        stopButton = findViewById(R.id.btn_stop);

        startButton.setOnClickListener(v -> startVpn());
        stopButton.setOnClickListener(v -> stopVpn());

        updateStatus(false);
    }

    private void startVpn() {
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
        stopService(new Intent(this, TunnelService.class));
        updateStatus(false);
    }

    private void updateStatus(boolean on) {
        running = on;
        statusView.setText(on ? R.string.status_connected : R.string.status_disconnected);
        startButton.setEnabled(!on);
        stopButton.setEnabled(on);
    }
}
