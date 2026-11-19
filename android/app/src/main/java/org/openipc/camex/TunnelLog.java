/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * TunnelLog.java — the tunnel's log under the panel (Show log): its states with the address
 * and the reason of a failure or reconnect, as the library reports them
 */
package org.openipc.camex;

import org.openipc.camex.tunnel.CamexTunnel;

import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.Date;
import java.util.Locale;

/** Process-wide, on the main thread: the library's listener runs there. */
final class TunnelLog {
    private static final int LINES = 50;
    private static final ArrayDeque<String> lines = new ArrayDeque<>();
    private static String last = "";
    private static Runnable onChange;
    private static boolean listening;

    private TunnelLog() { }

    /** Starts recording (once per process). */
    static void listen() {
        if (listening) return;
        listening = true;
        CamexTunnel.addListener((state, address, error) -> {
            String s = state.name().replace('_', ' ');
            if (!address.isEmpty()) s += " · " + address;
            if (!error.isEmpty() && state != CamexTunnel.State.CONNECTED) s += " · " + error;
            add(s);
        });
    }

    /** A line of the app's own (settings, boot). */
    static void add(String text) {
        if (text.equals(last)) return;   // the listener repeats the state when added
        last = text;
        String t = new SimpleDateFormat("HH:mm:ss", Locale.ROOT).format(new Date());
        lines.addLast(t + " " + text);
        while (lines.size() > LINES) lines.removeFirst();
        if (onChange != null) onChange.run();
    }

    static String text() {
        StringBuilder b = new StringBuilder();
        for (String l : lines) b.append(b.length() == 0 ? "" : "\n").append(l);
        return b.toString();
    }

    static void onChange(Runnable r) {
        onChange = r;
    }
}
