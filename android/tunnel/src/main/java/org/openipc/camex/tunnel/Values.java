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
    static final String NETWORK = "network";
    static final String SELECTOR = "selector";
    static final String MTU = "mtu";
    static final String KEEPALIVE = "keepalive";
    static final String SERVER_TIMEOUT = "server_timeout";
    static final String WEB_PORT = "web_port";

    /** camex's default server port (install.sh, the README). */
    static final int DEFAULT_PORT = 5800;

    // Short labels and the defaults as hints of the empty fields (owner's decision 2026-11-18)
    static final List<Field> FIELDS = Collections.unmodifiableList(Arrays.asList(
            Field.hostPort(SERVER, "Server (host:port)", ""),
            Field.text(NAME, "Client name", "", true),
            Field.choice(TRANSPORT, "Proto", "UDP", "UDP", "TCP"),
            // camex 4.6: the server's address and the network's name are enough; the key
            // and psk only without a network (or as extra protection)
            Field.secret(NETWORK, "Network name", "empty: server key and psk below"),
            // camex 6.1: public, only when the server's network block has selector= (#201)
            Field.text(SELECTOR, "Network selector", "empty: none (the server says if needed)"),
            Field.text(PUBKEY, "Server public key", "empty with a network: checked by its name"),
            Field.secret(PSK, "Pre-shared key", "empty with a network"),
            Field.text(MTU, "MTU (576\u20139000)", "empty: the server's"),
            Field.text(KEEPALIVE, "Keepalive, sec (0\u20133600)", "empty: 10"),
            Field.text(SERVER_TIMEOUT, "Server timeout, sec (5\u20133600)", "empty: 20"),
            Field.text(WEB_PORT, "Web port for nginx (1\u201365535)", "empty: none")));

    /**
     * The client name a new setting starts with: the app's name and the last 4 characters of
     * the device's ANDROID_ID, as {@code camex-3f2a}; the app's name in lower case, letters,
     * digits and dashes only, at most 26 characters; no ID: the app's name alone.
     */
    static String defaultName(String app, String androidId) {
        String a = app == null ? "" : app.toLowerCase(Locale.ROOT).replaceAll("[^a-z0-9-]", "");
        if (a.length() > 26) a = a.substring(0, 26);
        if (a.isEmpty()) a = "camex";
        String id = androidId == null ? "" : androidId.toLowerCase(Locale.ROOT).replaceAll("[^0-9a-z]", "");
        return id.length() < 4 ? a : a + "-" + id.substring(id.length() - 4);
    }

    /** {@link #FIELDS} with the client name starting as {@code name}. */
    static List<Field> fields(String name) {
        List<Field> out = new ArrayList<>();
        for (Field f : FIELDS) out.add(NAME.equals(f.key) ? f.withInitial(name) : f);
        return Collections.unmodifiableList(out);
    }

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
        String network = get(v, NETWORK);
        if (!network.isEmpty() && !isNetwork(network)) {
            return new String[]{NETWORK, "1 to 63 bytes, no control character, no blank at the ends"};
        }
        // The selector goes in CPace: with a network, without a psk (camex refuses it else)
        String selector = get(v, SELECTOR).trim();
        if (!selector.isEmpty() && !isSelector(selector)) {
            return new String[]{SELECTOR, "1 to 31 of A-Z a-z 0-9 . _ -, or empty"};
        }
        if (!selector.isEmpty() && (network.isEmpty() || !get(v, PSK).isEmpty())) {
            return new String[]{SELECTOR, "only with a network name and without a psk, or empty"};
        }
        // With a network both are optional: the name is the key, the server is checked by it
        String key = get(v, PUBKEY).trim();
        if ((network.isEmpty() || !key.isEmpty()) && !isKey(key)) {
            return new String[]{PUBKEY, network.isEmpty() ? "44 characters of camex --genkey"
                    : "44 characters of camex --genkey, or empty"};
        }
        String psk = get(v, PSK);
        if ((network.isEmpty() || !psk.isEmpty()) && !isPsk(psk)) {
            return new String[]{PSK, network.isEmpty()
                    ? "the server's psk: one line below 128 bytes"
                    : "the client's own psk: one line below 128 bytes, or empty"};
        }
        String mtu = get(v, MTU).trim();
        if (!mtu.isEmpty() && range(mtu, 576, 9000) == null) {
            return new String[]{MTU, "576 to 9000, or empty"};
        }
        String keepalive = get(v, KEEPALIVE).trim();
        if (!keepalive.isEmpty() && range(keepalive, 0, 3600) == null) {
            return new String[]{KEEPALIVE, "0 to 3600 seconds, or empty"};
        }
        String timeout = get(v, SERVER_TIMEOUT).trim();
        if (!timeout.isEmpty() && range(timeout, 5, 3600) == null) {
            return new String[]{SERVER_TIMEOUT, "5 to 3600 seconds, or empty"};
        }
        String web = get(v, WEB_PORT).trim();
        if (!web.isEmpty() && range(web, 1, 65535) == null) {
            return new String[]{WEB_PORT, "1 to 65535, or empty"};
        }
        return null;
    }

    /** camex's command line for checked values: a client in auto mode, as camex's app runs it. */
    static String[] argv(Map<String, String> v) {
        String server = get(v, SERVER).trim();
        int colon = server.lastIndexOf(':');
        String keepalive = get(v, KEEPALIVE).trim();
        String timeout = get(v, SERVER_TIMEOUT).trim();
        List<String> a = new ArrayList<>(Arrays.asList(
                "camex", "--mode", "client", "--auto",
                "--name", get(v, NAME),
                "--server-host", server.substring(0, colon),
                "--port", String.valueOf(Integer.parseInt(server.substring(colon + 1))),
                "--transport", transport(get(v, TRANSPORT)),
                "--keepalive", keepalive.isEmpty() ? "10" : keepalive,
                "--server-timeout", timeout.isEmpty() ? "20"
                        : String.valueOf(Integer.parseInt(timeout))));
        String network = get(v, NETWORK);
        if (!network.isEmpty()) {
            a.add("--network");
            a.add(network);
            // No file of known servers here: CPace checks the server at every start
            a.add("--known-servers");
            a.add("none");
            String selector = get(v, SELECTOR).trim();
            if (!selector.isEmpty()) {
                a.add("--network-selector");
                a.add(selector);
            }
        }
        if (!get(v, PUBKEY).trim().isEmpty()) {
            a.add("--server-pubkey");
            a.add(get(v, PUBKEY).trim());
        }
        if (!get(v, PSK).isEmpty()) {
            a.add("--psk");
            a.add(get(v, PSK));
        }
        String mtu = get(v, MTU).trim();
        if (!mtu.isEmpty()) {
            a.add("--mtu");
            a.add(String.valueOf(Integer.parseInt(mtu)));
        }
        // nginx's port on this client (#174): a server before 3.19.0 refuses it, so only
        // when given
        String web = get(v, WEB_PORT).trim();
        if (!web.isEmpty()) {
            a.add("--web-port");
            a.add(String.valueOf(Integer.parseInt(web)));
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
    /** As camex takes --network: 1 to 63 bytes, no control character, no blank at the ends. */
    static boolean isNetwork(String n) {
        return !n.isEmpty() && n.equals(n.trim())
                && n.getBytes(StandardCharsets.UTF_8).length <= 63 && plain(n);
    }

    /** As camex takes --network-selector (#201): 1 to 31 of A-Z a-z 0-9 . _ - */
    static boolean isSelector(String s) {
        return s.matches("[A-Za-z0-9._-]{1,31}");
    }

    static boolean isName(String n) {
        return !n.isEmpty() && n.equals(n.trim())
                && n.getBytes(StandardCharsets.UTF_8).length <= 31 && plain(n);
    }

    /**
     * As camex takes --psk: one line below 128 bytes, taken as it is (a passphrase may begin or
     * end with a blank); a key in camex's forms (hex: 64 digits, base64: 32 bytes) neither all
     * zero.
     */
    static boolean isPsk(String s) {
        if (s.isEmpty() || s.getBytes(StandardCharsets.UTF_8).length >= 128 || !plain(s)) {
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
    static byte[] base64(String s) {
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

    /** The widest route the tunnel takes from the server (#175): a /8. */
    static final int ROUTE_PREFIX_MIN = 8;

    /**
     * Whether a route from the server goes into the VPN: never a default route, nor routes
     * wide enough to take all the app's traffic together (0.0.0.0/1 and 128.0.0.0/1); camex
     * on Android only reaches the server's network and the clients behind it (#175).
     */
    static boolean routeAllowed(int prefixLen) {
        return prefixLen >= ROUTE_PREFIX_MIN && prefixLen <= 32;
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
