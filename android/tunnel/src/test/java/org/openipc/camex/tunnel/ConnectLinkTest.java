/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * ConnectLinkTest.java — camex links to the library's values (#170)
 */
package org.openipc.camex.tunnel;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.util.Arrays;
import java.util.Map;

public class ConnectLinkTest {
    private static final String KEY = ValuesTest.KEY;
    private static final String PSK = ValuesTest.PSK;

    @Test
    public void webLink() {
        ConnectLink c = ConnectLink.parse("https://openipc.github.io/camex/connect/#h=vpn.example.org"
                + "&p=5800&t=tcp&k=" + KEY + "&s=" + PSK + "&n=box%201&m=1400&a=15&r=1");
        Map<String, String> v = c.values;
        assertEquals("vpn.example.org:5800", v.get(CamexTunnel.SERVER));
        assertEquals("TCP", v.get(CamexTunnel.TRANSPORT));
        assertEquals(KEY, v.get(CamexTunnel.PUBKEY));
        assertEquals(PSK, v.get(CamexTunnel.PSK));
        assertEquals("box 1", v.get(CamexTunnel.NAME));
        assertEquals("1400", v.get(CamexTunnel.MTU));
        assertEquals("15", v.get(CamexTunnel.KEEPALIVE));
        assertTrue(c.errors.isEmpty());
        // the order of the fields
        assertEquals(Arrays.asList("server", "name", "transport", "pubkey", "psk", "mtu", "keepalive"),
                Arrays.asList(v.keySet().toArray()));
        // a complete link is good values
        assertNull(CamexTunnel.check(v));
    }

    @Test
    public void schemeLink() {
        ConnectLink c = ConnectLink.parse("camex://connect?h=10.0.0.1&t=udp&k=" + KEY);
        assertEquals("10.0.0.1:5800", c.values.get(CamexTunnel.SERVER));
        assertEquals("UDP", c.values.get(CamexTunnel.TRANSPORT));
        assertNull(c.values.get(CamexTunnel.PSK));
        assertTrue(ConnectLink.parse("camex://connect/#p=1").errors.containsKey("p"));
    }

    @Test
    public void notCamex() {
        assertNull(ConnectLink.parse(null));
        assertNull(ConnectLink.parse("https://example.org/#h=x"));
        assertNull(ConnectLink.parse("https://openipc.github.io/camex/connect/?h=x"));
        assertNull(ConnectLink.parse("camex://other?h=x"));
    }

    @Test
    public void errors() {
        ConnectLink c = ConnectLink.parse("https://openipc.github.io/camex/connect#h=bad_host&p=99999"
                + "&t=quic&k=short&s=hex:Secret&n=&m=1&a=9999&r=2&z=later&s2=x");
        assertEquals(Arrays.asList("h", "p", "t", "k", "s", "n", "m", "a", "r"),
                Arrays.asList(c.errors.keySet().toArray()));
        assertTrue(c.values.isEmpty());
        // a reason never holds the value (a psk)
        assertTrue(!c.errors.get("s").contains("Secret"));
    }

    @Test
    public void decoding() {
        assertEquals("a+b c", ConnectLink.decode("a+b%20c"));
        assertEquals("ключ", ConnectLink.decode("%D0%BA%D0%BB%D1%8E%D1%87"));
        assertNull(ConnectLink.decode("%zz"));
        assertNull(ConnectLink.decode("%C3"));
        assertTrue(ConnectLink.parse("camex://connect?n=%zz").errors.containsKey("n"));
        // the last of a repeated field wins
        assertEquals("b:5800", ConnectLink.parse("camex://connect?h=a&h=b").values.get("server"));
    }
}
