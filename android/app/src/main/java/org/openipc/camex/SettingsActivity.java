/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * SettingsActivity.java — camex connection settings (platform widgets only)
 */
package org.openipc.camex;

import android.app.Activity;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.net.Uri;
import android.os.Bundle;
import android.text.InputType;
import android.text.TextUtils;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.RadioGroup;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.List;

/**
 * Editable connection settings persisted to the "camex" SharedPreferences, read
 * back by {@link TunnelService} when building the camex argv. Also opened by a
 * camex link ({@link ConnectLink}, #146): its values are filled in and marked,
 * nothing is saved or connected until the user taps Save.
 */
public class SettingsActivity extends Activity {

    private static final String PREFS = "camex";
    private static final int DEFAULT_PORT = 5800;
    private static final String DEFAULT_NAME = "android-client";
    // camex defaults (see src/main.c)
    private static final int DEFAULT_MTU = 1500;
    private static final int DEFAULT_KEEPALIVE = 10;
    private static final int DEFAULT_TIMEOUT = 20;
    // The underline of a field a link changed
    private static final int LINK_MARK = 0xFF2E7D32;

    private EditText hostInput, portInput, nameInput, pskInput;
    private EditText pubkeyInput;
    private EditText mtuInput, keepaliveInput, timeoutInput;
    private Switch fullTunnelSwitch;
    private RadioGroup transportGroup;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setTitle(R.string.settings_title);
        setContentView(R.layout.activity_settings);

        hostInput = findViewById(R.id.input_server_host);
        portInput = findViewById(R.id.input_server_port);
        nameInput = findViewById(R.id.input_name);
        pskInput = findViewById(R.id.input_psk);
        pubkeyInput = findViewById(R.id.input_server_pubkey);
        fullTunnelSwitch = findViewById(R.id.switch_full_tunnel);
        transportGroup = findViewById(R.id.group_transport);
        mtuInput = findViewById(R.id.input_mtu);
        keepaliveInput = findViewById(R.id.input_keepalive);
        timeoutInput = findViewById(R.id.input_server_timeout);

        CheckBox showPsk = findViewById(R.id.check_show_psk);
        showPsk.setOnCheckedChangeListener((b, show) -> {
            int sel = pskInput.getSelectionEnd();
            pskInput.setInputType(InputType.TYPE_CLASS_TEXT | (show
                ? InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                : InputType.TYPE_TEXT_VARIATION_PASSWORD));
            pskInput.setSelection(Math.max(sel, 0));
        });

        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        // After a rotation the views restore what was typed: keep it
        if (savedInstanceState == null) {
            load(prefs);
            Uri data = getIntent().getData();
            if (data != null) fromLink(data.toString());
        }

