/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * ValuesTest.java — the fields, checks and command line of the tunnel library (#170)
 */
package org.openipc.camex.tunnel;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;

public class ValuesTest {
    static final String KEY = "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789+/AbCdE=";
    static final String PSK = "hex:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

    static Map<String, String> good() {
        Map<String, String> v = new HashMap<>();
        v.put(Values.SERVER, "vpn.example.org:5800");
        v.put(Values.NAME, "decoder-1");
        v.put(Values.TRANSPORT, "UDP");
        v.put(Values.PUBKEY, KEY);
        v.put(Values.PSK, PSK);
        return v;
    }

    private static String bad(String key, String value) {
        Map<String, String> v = good();
        v.put(key, value);
        String[] r = Values.check(v);
        return r == null ? null : r[0];
    }

    @Test
    public void fields() {
        assertEquals(Arrays.asList("server", "name", "transport", "pubkey", "psk", "mtu", "keepalive"),
                Arrays.asList(Values.FIELDS.stream().map(f -> f.key).toArray()));
        Field t = Values.FIELDS.get(2);
        assertEquals(Field.Kind.CHOICE, t.kind);
        assertEquals(Arrays.asList("UDP", "TCP"), t.choices);
        assertEquals("UDP", t.initial);
        assertEquals(Field.Kind.SECRET, Values.FIELDS.get(4).kind);
        assertFalse(Values.FIELDS.get(5).required);
        assertTrue(Values.FIELDS.get(0).required);
    }

    @Test
    public void goodValues() {
        assertNull(Values.check(good()));
        Map<String, String> v = good();
        v.put(Values.TRANSPORT, "tcp");
        v.put(Values.SERVER, "10.0.0.1:1");
        v.put(Values.MTU, "1400");
        v.put(Values.KEEPALIVE, "0");
        v.put(Values.PSK, "a plain pass phrase");
        assertNull(Values.check(v));
    }

    @Test
    public void badValues() {
        assertEquals("server", bad(Values.SERVER, "vpn.example.org"));
        assertEquals("server", bad(Values.SERVER, "vpn.example.org:0"));
        assertEquals("server", bad(Values.SERVER, "vpn.example.org:70000"));
        assertEquals("server", bad(Values.SERVER, ":5800"));
        assertEquals("server", bad(Values.SERVER, "bad_host:5800"));
        assertEquals("name", bad(Values.NAME, ""));
        assertEquals("name", bad(Values.NAME, " lead"));
        assertEquals("name", bad(Values.NAME, "0123456789012345678901234567890123"));
        assertEquals("name", bad(Values.NAME, "tab\there"));
        assertEquals("transport", bad(Values.TRANSPORT, "QUIC"));
        assertEquals("pubkey", bad(Values.PUBKEY, "short"));
        assertEquals("pubkey", bad(Values.PUBKEY, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="));
        assertEquals("psk", bad(Values.PSK, ""));
        assertEquals("psk", bad(Values.PSK, "hex:0011"));
        assertEquals("psk", bad(Values.PSK, "hex:" + new String(new char[64]).replace('\0', '0')));
        assertEquals("psk", bad(Values.PSK, "line\nbreak"));
        assertEquals("mtu", bad(Values.MTU, "100"));
        assertEquals("keepalive", bad(Values.KEEPALIVE, "x"));
        assertEquals("server", Values.check(new HashMap<>())[0]);
        assertEquals("server", Values.check(null)[0]);
    }

    @Test
    public void reasonNeverHoldsThePsk() {
        Map<String, String> v = good();
        v.put(Values.PSK, "hex:Secret");
        String[] r = Values.check(v);
        assertEquals("psk", r[0]);
        assertFalse(r[1].contains("Secret"));
    }

    @Test
    public void argv() {
        Map<String, String> v = good();
        v.put(Values.TRANSPORT, "TCP");
        v.put(Values.SERVER, " vpn.example.org:05800 ");
        assertArrayEquals(new String[]{"camex", "--mode", "client", "--auto", "--name", "decoder-1",
                        "--server-host", "vpn.example.org", "--port", "5800", "--transport", "tcp",
                        "--keepalive", "10", "--server-timeout", "20", "--server-pubkey", KEY,
                        "--psk", PSK},
                Values.argv(v));
        v.put(Values.MTU, "1400");
        v.put(Values.KEEPALIVE, "25");
        String[] a = Values.argv(v);
        assertEquals("25", a[Arrays.asList(a).indexOf("--keepalive") + 1]);
        assertEquals("1400", a[Arrays.asList(a).indexOf("--mtu") + 1]);
    }

    @Test
    public void networkOf() {
        assertEquals("10.200.0.0", Values.networkOf("10.200.0.5", 24));
        assertEquals("192.168.1.128", Values.networkOf("192.168.1.200", 25));
        assertEquals("0.0.0.0", Values.networkOf("10.200.0.5", 0));
    }
}
