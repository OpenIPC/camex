/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * ListenersTest.java — the owner of a listening port from /proc/net/tcp{,6} (#187)
 */
package org.openipc.camex.tunnel;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class ListenersTest {
    // the tunnel address 10.200.0.12, as an int in host byte order
    private static final int ME = (10 << 24) | (200 << 16) | 12;

    private static final String HEAD =
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n";
    private static final String HEAD6 =
        "  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n";

    private static String v4(String addr, int port, String st, int uid) {
        return String.format("   0: %s:%04X 00000000:0000 %s 00000000:00000000 00:00000000 00000000 %5d        0 1 1 0 100 0 0 10 0\n",
            addr, port, st, uid);
    }

    private static String v6(String addr, int port, int uid) {
        return String.format("   0: %s:%04X 00000000000000000000000000000000:0000 0A 00000000:00000000 00:00000000 00000000 %5d        0 1 1 0 100 0 0 10 0\n",
            addr, port, uid);
    }

    // 0.0.0.0:8085 LISTEN by 10123; 127.0.0.1:2021 by 10200; an established 10.200.0.12:8085 by 10200
    private static final String TCP = HEAD
        + v4("00000000", 8085, "0A", 10123)
        + v4("0100007F", 2021, "0A", 10200)
        + v4("0C00C80A", 8085, "01", 10200);
    // :::8086 LISTEN by 10300
    private static final String TCP6 = HEAD6 + v6("00000000000000000000000000000000", 8086, 10300);

    @Test public void ownListener() {
        assertTrue(Listeners.ownedBy(8085, ME, 10123, TCP, TCP6));
        assertTrue(Listeners.ownedBy(8086, ME, 10300, TCP, TCP6));   // :: takes IPv4 too
    }

    @Test public void anotherAppsListener() {
        assertFalse(Listeners.ownedBy(8085, ME, 10200, TCP, TCP6));   // only an established socket is its
        assertFalse(Listeners.ownedBy(8086, ME, 10123, TCP, TCP6));
    }

    @Test public void nobodyListens() {
        assertFalse(Listeners.ownedBy(80, ME, 10123, TCP, TCP6));
        assertFalse(Listeners.ownedBy(2021, ME, 10200, TCP, TCP6));   // 127.0.0.1 only: not reachable
    }

    @Test public void aTableNotRead() {
        assertFalse(Listeners.ownedBy(8085, ME, 10123, TCP, null));
        assertFalse(Listeners.ownedBy(8086, ME, 10300, null, TCP6));
    }

    @Test public void oursAndAnothers() {
        // another app on the tunnel address itself, or on ::ffff:10.200.0.12, or on ::
        assertFalse(Listeners.ownedBy(8085, ME, 10123, TCP + v4("0C00C80A", 8085, "0A", 10999), TCP6));
        assertFalse(Listeners.ownedBy(8085, ME, 10123, TCP,
            TCP6 + v6("0000000000000000FFFF00000C00C80A", 8085, 10999)));
        assertFalse(Listeners.ownedBy(8085, ME, 10123, TCP,
            TCP6 + v6("00000000000000000000000000000000", 8085, 10999)));
    }

    @Test public void othersOnOtherAddresses() {
        // another app on 127.0.0.1, on another IPv4 address, on an IPv6 address: not in the way
        String tcp = TCP + v4("0100007F", 8085, "0A", 10999) + v4("0D00C80A", 8085, "0A", 10999);
        String tcp6 = TCP6 + v6("000080FE00000000FF0000000100FE00", 8085, 10999)
            + v6("0000000000000000FFFF00000100007F", 8085, 10999);
        assertTrue(Listeners.ownedBy(8085, ME, 10123, tcp, tcp6));
    }
}