        Button save = findViewById(R.id.btn_save);
        save.setOnClickListener(view -> save(prefs));
    }

    private void load(SharedPreferences prefs) {
        hostInput.setText(prefs.getString("server_host", ""));
        portInput.setText(String.valueOf(prefs.getInt("server_port", DEFAULT_PORT)));
        nameInput.setText(prefs.getString("name", DEFAULT_NAME));
        pskInput.setText(prefs.getString("psk", ""));
        pubkeyInput.setText(prefs.getString("server_pubkey", ""));
        fullTunnelSwitch.setChecked(prefs.getBoolean("full_tunnel", false));

        // Transport: default UDP unless "tcp" was saved.
        boolean tcp = "tcp".equals(prefs.getString("transport", "udp"));
        transportGroup.check(tcp ? R.id.btn_tcp : R.id.btn_udp);

        mtuInput.setText(String.valueOf(prefs.getInt("mtu", DEFAULT_MTU)));
        keepaliveInput.setText(String.valueOf(prefs.getInt("keepalive", DEFAULT_KEEPALIVE)));
        timeoutInput.setText(String.valueOf(prefs.getInt("server_timeout", DEFAULT_TIMEOUT)));
    }

    /** A link's values over the saved ones: the changed fields marked. */
    private void fromLink(String link) {
        ConnectLink c = ConnectLink.parse(link);
        if (c == null) return;
        List<String> changed = new ArrayList<>();
        set(hostInput, c.host, R.string.server_host, changed);
        set(portInput, c.port == null ? null : String.valueOf(c.port), R.string.server_port, changed);
        set(nameInput, c.name, R.string.client_name, changed);
        set(pubkeyInput, c.pubkey, R.string.server_pubkey, changed);
        set(pskInput, c.psk, R.string.psk, changed);
        set(mtuInput, c.mtu == null ? null : String.valueOf(c.mtu), R.string.mtu, changed);
        set(keepaliveInput, c.keepalive == null ? null : String.valueOf(c.keepalive),
            R.string.keepalive, changed);
        if (c.transport != null) {
            int id = "tcp".equals(c.transport) ? R.id.btn_tcp : R.id.btn_udp;
            if (transportGroup.getCheckedRadioButtonId() != id) {
                transportGroup.check(id);
                changed.add(getString(R.string.transport));
            }
        }
        if (c.fullTunnel != null && fullTunnelSwitch.isChecked() != c.fullTunnel) {
            fullTunnelSwitch.setChecked(c.fullTunnel);
            changed.add(getString(R.string.full_tunnel));
        }
        StringBuilder note = new StringBuilder();
        note.append(changed.isEmpty() ? getString(R.string.link_same)
            : getString(R.string.link_changed, TextUtils.join(", ", changed)));
        if (!c.rejected.isEmpty()) {
            List<String> bad = new ArrayList<>();
            for (String key : c.rejected) bad.add(label(key));
            note.append('\n').append(getString(R.string.link_rejected,
                TextUtils.join(", ", bad)));
        }
        TextView view = findViewById(R.id.text_link_note);
        view.setText(note);
        view.setVisibility(View.VISIBLE);
    }

    /** The name of a link's field, as the screen shows it. */
    private String label(String key) {
        switch (key) {
            case "h": return getString(R.string.server_host);
            case "p": return getString(R.string.server_port);
            case "t": return getString(R.string.transport);
            case "k": return getString(R.string.server_pubkey);
            case "s": return getString(R.string.psk);
            case "n": return getString(R.string.client_name);
            case "m": return getString(R.string.mtu);
            case "a": return getString(R.string.keepalive);
            case "r": return getString(R.string.full_tunnel);
            default: return key;
        }
    }

    private void set(EditText input, String value, int label, List<String> changed) {
        if (value == null || value.equals(text(input))) return;
        input.setText(value);
        input.setBackgroundTintList(ColorStateList.valueOf(LINK_MARK));
        changed.add(getString(label));
    }

    private void save(SharedPreferences prefs) {
        String host = text(hostInput);
        if (TextUtils.isEmpty(host)) {
            hostInput.setError(getString(R.string.server_host));
            return;
        }

        int port = DEFAULT_PORT;
        String portStr = text(portInput);
        if (!TextUtils.isEmpty(portStr)) {
            try {
                port = Integer.parseInt(portStr);
            } catch (NumberFormatException ignored) { /* keep default */ }
        }
        if (port < 1 || port > 65535) {
            portInput.setError(getString(R.string.server_port));
            return;
        }

        String name = text(nameInput);
        if (TextUtils.isEmpty(name)) name = DEFAULT_NAME;

        int mtu = parseInt(mtuInput, DEFAULT_MTU);
        if (mtu < 576 || mtu > 9000) {
            mtuInput.setError(getString(R.string.err_mtu));
            return;
        }

        int keepalive = parseInt(keepaliveInput, DEFAULT_KEEPALIVE);
        if (keepalive < 0 || keepalive > 3600) {
            keepaliveInput.setError(getString(R.string.err_keepalive));
            return;
        }

        int timeout = parseInt(timeoutInput, DEFAULT_TIMEOUT);
        if (timeout < 5 || timeout > 3600) {
            timeoutInput.setError(getString(R.string.err_timeout));
            return;
        }

        // Protocol v2 is always encrypted: both keys are required
        String pubkey = text(pubkeyInput).trim();
        if (!isServerPubkey(pubkey)) {
            pubkeyInput.setError(getString(R.string.err_server_pubkey));
            return;
        }
        String psk = text(pskInput);
        if (TextUtils.isEmpty(psk)) {
            pskInput.setError(getString(R.string.err_psk));
            return;
        }

        String transport =
            transportGroup.getCheckedRadioButtonId() == R.id.btn_tcp ? "tcp" : "udp";

        prefs.edit()
            .putString("server_host", host)
            .putInt("server_port", port)
            .putString("name", name)
            .putString("transport", transport)
            .putBoolean("full_tunnel", fullTunnelSwitch.isChecked())
            .putString("server_pubkey", pubkey)
            .putString("psk", psk)
            .putInt("mtu", mtu)
            .putInt("keepalive", keepalive)
            .putInt("server_timeout", timeout)
            .apply();

        Toast.makeText(this, R.string.save, Toast.LENGTH_SHORT).show();
        finish();
    }

    private static String text(EditText input) {
        return input.getText() == null ? "" : input.getText().toString().trim();
    }

    private static int parseInt(EditText input, int fallback) {
        String s = text(input);
        if (TextUtils.isEmpty(s)) return fallback;
        try {
            return Integer.parseInt(s);
        } catch (NumberFormatException ignored) {
            return fallback;
        }
    }

    /** A server public key: base64 of exactly 32 bytes (camex --genkey). */
    static boolean isServerPubkey(String key) {
        return ConnectLink.isServerPubkey(key);
    }
}
