/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * LinkCasesTest.java — the link of install.sh, field by field (#146): the camex app's cases,
 * moved here with its parser (#172)
 */
package org.openipc.camex.tunnel;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import java.util.Arrays;
import java.util.Base64;
import java.util.Map;
import java.util.Random;

import org.junit.Test;

public class LinkCasesTest {

    private static final String KEY = "+v/tp4H5gAJTw3y6yaU1zbS6a6fUDV+e5EI6oh7cTZk=";
    private static final String WEB = "https://openipc.github.io/camex/connect/#";

    private static ConnectLink parse(String link) {
        return ConnectLink.parse(link);
    }

    private static Object errors(String link) {
        return Arrays.asList(parse(link).errors.keySet().toArray());
    }

    private static String value(String link, String key) {
        return parse(link).values.get(key);
    }

    @Test
    public void webLinkAllFields() {
        ConnectLink c = parse(WEB + "h=vpn.example.org&p=5900&t=tcp&k=" + KEY
            + "&s=my%20long%20pass%26phrase%2B%25&n=cam-1&m=1400&a=0&r=1");
        Map<String, String> v = c.values;
        assertEquals("vpn.example.org:5900", v.get(CamexTunnel.SERVER));
        assertEquals("TCP", v.get(CamexTunnel.TRANSPORT));
        assertEquals(KEY, v.get(CamexTunnel.PUBKEY));
        assertEquals("my long pass&phrase+%", v.get(CamexTunnel.PSK));
        assertEquals("cam-1", v.get(CamexTunnel.NAME));
        assertEquals("1400", v.get(CamexTunnel.MTU));
        assertEquals("0", v.get(CamexTunnel.KEEPALIVE));
        assertTrue(c.errors.isEmpty());
    }

    @Test
    public void schemeLink() {
        ConnectLink c = parse("camex://connect?h=10.0.0.1&p=5800&t=udp&k=" + KEY + "&s=x");
        assertEquals("10.0.0.1:5800", c.values.get(CamexTunnel.SERVER));
        assertEquals("UDP", c.values.get(CamexTunnel.TRANSPORT));
        assertEquals("x", c.values.get(CamexTunnel.PSK));
        assertNull(c.values.get(CamexTunnel.NAME));
        assertEquals("cam.example.org:5800", value("camex://connect/?h=cam.example.org",
                CamexTunnel.SERVER));
    }

    @Test
    public void webLinkWithoutSlash() {
        assertEquals("a:5900", value("https://openipc.github.io/camex/connect#h=a&p=5900",
                CamexTunnel.SERVER));
        assertNull(parse("https://openipc.github.io/camex/connectx#p=5900"));
    }

    @Test
    public void hostIsANameOrIpv4InCamexLimit() {
        StringBuilder h = new StringBuilder();
        for (int i = 0; i < 63; i++) h.append('a');
        assertEquals(h + ":5800", value(WEB + "h=" + h, CamexTunnel.SERVER));
        assertEquals(Arrays.asList("h"), errors(WEB + "h=" + h + "a"));
        assertEquals(Arrays.asList("h"), errors(WEB + "h=."));
        assertEquals(Arrays.asList("h"), errors(WEB + "h=-bad"));
        assertEquals(Arrays.asList("h"), errors(WEB + "h=a..b"));
        assertEquals("10.0.0.1:5800", value(WEB + "h=10.0.0.1", CamexTunnel.SERVER));
        // camex resolves IPv4 only
        assertEquals(Arrays.asList("h"), errors(WEB + "h=%5B2001:db8::1%5D"));
        assertEquals(Arrays.asList("h"), errors(WEB + "h=2001:db8::1"));
    }

    @Test
    public void pskCharactersJniKeeps() {
        // Beyond U+FFFF (literal or %XX) and control characters: refused
        assertEquals(Arrays.asList("s"), errors(WEB + "s=a😀"));
        assertEquals(Arrays.asList("s"), errors(WEB + "s=a%F0%9F%98%80"));
        assertEquals(Arrays.asList("s"), errors(WEB + "s=a%00b"));
        assertEquals(Arrays.asList("s"), errors(WEB + "s=a%09b"));
        // A literal letter and its %XX form are the same
        assertEquals("пароль", value(WEB + "s=пароль", CamexTunnel.PSK));
    }

    @Test
    public void literalSupplementaryCharacterKeptWhole() {
        assertEquals("😀", ConnectLink.decode("😀"));
        assertEquals("😀", ConnectLink.decode("%F0%9F%98%80"));
        assertNull(ConnectLink.decode("a\uD83Db"));   // a lone surrogate
    }

    @Test
    public void plusStaysInKeys() {
        ConnectLink c = parse(WEB + "k=" + KEY + "&s=base64:" + KEY);
        assertEquals(KEY, c.values.get(CamexTunnel.PUBKEY));
        assertEquals("base64:" + KEY, c.values.get(CamexTunnel.PSK));
    }

    @Test
    public void utf8Psk() {
        assertEquals("пароль", value(WEB + "s=%D0%BF%D0%B0%D1%80%D0%BE%D0%BB%D1%8C",
                CamexTunnel.PSK));
    }

    @Test
    public void badFieldsRejectedOneByOne() {
        ConnectLink c = parse(WEB + "h=a%20b&p=70000&t=sctp&k=short&s=%09x&n=a%09b"
            + "&m=100&a=-1&r=yes&x=later");
        assertTrue(c.values.isEmpty());
        assertEquals(Arrays.asList("h", "p", "t", "k", "s", "n", "m", "a", "r"),
                Arrays.asList(c.errors.keySet().toArray()));
    }

    @Test
    public void badEscapesAndBytes() {
        ConnectLink c = parse(WEB + "s=%zz&h=%FF&p=5800");
        assertEquals(Arrays.asList("s", "h"), Arrays.asList(c.errors.keySet().toArray()));
        // no host: no server, though the port was fine
        assertNull(c.values.get(CamexTunnel.SERVER));
        assertNull(ConnectLink.decode("%4"));
    }

    @Test
    public void pskLimits() {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < 64; i++) b.append("%D0%B9");   // 128 bytes
        assertFalse(parse(WEB + "s=" + b).errors.isEmpty());
        assertFalse(parse(WEB + "s=a%0Ab").errors.isEmpty());
        assertFalse(parse(WEB + "s=").errors.isEmpty());
        // camex takes a passphrase as it is: blanks at its ends stay
        assertEquals(" x ", value(WEB + "s=%20x%20", CamexTunnel.PSK));
    }

    @Test
    public void keysInCamexForms() {
        String zero = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
        assertEquals(Arrays.asList("k"), errors(WEB + "k=" + zero));
        assertFalse(Values.isKey(zero));
        assertEquals(Arrays.asList("s"), errors(WEB + "s=hex:bad"));
        StringBuilder z = new StringBuilder("hex:");
        for (int i = 0; i < 64; i++) z.append('0');
        assertEquals(Arrays.asList("s"), errors(WEB + "s=" + z));
        assertEquals(Arrays.asList("s"), errors(WEB + "s=base64:" + zero));
        assertEquals(Arrays.asList("s"), errors(WEB + "s=base64:short"));
        String hex = "hex:5756dc482cd211054279c9054dcf1783430954058c0f5a5c09c6842fb76e3fbd";
        assertEquals(hex, value(WEB + "s=" + hex.replace(":", "%3A"), CamexTunnel.PSK));
    }

    @Test
    public void clientNamesAsCamexTakesThem() {
        assertEquals("cam@1", value(WEB + "n=cam%401", CamexTunnel.NAME));
        assertEquals("камера", value(WEB + "n=%D0%BA%D0%B0%D0%BC%D0%B5%D1%80%D0%B0",
                CamexTunnel.NAME));
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < 16; i++) b.append("%D0%B9");   // 32 bytes
        assertEquals(Arrays.asList("n"), errors(WEB + "n=" + b));
        assertEquals(Arrays.asList("n"), errors(WEB + "n=a%09b"));
        assertEquals(Arrays.asList("n"), errors(WEB + "n=%20a"));
    }

    @Test
    public void notCamexLinks() {
        assertNull(parse(null));
        assertNull(parse("https://example.org/camex/connect/#h=x"));
        assertNull(parse("https://openipc.github.io/camex/connect/"));
        assertNull(parse("camex://other?h=x"));
        assertTrue(parse(WEB).values.isEmpty());
        assertTrue(parse(WEB).errors.isEmpty());
    }

    @Test
    public void lastOfARepeatedFieldWins() {
        assertEquals("a:2", value(WEB + "h=a&p=1&p=2", CamexTunnel.SERVER));
    }

    // The key check decodes base64 itself (no java.util.Base64 before API 26, #158): the same
    // bytes as the JDK for any 32-byte key
    @Test
    public void ownBase64DecoderMatchesTheJdk() {
        Random rnd = new Random(158);
        byte[] key = new byte[32];
        for (int i = 0; i < 1000; i++) {
            rnd.nextBytes(key);
            String text = Base64.getEncoder().encodeToString(key);
            assertArrayEquals(key, Values.base64(text));
        }
        assertEquals(KEY, Base64.getEncoder().encodeToString(Values.base64(KEY)));
        assertEquals(Arrays.asList("k"), errors(WEB + "k="
            + Base64.getEncoder().encodeToString(new byte[32])));
    }
}
