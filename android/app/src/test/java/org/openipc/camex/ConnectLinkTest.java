/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * ConnectLinkTest.java — the link of install.sh, field by field (#146)
 */
package org.openipc.camex;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import java.util.Arrays;

import org.junit.Test;

public class ConnectLinkTest {

    private static final String KEY = "+v/tp4H5gAJTw3y6yaU1zbS6a6fUDV+e5EI6oh7cTZk=";
    private static final String WEB = "https://openipc.github.io/camex/connect/#";

    @Test
    public void webLinkAllFields() {
        ConnectLink c = ConnectLink.parse(WEB + "h=vpn.example.org&p=5900&t=tcp&k=" + KEY
            + "&s=my%20long%20pass%26phrase%2B%25&n=cam-1&m=1400&a=0&r=1");
        assertEquals("vpn.example.org", c.host);
        assertEquals(Integer.valueOf(5900), c.port);
        assertEquals("tcp", c.transport);
        assertEquals(KEY, c.pubkey);
        assertEquals("my long pass&phrase+%", c.psk);
        assertEquals("cam-1", c.name);
        assertEquals(Integer.valueOf(1400), c.mtu);
        assertEquals(Integer.valueOf(0), c.keepalive);
        assertTrue(c.fullTunnel);
        assertTrue(c.rejected.isEmpty());
    }

    @Test
    public void schemeLink() {
        ConnectLink c = ConnectLink.parse("camex://connect?h=10.0.0.1&p=5800&t=udp&k=" + KEY + "&s=x");
        assertEquals("10.0.0.1", c.host);
        assertEquals("udp", c.transport);
        assertEquals("x", c.psk);
        assertNull(c.name);
        assertNull(c.fullTunnel);
        c = ConnectLink.parse("camex://connect/?h=cam.example.org");
        assertEquals("cam.example.org", c.host);
    }

    @Test
    public void webLinkWithoutSlash() {
        ConnectLink c = ConnectLink.parse("https://openipc.github.io/camex/connect#p=5900");
        assertEquals(Integer.valueOf(5900), c.port);
        assertNull(ConnectLink.parse("https://openipc.github.io/camex/connectx#p=5900"));
    }

    @Test
    public void hostIsANameOrIpv4InCamexLimit() {
        StringBuilder h = new StringBuilder();
        for (int i = 0; i < 63; i++) h.append('a');
        assertEquals(h.toString(), ConnectLink.parse(WEB + "h=" + h).host);
        assertEquals(Arrays.asList("h"), ConnectLink.parse(WEB + "h=" + h + "a").rejected);
        assertEquals(Arrays.asList("h"), ConnectLink.parse(WEB + "h=.").rejected);
        assertEquals(Arrays.asList("h"), ConnectLink.parse(WEB + "h=-bad").rejected);
        assertEquals(Arrays.asList("h"), ConnectLink.parse(WEB + "h=a..b").rejected);
        assertEquals("10.0.0.1", ConnectLink.parse(WEB + "h=10.0.0.1").host);
        // camex resolves IPv4 only
        assertEquals(Arrays.asList("h"), ConnectLink.parse(WEB + "h=%5B2001:db8::1%5D").rejected);
        assertEquals(Arrays.asList("h"), ConnectLink.parse(WEB + "h=2001:db8::1").rejected);
    }

    @Test
    public void pskCharactersJniKeeps() {
        // Beyond U+FFFF (literal or %XX) and control characters: refused
        assertEquals(Arrays.asList("s"), ConnectLink.parse(WEB + "s=a\uD83D\uDE00").rejected);
        assertEquals(Arrays.asList("s"), ConnectLink.parse(WEB + "s=a%F0%9F%98%80").rejected);
        assertEquals(Arrays.asList("s"), ConnectLink.parse(WEB + "s=a%00b").rejected);
        assertEquals(Arrays.asList("s"), ConnectLink.parse(WEB + "s=a%09b").rejected);
        // A literal letter and its %XX form are the same
        assertEquals("пароль", ConnectLink.parse(WEB + "s=пароль").psk);
    }

    @Test
    public void literalSupplementaryCharacterKeptWhole() {
        assertEquals("\uD83D\uDE00", ConnectLink.decode("\uD83D\uDE00"));
        assertEquals("\uD83D\uDE00", ConnectLink.decode("%F0%9F%98%80"));
        assertNull(ConnectLink.decode("a\uD83Db"));   // a lone surrogate
    }

    @Test
    public void plusStaysInKeys() {
        // '+' and '/' of base64 come through unencoded
        ConnectLink c = ConnectLink.parse(WEB + "k=" + KEY + "&s=base64:" + KEY);
        assertEquals(KEY, c.pubkey);
        assertEquals("base64:" + KEY, c.psk);
    }

    @Test
    public void utf8Psk() {
        ConnectLink c = ConnectLink.parse(WEB + "s=%D0%BF%D0%B0%D1%80%D0%BE%D0%BB%D1%8C");
        assertEquals("пароль", c.psk);
    }

    @Test
    public void badFieldsRejectedOneByOne() {
        ConnectLink c = ConnectLink.parse(WEB + "h=a%20b&p=70000&t=sctp&k=short&s=%20x&n=a/b"
            + "&m=100&a=-1&r=yes&x=later");
        assertNull(c.host);
        assertNull(c.port);
        assertNull(c.transport);
        assertNull(c.pubkey);
        assertNull(c.psk);
        assertNull(c.name);
        assertNull(c.mtu);
        assertNull(c.keepalive);
        assertNull(c.fullTunnel);
        assertEquals(Arrays.asList("h", "p", "t", "k", "s", "n", "m", "a", "r"), c.rejected);
        assertTrue(c.isEmpty());
    }

    @Test
    public void badEscapesAndBytes() {
        ConnectLink c = ConnectLink.parse(WEB + "s=%zz&h=%FF&p=5800");
        assertEquals(Arrays.asList("s", "h"), c.rejected);
        assertEquals(Integer.valueOf(5800), c.port);
        assertNull(ConnectLink.decode("%4"));
    }

    @Test
    public void pskLimits() {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < 64; i++) b.append("%D0%B9");   // 128 bytes
        assertFalse(ConnectLink.parse(WEB + "s=" + b).rejected.isEmpty());
        assertFalse(ConnectLink.parse(WEB + "s=a%0Ab").rejected.isEmpty());
        assertFalse(ConnectLink.parse(WEB + "s=").rejected.isEmpty());
    }

    @Test
    public void keysInCamexForms() {
        String zero = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
        assertEquals(Arrays.asList("k"), ConnectLink.parse(WEB + "k=" + zero).rejected);
        assertFalse(ConnectLink.isServerPubkey(zero));
        assertEquals(Arrays.asList("s"), ConnectLink.parse(WEB + "s=hex:bad").rejected);
        StringBuilder z = new StringBuilder("hex:");
        for (int i = 0; i < 64; i++) z.append('0');
        assertEquals(Arrays.asList("s"), ConnectLink.parse(WEB + "s=" + z).rejected);
        assertEquals(Arrays.asList("s"), ConnectLink.parse(WEB + "s=base64:" + zero).rejected);
        assertEquals(Arrays.asList("s"), ConnectLink.parse(WEB + "s=base64:short").rejected);
        String hex = "hex:5756dc482cd211054279c9054dcf1783430954058c0f5a5c09c6842fb76e3fbd";
        assertEquals(hex, ConnectLink.parse(WEB + "s=" + hex.replace(":", "%3A")).psk);
    }

    @Test
    public void notCamexLinks() {
        assertNull(ConnectLink.parse(null));
        assertNull(ConnectLink.parse("https://example.org/camex/connect/#h=x"));
        assertNull(ConnectLink.parse("https://openipc.github.io/camex/connect/"));
        assertNull(ConnectLink.parse("camex://other?h=x"));
        assertTrue(ConnectLink.parse(WEB).isEmpty());
    }

    @Test
    public void lastOfARepeatedFieldWins() {
        ConnectLink c = ConnectLink.parse(WEB + "p=1&p=2");
        assertEquals(Integer.valueOf(2), c.port);
    }
}
