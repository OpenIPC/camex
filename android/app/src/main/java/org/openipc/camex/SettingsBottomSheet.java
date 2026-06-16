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

    private TextInputEditText hostInput, portInput, nameInput, pskInput;
    private TextInputLayout pskLayout;
    private SwitchMaterial encryptSwitch;

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

        SharedPreferences prefs = requireContext().getSharedPreferences(PREFS, 0);
        hostInput.setText(prefs.getString("server_host", ""));
        portInput.setText(String.valueOf(prefs.getInt("server_port", DEFAULT_PORT)));
        nameInput.setText(prefs.getString("name", DEFAULT_NAME));
        pskInput.setText(prefs.getString("psk", ""));
        boolean encrypt = prefs.getBoolean("encrypt", false);
        encryptSwitch.setChecked(encrypt);
        pskLayout.setEnabled(encrypt);

        encryptSwitch.setOnCheckedChangeListener((b, checked) -> pskLayout.setEnabled(checked));

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

        prefs.edit()
            .putString("server_host", host)
            .putInt("server_port", port)
            .putString("name", name)
            .putBoolean("encrypt", encryptSwitch.isChecked())
            .putString("psk", text(pskInput))
            .apply();

        Toast.makeText(requireContext(), R.string.save, Toast.LENGTH_SHORT).show();
        dismiss();
    }

    private static String text(TextInputEditText input) {
        return input.getText() == null ? "" : input.getText().toString().trim();
    }
}
