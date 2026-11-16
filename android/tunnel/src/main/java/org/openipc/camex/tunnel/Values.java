/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * Values.java — the settings of the camex tunnel: fields, checks, camex's command line (#170)
 */
package org.openipc.camex.tunnel;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The tunnel's settings as a values Map, checked as camex takes them. Plain Java (no
 * android.*): tested on the JVM.
 */
final class Values {
    static final String SERVER = "server";
    static final String NAME = "name";
    static final String TRANSPORT = "transport";
    static final String PUBKEY = "pubkey";
    static final String PSK = "psk";
    static final String MTU = "mtu";
    static final String KEEPALIVE = "keepalive";

    /** camex's default server port (install.sh, the README). */
    static final int DEFAULT_PORT = 5800;

    static final List<Field> FIELDS = Collections.unmodifiableList(Arrays.asList(
            Field.hostPort(SERVER, "Server (host:port)", ""),
            Field.text(NAME, "Client name (its ID on the server)", "", true),
            Field.choice(TRANSPORT, "Transport", "UDP", "UDP", "TCP"),
            Field.text(PUBKEY, "Server public key", "", true),
            Field.secret(PSK, "Pre-shared key"),
            Field.text(MTU, "MTU (576 to 9000, empty: the server's)", "", false),
            Field.text(KEEPALIVE, "Keepalive, seconds (0 to 3600, empty: 10)", "", false)));

    // A host name or an IPv4 address (camex resolves IPv4 only), in its server_host[64]
    private static final String HOST =
        "[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?(\\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*";

    private Values() { }

    private static String get(Map<String, String> v, String key) {
        String s = v == null ? null : v.get(key);
        return s == null ? "" : s;
    }

    /**
     * Null when the values are fine, otherwise {@code {key, reason}} of the first wrong
     * field, in the order of {@link #FIELDS}.
     */
    static String[] check(Map<String, String> v) {
        String server = get(v, SERVER).trim();
        int colon = server.lastIndexOf(':');
        if (colon <= 0 || !isHost(server.substring(0, colon))
                || range(server.substring(colon + 1), 1, 65535) == null) {
            return new String[]{SERVER, "host:port"};
        }
        if (!isName(get(v, NAME))) {
            return new String[]{NAME, "1 to 31 bytes, no control character, no blank at the ends"};
        }
        if (transport(get(v, TRANSPORT)) == null) {
            return new String[]{TRANSPORT, "UDP or TCP"};
        }
        if (!isKey(get(v, PUBKEY).trim())) {
            return new String[]{PUBKEY, "44 characters of camex --genkey"};
        }
        if (!isPsk(get(v, PSK))) {
            return new String[]{PSK, "the server's psk: one line below 128 bytes"};
        }
        String mtu = get(v, MTU).trim();
        if (!mtu.isEmpty() && range(mtu, 576, 9000) == null) {
            return new String[]{MTU, "576 to 9000, or empty"};
        }
        String keepalive = get(v, KEEPALIVE).trim();
        if (!keepalive.isEmpty() && range(keepalive, 0, 3600) == null) {
            return new String[]{KEEPALIVE, "0 to 3600 seconds, or empty"};
        }
        return null;
    }

    /** camex's command line for checked values: a client in auto mode, as camex's app runs it. */
    static String[] argv(Map<String, String> v) {
        String server = get(v, SERVER).trim();
        int colon = server.lastIndexOf(':');
        String keepalive = get(v, KEEPALIVE).trim();
        List<String> a = new ArrayList<>(Arrays.asList(
                "camex", "--mode", "client", "--auto",
                "--name", get(v, NAME),
                "--server-host", server.substring(0, colon),
                "--port", String.valueOf(Integer.parseInt(server.substring(colon + 1))),
                "--transport", transport(get(v, TRANSPORT)),
                "--keepalive", keepalive.isEmpty() ? "10" : keepalive,
                "--server-timeout", "20",
                "--server-pubkey", get(v, PUBKEY).trim(),
                "--psk", get(v, PSK)));
        String mtu = get(v, MTU).trim();
        if (!mtu.isEmpty()) {
            a.add("--mtu");
            a.add(String.valueOf(Integer.parseInt(mtu)));
        }
        return a.toArray(new String[0]);
    }

    /** "udp" or "tcp" for UDP / TCP in any case, null otherwise. */
    static String transport(String t) {
        String s = t.trim().toLowerCase(Locale.ROOT);
        return s.equals("udp") || s.equals("tcp") ? s : null;
    }

    static boolean isHost(String h) {
        return h.length() <= 63 && h.matches(HOST);
    }

    /** The client ID as camex takes it: up to 31 bytes (its client_id[32]). */
    static boolean isName(String n) {
        return !n.isEmpty() && n.equals(n.trim())
                && n.getBytes(StandardCharsets.UTF_8).length <= 31 && plain(n);
    }

    /**
     * As camex takes --psk: one line, no blank at its ends (it trims them), below 128 bytes;
     * a key in camex's forms (hex: 64 digits, base64: 32 bytes) neither all zero.
     */
    static boolean isPsk(String s) {
        if (s.isEmpty() || !s.equals(s.trim())
                || s.getBytes(StandardCharsets.UTF_8).length >= 128 || !plain(s)) {
            return false;
        }
        if (s.startsWith("hex:")) {
            String h = s.substring(4);
            return h.matches("[0-9a-fA-F]{64}") && !h.matches("0{64}");
        }
        if (s.startsWith("base64:")) {
            return isKey(s.substring(7));
        }
        return true;
    }

    // No control character; nothing beyond U+FFFF, which JNI would hand to camex in other
    // bytes (modified UTF-8)
    private static boolean plain(String s) {
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            if (ch < 0x20 || ch == 0x7f || Character.isSurrogate(ch)) return false;
        }
        return true;
    }

    static Integer range(String v, int lo, int hi) {
        if (!v.matches("[0-9]{1,5}")) return null;
        int n = Integer.parseInt(v);
        return n >= lo && n <= hi ? n : null;
    }

    /** A key as camex takes it: 43 base64 characters and '=', 32 bytes, not all zero. */
    static boolean isKey(String key) {
        if (key == null || !key.matches("[A-Za-z0-9+/]{43}=")) return false;
        byte[] raw = base64(key);
        if (raw.length != 32) return false;
        for (byte x : raw) if (x != 0) return true;
        return false;
    }

    // java.util.Base64 needs API 26, android.util.Base64 is a stub on the JVM; like camex's
    // own decoder, the unused low bits of the last character are not checked
    private static byte[] base64(String s) {
        final String abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        int bits = 0, n = 0;
        for (int i = 0; i < s.length() && s.charAt(i) != '='; i++) {
            bits = (bits << 6) | abc.indexOf(s.charAt(i));
            n += 6;
            if (n >= 8) {
                n -= 8;
                out.write((bits >> n) & 0xff);
            }
        }
        return out.toByteArray();
    }

    /** Network address of ip/prefixLen, e.g. 10.200.0.5/24 -> 10.200.0.0. */
    static String networkOf(String ip, int prefixLen) {
        String[] p = ip.split("\\.");
        int addr = 0;
        for (String octet : p) addr = (addr << 8) | (Integer.parseInt(octet) & 0xff);
        int mask = prefixLen == 0 ? 0 : -1 << (32 - prefixLen);
        addr &= mask;
        return ((addr >>> 24) & 0xff) + "." + ((addr >>> 16) & 0xff) + "."
                + ((addr >>> 8) & 0xff) + "." + (addr & 0xff);
    }
}
