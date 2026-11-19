/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * MainActivity.java — camex VPN launcher for Android, on the tunnel library (#172), in brass
 * (brand brass 1.3.0, the interface review of 2026-11-18): the heading with the version, the
 * board TUNNEL / SERVER / CLIENT / PROTO with its lamp, START, STOP and SETTINGS in a row, the
 * check boxes Start on boot and Show log, the tunnel's log under the panel
 */
package org.openipc.camex;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.openipc.camex.tunnel.CamexTunnel;

import java.util.Map;

public class MainActivity extends Activity {
    private Brass.Row rowTunnel, rowServer, rowClient, rowProto;
    private Button startButton, stopButton, settingsButton;
    private Brass.LogWindow logWindow;
    private static final int LOG_LINES = 12;

    // The tunnel's state from the library (it reconnects by itself until Stop)
    private final CamexTunnel.Listener listener = this::showState;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TunnelLog.listen();
        int pad = Brass.dp(this, 16);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(pad, pad, pad, pad);

        String version;
        try {
            version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (PackageManager.NameNotFoundException e) {
            version = CamexTunnel.VERSION;
        }
        page.addView(Brass.titleRow(this, getString(R.string.app_name), version));
        TextView sub = new TextView(this);
        sub.setText(R.string.subtitle);
        sub.setTextColor(Brass.color(this, R.color.dim));
        sub.setGravity(Gravity.CENTER_HORIZONTAL);
        LinearLayout.LayoutParams sp = new LinearLayout.LayoutParams(-1, -2);
        sp.setMargins(0, 0, 0, pad);
        page.addView(sub, sp);

        LinearLayout panel = Brass.panel(this);
        page.addView(panel);
        LinearLayout board = new LinearLayout(this);
        board.setOrientation(LinearLayout.VERTICAL);
        board.setBackgroundResource(R.drawable.display);
        int bp = Brass.dp(this, 12);
        board.setPadding(bp, bp - Brass.dp(this, 3), bp, bp - Brass.dp(this, 3));
        rowTunnel = new Brass.Row(this, "TUNNEL");
        rowServer = new Brass.Row(this, "SERVER");
        rowClient = new Brass.Row(this, "CLIENT");
        rowProto = new Brass.Row(this, "PROTO");
        for (Brass.Row r : new Brass.Row[]{rowTunnel, rowServer, rowClient, rowProto})
            board.addView(r.view);
        LinearLayout.LayoutParams bl = new LinearLayout.LayoutParams(-1, -2);
        bl.setMargins(Brass.dp(this, 6), 0, Brass.dp(this, 6), Brass.dp(this, 6));
        panel.addView(board, bl);

        // three single buttons: one row (the approved mock-up)
        LinearLayout buttons = new LinearLayout(this);
        panel.addView(buttons);
        startButton = Brass.button(this, R.string.btn_start);
        stopButton = Brass.button(this, R.string.btn_stop);
        settingsButton = Brass.button(this, R.string.btn_settings);
        for (Button b : new Button[]{startButton, stopButton, settingsButton})
            buttons.addView(b, Brass.margins(this, 0, -2, 1));

        // under the buttons, in a row: Start on boot, Show log (both off by default)
        SharedPreferences prefs = Prefs.of(this);
        LinearLayout checks = new LinearLayout(this);
        CheckBox boot = new CheckBox(this);
        boot.setText(R.string.start_on_boot);
        boot.setChecked(prefs.getBoolean(Prefs.START_ON_BOOT, false));
        boot.setOnCheckedChangeListener((b, on) -> prefs.edit().putBoolean(Prefs.START_ON_BOOT, on).apply());
        checks.addView(boot, new LinearLayout.LayoutParams(-2, -2));
        checks.addView(new View(this), new LinearLayout.LayoutParams(0, 1, 1));
        CheckBox showLog = new CheckBox(this);
        showLog.setText(R.string.show_log);
        showLog.setChecked(prefs.getBoolean(Prefs.SHOW_LOG, false));
        checks.addView(showLog, new LinearLayout.LayoutParams(-2, -2));
        checks.addView(new View(this), new LinearLayout.LayoutParams(0, 1, 1));
        panel.addView(checks);

        // The Show log window (brand brass 1.3.1): oldest line at the top, 12 lines with its own
        // scroll, it follows the end unless scrolled up, D-pad focus with the focus ring
        logWindow = new Brass.LogWindow(this, LOG_LINES);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, logWindow.height);
        lp.setMargins(0, pad, 0, 0);
        page.addView(logWindow.view, lp);
        showLog.setOnCheckedChangeListener((b, on) -> {
            prefs.edit().putBoolean(Prefs.SHOW_LOG, on).apply();
            showLog(on);
        });
        showLog(showLog.isChecked());

        ScrollView scroll = new ScrollView(this);
        scroll.addView(page);
        setContentView(scroll);

        startButton.setOnClickListener(v -> startVpn());
        stopButton.setOnClickListener(v -> {
            TunnelLog.add("stop");
            Prefs.of(this).edit().putBoolean(Prefs.RUNNING, false).apply();
            CamexTunnel.stop();
        });
        settingsButton.setOnClickListener(v -> openSettings());

        CamexTunnel.setNotificationLook(getString(R.string.app_name), 0);
        requestNotificationPermission();
    }

    @Override
    protected void onStart() {
        super.onStart();
        // the listener gives the state at once; the settings may have changed meanwhile
        CamexTunnel.addListener(listener);
        TunnelLog.onChange(this::showLogText);
        showLogText();
    }

    @Override
    protected void onStop() {
        CamexTunnel.removeListener(listener);
        TunnelLog.onChange(null);
        super.onStop();
    }

    private void showLog(boolean on) {
        logWindow.view.setVisibility(on ? View.VISIBLE : View.GONE);
        if (on) logWindow.shown();
    }

    private void showLogText() {
        logWindow.setText(TunnelLog.text(), TunnelLog.removed());
    }

    private void openSettings() {
        startActivity(new Intent(this, SettingsActivity.class));
    }

    // Android 13+ needs runtime POST_NOTIFICATIONS for the foreground notification.
    private void requestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                    != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, 1);
        }
    }

    private void startVpn() {
        // Protocol v2 needs both keys and a server: without them camex cannot start
        String[] bad = CamexTunnel.check(Prefs.load(this));
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
            TunnelLog.add("start");
            Prefs.of(this).edit().putBoolean(Prefs.RUNNING, true).apply();
            CamexTunnel.start(this, Prefs.load(this));
        }
    }

    private void showState(CamexTunnel.State state, String address, String error) {
        boolean on = state == CamexTunnel.State.CONNECTING || state == CamexTunnel.State.CONNECTED;
        // red only for an error; a stopped tunnel: a lamp without colour
        int lamp;
        String text;
        switch (state) {
            case CONNECTED:
                lamp = R.color.green;
                text = "CONNECTED · " + address;
                break;
            case CONNECTING:
                lamp = R.color.amber;
                text = "CONNECTING";
                break;
            case NEEDS_CONSENT:
                lamp = R.color.amber;
                text = "VPN CONSENT NEEDED";
                break;
            case FAILED:
                lamp = R.color.red;
                text = "FAILED";
                break;
            default:
                lamp = Brass.Row.UNLIT;
                text = "STOPPED";
        }
        if (!error.isEmpty() && state != CamexTunnel.State.CONNECTED) text += "\n" + error;
        rowTunnel.set(lamp, text);

        Map<String, String> v = Prefs.load(this);
        String server = v.get(CamexTunnel.SERVER);
        rowServer.set(0, server.isEmpty() ? "NOT SET" : server);
        rowClient.set(0, v.get(CamexTunnel.NAME));
        String mtu = v.get(CamexTunnel.MTU);
        rowProto.set(0, v.get(CamexTunnel.TRANSPORT) + (mtu.isEmpty() ? "" : " · MTU " + mtu));

        startButton.setEnabled(!on);
        stopButton.setEnabled(on);
        // Settings only editable while disconnected
        settingsButton.setEnabled(!on);
    }
}
