/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * SettingsBottomSheet.java — camex connection settings (Material bottom sheet)
 */
package org.openipc.camex;

import android.content.SharedPreferences;
import android.os.Bundle;
import android.text.TextUtils;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Toast;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import com.google.android.material.bottomsheet.BottomSheetDialogFragment;
import com.google.android.material.button.MaterialButton;
import com.google.android.material.button.MaterialButtonToggleGroup;
import com.google.android.material.switchmaterial.SwitchMaterial;
import com.google.android.material.textfield.TextInputEditText;
import com.google.android.material.textfield.TextInputLayout;

/**
 * Editable connection settings persisted to the "camex" SharedPreferences, read
 * back by {@link TunnelService} when building the camex argv.
 */
public class SettingsBottomSheet extends BottomSheetDialogFragment {

    private static final String PREFS = "camex";
    private static final int DEFAULT_PORT = 5800;
    private static final String DEFAULT_NAME = "android-client";
    // camex defaults (see src/main.c)
    private static final int DEFAULT_MTU = 1500;
    private static final int DEFAULT_KEEPALIVE = 10;
    private static final int DEFAULT_TIMEOUT = 20;

    private TextInputEditText hostInput, portInput, nameInput, pskInput;
    private TextInputEditText mtuInput, keepaliveInput, timeoutInput;
    private TextInputLayout pskLayout;
    private SwitchMaterial encryptSwitch;
    private MaterialButtonToggleGroup transportGroup;

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater,
                             @Nullable ViewGroup container,
                             @Nullable Bundle savedInstanceState) {
        return inflater.inflate(R.layout.bottom_sheet_settings, container, false);
    }

    @Override
    public void onViewCreated(@NonNull View v, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(v, savedInstanceState);

        hostInput = v.findViewById(R.id.input_server_host);
        portInput = v.findViewById(R.id.input_server_port);
        nameInput = v.findViewById(R.id.input_name);
        pskInput = v.findViewById(R.id.input_psk);
        pskLayout = v.findViewById(R.id.layout_psk);
        encryptSwitch = v.findViewById(R.id.switch_encrypt);
        transportGroup = v.findViewById(R.id.group_transport);
        mtuInput = v.findViewById(R.id.input_mtu);
        keepaliveInput = v.findViewById(R.id.input_keepalive);
        timeoutInput = v.findViewById(R.id.input_server_timeout);

        SharedPreferences prefs = requireContext().getSharedPreferences(PREFS, 0);
        hostInput.setText(prefs.getString("server_host", ""));
        portInput.setText(String.valueOf(prefs.getInt("server_port", DEFAULT_PORT)));
        nameInput.setText(prefs.getString("name", DEFAULT_NAME));
        pskInput.setText(prefs.getString("psk", ""));
        boolean encrypt = prefs.getBoolean("encrypt", false);
        encryptSwitch.setChecked(encrypt);
        pskLayout.setEnabled(encrypt);

        encryptSwitch.setOnCheckedChangeListener((b, checked) -> pskLayout.setEnabled(checked));

        // Transport: default UDP unless "tcp" was saved.
        boolean tcp = "tcp".equals(prefs.getString("transport", "udp"));
        transportGroup.check(tcp ? R.id.btn_tcp : R.id.btn_udp);

        mtuInput.setText(String.valueOf(prefs.getInt("mtu", DEFAULT_MTU)));
        keepaliveInput.setText(String.valueOf(prefs.getInt("keepalive", DEFAULT_KEEPALIVE)));
        timeoutInput.setText(String.valueOf(prefs.getInt("server_timeout", DEFAULT_TIMEOUT)));

        MaterialButton save = v.findViewById(R.id.btn_save);
        save.setOnClickListener(view -> save(prefs));
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

        String transport =
            transportGroup.getCheckedButtonId() == R.id.btn_tcp ? "tcp" : "udp";

        prefs.edit()
            .putString("server_host", host)
            .putInt("server_port", port)
            .putString("name", name)
            .putString("transport", transport)
            .putBoolean("encrypt", encryptSwitch.isChecked())
            .putString("psk", text(pskInput))
            .putInt("mtu", mtu)
            .putInt("keepalive", keepalive)
            .putInt("server_timeout", timeout)
            .apply();

        Toast.makeText(requireContext(), R.string.save, Toast.LENGTH_SHORT).show();
        dismiss();
    }

    private static String text(TextInputEditText input) {
        return input.getText() == null ? "" : input.getText().toString().trim();
    }

    private static int parseInt(TextInputEditText input, int fallback) {
        String s = text(input);
        if (TextUtils.isEmpty(s)) return fallback;
        try {
            return Integer.parseInt(s);
        } catch (NumberFormatException ignored) {
            return fallback;
        }
    }
}
