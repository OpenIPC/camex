/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * ConnectLink.java — the tunnel's values from a camex link (#170), as camex's app reads it (#146)
 */
package org.openipc.camex.tunnel;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * A link printed by camex's install.sh, field by field:
 * {@code https://openipc.github.io/camex/connect/#h=&p=&t=&k=&s=} (the fields after {@code #},
 * never sent to a server) or {@code camex://connect?h=&p=&t=&k=&s=}. Fields: {@code h} host,
 * {@code p} port, {@code t} transport, {@code k} server public key, {@code s} psk, {@code w}
 * the network's name (camex 4.6: with it {@code k} and {@code s} may be left out), optional
 * {@code n} client name, {@code m} MTU, {@code a} keepalive, {@code r} route all traffic.
 *
 * <p>{@link #values} holds what came through, under the keys of {@link CamexTunnel#fields()}:
 * {@code h} and {@code p} make {@code server} (a host without a port takes camex's 5800; a
 * malformed port, or a port without a host, leaves {@code server} out: {@link #host} and
 * {@link #port} have each on its own), the
 * transport is {@code UDP} or {@code TCP}. A malformed field is left out of the values and named
 * in {@link #errors}; unknown fields are ignored (a later version's). {@code r} is read and
 * ignored: the library never routes all traffic. The app puts the values over its own
 * settings, checks them with {@link CamexTunnel#check} and saves them.
 *
 * <p>Plain Java (no android.*): tested on the JVM.
 */
public final class ConnectLink {
    public static final String WEB_PREFIX = "https://openipc.github.io/camex/connect";
    public static final String SCHEME_PREFIX = "camex://connect";

    /** The values that came through, by field key, in the order of the fields. */
    public final Map<String, String> values;
    /** The link's malformed fields: its letter ("h", "s"...) and a short reason. */
    public final Map<String, String> errors;
    /**
     * The link's host and port each on its own, null when the link has none (or a malformed
     * one): an app that keeps them apart puts only what the link has over its settings.
     */
    public final String host;
    public final Integer port;

    private ConnectLink(Map<String, String> values, Map<String, String> errors, String host,
                        Integer port) {
        this.values = Collections.unmodifiableMap(values);
        this.errors = Collections.unmodifiableMap(errors);
        this.host = host;
        this.port = port;
    }

    /** The link's values, or null when it is not a camex link. */
    public static ConnectLink parse(String link) {
        if (link == null) return null;
        String fields;
        if (link.startsWith(WEB_PREFIX)) {
            // .../connect/#... or .../connect#...
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

        // key=value pairs split on '&'; the last of a repeated key wins
        Map<String, String> raw = new LinkedHashMap<>();
        for (String pair : fields.split("&", -1)) {
            if (pair.isEmpty()) continue;
            int eq = pair.indexOf('=');
            String key = eq < 0 ? pair : pair.substring(0, eq);
            raw.remove(key);
            raw.put(key, decode(eq < 0 ? "" : pair.substring(eq + 1)));
        }

        Map<String, String> errors = new LinkedHashMap<>();
        Map<String, String> got = new LinkedHashMap<>();
        for (Map.Entry<String, String> f : raw.entrySet()) {
            String key = f.getKey();
            if (key.length() != 1 || "hptkswnmar".indexOf(key.charAt(0)) < 0) continue;
            String v = f.getValue();
            String why = v == null ? "not URL-encoded UTF-8" : reason(key, v);
            if (why == null) got.put(key, v);
            else errors.put(key, why);
        }

        Map<String, String> values = new LinkedHashMap<>();
        String host = got.get("h");
        String port = got.get("p");
        if (host != null && !errors.containsKey("p")) {
            // a malformed port leaves the server out, not the default port in
            values.put(Values.SERVER, host + ":" + (port != null
                    ? Integer.parseInt(port) : Values.DEFAULT_PORT));
        }
        if (got.containsKey("n")) values.put(Values.NAME, got.get("n"));
        if (got.containsKey("t")) values.put(Values.TRANSPORT, got.get("t").toUpperCase(java.util.Locale.ROOT));
        if (got.containsKey("w")) values.put(Values.NETWORK, got.get("w"));
        if (got.containsKey("k")) values.put(Values.PUBKEY, got.get("k"));
        if (got.containsKey("s")) values.put(Values.PSK, got.get("s"));
        if (got.containsKey("m")) values.put(Values.MTU, String.valueOf(Integer.parseInt(got.get("m"))));
        if (got.containsKey("a")) values.put(Values.KEEPALIVE, String.valueOf(Integer.parseInt(got.get("a"))));
        return new ConnectLink(values, errors, host,
                port != null ? Integer.valueOf(Integer.parseInt(port)) : null);
    }

    // Null when the link's field is fine, otherwise why not
    private static String reason(String key, String v) {
        switch (key) {
            case "h":
                return Values.isHost(v) ? null : "a host name or an IPv4 address, up to 63 characters";
            case "p":
                return Values.range(v, 1, 65535) != null ? null : "a port, 1 to 65535";
            case "t":
                return v.equals("udp") || v.equals("tcp") ? null : "udp or tcp";
            case "k":
                return Values.isKey(v) ? null : "44 characters of camex --genkey";
            case "s":
                return Values.isPsk(v) ? null : "the server's psk: one line below 128 bytes";
            case "n":
                return Values.isName(v) ? null : "1 to 31 bytes, no control character";
            case "w":
                return Values.isNetwork(v) ? null : "a network name: 1 to 63 bytes, no control character";
            case "m":
                return Values.range(v, 576, 9000) != null ? null : "576 to 9000";
            case "a":
                return Values.range(v, 0, 3600) != null ? null : "0 to 3600";
            case "r":
                return v.equals("0") || v.equals("1") ? null : "0 or 1";
            default:
                return null;
        }
    }

    /**
     * %XX as UTF-8; '+' stays '+' (it is a character of base64 keys), so a blank is %20.
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
}
