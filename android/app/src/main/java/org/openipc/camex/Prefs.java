/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * Prefs.java — camex's saved settings as the tunnel library's values
 */
package org.openipc.camex;

import android.content.Context;
import android.content.SharedPreferences;

import org.openipc.camex.tunnel.CamexTunnel;

import java.util.HashMap;
import java.util.Map;

/**
 * The settings, kept in the "camex" preferences in the layout of earlier versions (host and port
 * apart, numbers as ints), so an update keeps them; read and written as the library's values
 * (keys of {@link CamexTunnel#fields()}). An empty number is not stored: camex takes its default
 * (the field's hint). No client name saved: {@link CamexTunnel#defaultName} with "camex".
 */
final class Prefs {
    static final String FILE = "camex";
    static final String APP = "camex";
    static final int DEFAULT_PORT = 5800;
    static final String START_ON_BOOT = "start_on_boot";
    static final String SHOW_LOG = "show_log";

    private Prefs() { }

    static SharedPreferences of(Context c) {
        return c.getSharedPreferences(FILE, Context.MODE_PRIVATE);
    }

    static Map<String, String> load(Context c) {
        SharedPreferences p = of(c);
        Map<String, String> v = new HashMap<>();
        String host = p.getString("server_host", "");
        v.put(CamexTunnel.SERVER, host.isEmpty() ? "" : host + ":" + p.getInt("server_port", DEFAULT_PORT));
        String name = p.getString("name", "");
        v.put(CamexTunnel.NAME, name.isEmpty() ? CamexTunnel.defaultName(c, APP) : name);
        v.put(CamexTunnel.TRANSPORT, "tcp".equals(p.getString("transport", "udp")) ? "TCP" : "UDP");
        v.put(CamexTunnel.PUBKEY, p.getString("server_pubkey", ""));
        v.put(CamexTunnel.PSK, p.getString("psk", ""));
        v.put(CamexTunnel.MTU, number(p, "mtu"));
        v.put(CamexTunnel.KEEPALIVE, number(p, "keepalive"));
        v.put(CamexTunnel.SERVER_TIMEOUT, number(p, "server_timeout"));
        return v;
    }

    /** Saves checked values (CamexTunnel.check() gave null). */
    static void save(Context c, Map<String, String> v) {
        String server = v.get(CamexTunnel.SERVER).trim();
        int colon = server.lastIndexOf(':');
        SharedPreferences.Editor e = of(c).edit()
            .putString("server_host", server.substring(0, colon))
            .putInt("server_port", Integer.parseInt(server.substring(colon + 1)))
            .putString("name", v.get(CamexTunnel.NAME))
            .putString("transport", "TCP".equals(v.get(CamexTunnel.TRANSPORT)) ? "tcp" : "udp")
            .remove("full_tunnel")   // no full tunnel any more (#172)
            .putString("server_pubkey", v.get(CamexTunnel.PUBKEY).trim())
            .putString("psk", v.get(CamexTunnel.PSK));
        number(e, "mtu", v.get(CamexTunnel.MTU));
        number(e, "keepalive", v.get(CamexTunnel.KEEPALIVE));
        number(e, "server_timeout", v.get(CamexTunnel.SERVER_TIMEOUT));
        e.apply();
    }

    private static String number(SharedPreferences p, String key) {
        return p.contains(key) ? String.valueOf(p.getInt(key, 0)) : "";
    }

    private static void number(SharedPreferences.Editor e, String key, String value) {
        String s = value == null ? "" : value.trim();
        if (s.isEmpty()) e.remove(key);
        else e.putInt(key, Integer.parseInt(s));
    }
}
