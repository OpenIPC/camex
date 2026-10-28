/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * ConnectLink.java — the settings of a link printed by install.sh (#146)
 */
package org.openipc.camex;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Base64;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The tunnel's settings in a link, field by field:
 * {@code https://openipc.github.io/camex/connect/#h=&p=&t=&k=&s=} (the
 * settings after {@code #}, never sent to a server) or
 * {@code camex://connect?h=&p=&t=&k=&s=}. Fields: {@code h} host, {@code p}
 * port, {@code t} transport, {@code k} server public key, {@code s} psk,
 * optional {@code n} client ID, {@code m} MTU, {@code a} keepalive,
 * {@code r} route all traffic (1 or 0). A malformed field is left out and
 * named in {@link #rejected}; unknown fields are ignored.
 *
 * Plain Java (no android.*): tested on the JVM.
 */
final class ConnectLink {

    static final String WEB_PREFIX = "https://openipc.github.io/camex/connect";
    static final String SCHEME_PREFIX = "camex://connect";

    private static final String HOST =
        "[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?(\\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*";

    String host, transport, pubkey, psk, name;
    Integer port, mtu, keepalive;
    Boolean fullTunnel;
    /** The fields of the link that were malformed, in the link's order. */
    final List<String> rejected = new ArrayList<>();

    private ConnectLink() { }

    /** The link's settings, or null when it is not a camex link. */
    static ConnectLink parse(String link) {
        if (link == null) return null;
        String fields;
        if (link.startsWith(WEB_PREFIX)) {
            // .../connect/#... or .../connect#... (the manifest takes both)
            String rest = link.substring(WEB_PREFIX.length());
            if (rest.startsWith("/")) rest = rest.substring(1);
            if (!rest.startsWith("#")) return null;
            fields = rest.substring(1);
        } else if (link.startsWith(SCHEME_PREFIX)) {
            String rest = link.substring(SCHEME_PREFIX.length());
            if (rest.startsWith("/")) rest = rest.substring(1);
            if (!rest.startsWith("?") && !rest.startsWith("#")) return null;
            fields = rest.substring(1);
        } else {
            return null;
        }
        ConnectLink c = new ConnectLink();
        for (Map.Entry<String, String> f : split(fields).entrySet()) {
            if (!c.take(f.getKey(), f.getValue())) c.rejected.add(f.getKey());
        }
        return c;
    }

    /** No field at all came through. */
    boolean isEmpty() {
        return host == null && port == null && transport == null && pubkey == null
            && psk == null && name == null && mtu == null && keepalive == null
            && fullTunnel == null;
    }

    // key=value pairs split on '&'; the last of a repeated key wins; a
    // value that does not decode is kept as null (rejected)
    private static Map<String, String> split(String fields) {
        Map<String, String> out = new LinkedHashMap<>();
        for (String pair : fields.split("&", -1)) {
            if (pair.isEmpty()) continue;
            int eq = pair.indexOf('=');
            String key = eq < 0 ? pair : pair.substring(0, eq);
            String value = eq < 0 ? "" : pair.substring(eq + 1);
            out.remove(key);
            out.put(key, decode(value));
        }
        return out;
    }

    /**
     * %XX as UTF-8; '+' stays '+' (it is a character of base64 keys), so a
     * blank is %20; a character as it is (a pair of surrogates as one).
     * Null when the escapes, the surrogates or the UTF-8 are malformed.
     */
    static String decode(String s) {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        for (int i = 0; i < s.length(); ) {
            char ch = s.charAt(i);
            if (ch == '%') {
                if (i + 2 >= s.length()) return null;
                int hi = Character.digit(s.charAt(i + 1), 16);
                int lo = Character.digit(s.charAt(i + 2), 16);
                if (hi < 0 || lo < 0) return null;
                b.write(hi * 16 + lo);
                i += 3;
            } else if (ch < 0x80) {
                b.write(ch);
                i++;
            } else {
                int cp = s.codePointAt(i);
                if (cp >= 0xD800 && cp <= 0xDFFF) return null;   // a lone surrogate
                byte[] u = new String(Character.toChars(cp)).getBytes(StandardCharsets.UTF_8);
                b.write(u, 0, u.length);
                i += Character.charCount(cp);
            }
        }
        byte[] raw = b.toByteArray();
        String out = new String(raw, StandardCharsets.UTF_8);
        // Bytes that are not UTF-8 come back as U+FFFD: refuse those
        if (!java.util.Arrays.equals(out.getBytes(StandardCharsets.UTF_8), raw)) return null;
        return out;
    }

    private boolean take(String key, String v) {
        if (v == null) return !isKnown(key);
        switch (key) {
            case "h":
                // A host name or an IPv4 address (camex resolves IPv4 only),
                // in its server_host[64]
                // (labels: letters, digits, '-' inside; an IPv4 address too)
                if (v.length() > 63 || !v.matches(HOST)) return false;
                host = v;
                return true;
            case "p":
                port = range(v, 1, 65535);
                return port != null;
            case "t":
                if (!v.equals("udp") && !v.equals("tcp")) return false;
                transport = v;
                return true;
            case "k":
                if (!isServerPubkey(v)) return false;
                pubkey = v;
                return true;
            case "s":
                // As camex takes --psk: one line, no blank at its ends (it
                // trims them), below 128 bytes
                if (v.isEmpty() || !v.equals(v.trim())
                        || v.getBytes(StandardCharsets.UTF_8).length >= 128) {
                    return false;
                }
                // No control character; nothing beyond U+FFFF, which JNI
                // would hand to camex in other bytes (modified UTF-8)
                for (int i = 0; i < v.length(); i++) {
                    char ch = v.charAt(i);
                    if (ch < 0x20 || ch == 0x7f || Character.isSurrogate(ch)) return false;
                }
                // A key in camex's forms: hex: 64 digits, base64: 32 bytes,
                // neither all zero
                if (v.startsWith("hex:")) {
                    String h = v.substring(4);
                    if (!h.matches("[0-9a-fA-F]{64}") || h.matches("0{64}")) return false;
                } else if (v.startsWith("base64:")) {
                    if (!isKey(v.substring(7))) return false;
                }
                psk = v;
                return true;
            case "n":
                // The client ID (CAMEX_CLIENT_ID_LEN 32 with its NUL)
                if (!v.matches("[A-Za-z0-9._-]{1,31}")) return false;
                name = v;
                return true;
            case "m":
                mtu = range(v, 576, 9000);
                return mtu != null;
            case "a":
                keepalive = range(v, 0, 3600);
                return keepalive != null;
            case "r":
                if (!v.equals("0") && !v.equals("1")) return false;
                fullTunnel = v.equals("1");
                return true;
            default:
                return true;   // a field of a later version
        }
    }

    private static boolean isKnown(String key) {
        return key.length() == 1 && "hptksnmar".indexOf(key.charAt(0)) >= 0;
    }

    private static Integer range(String v, int lo, int hi) {
        if (!v.matches("[0-9]{1,5}")) return null;
        int n = Integer.parseInt(v);
        return n >= lo && n <= hi ? n : null;
    }

    /** A server public key: base64 of exactly 32 bytes (camex --genkey). */
    static boolean isServerPubkey(String key) {
        return isKey(key);
    }

    // Exactly what camex accepts: 43 base64 characters and '=', 32 bytes,
    // not all zero
    private static boolean isKey(String key) {
        if (key == null || !key.matches("[A-Za-z0-9+/]{43}=")) return false;
        byte[] raw;
        try {
            raw = Base64.getDecoder().decode(key);
        } catch (IllegalArgumentException e) {
            return false;
        }
        if (raw.length != 32) return false;
        for (byte x : raw) if (x != 0) return true;
        return false;
    }
}
