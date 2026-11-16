/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * ConsentActivity.java — shows the VPN consent dialog for the instrumented test, as an app's
 * own screen does: the system dialog needs startActivityForResult (its caller's package)
 */
package org.openipc.camex.tunnel;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

public class ConsentActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Intent i = CamexTunnel.consentIntent(this);
        if (i == null) {
            finish();
            return;
        }
        startActivityForResult(i, 1);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        Log.i("CAMEX_E2E", "consent " + (result == RESULT_OK ? "given" : "refused"));
        finish();
    }
}
