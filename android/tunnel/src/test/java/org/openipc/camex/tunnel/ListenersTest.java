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
    private static final String HEAD =
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n";
    // 0.0.0.0:8085 LISTEN by 10123; 127.0.0.1:2021 LISTEN by 10200; an established 8085 by 10200
    private static final String TCP = HEAD
        + "   0: 00000000:1F95 00000000:0000 0A 00000000:00000000 00:00000000 00000000 10123        0 1111 1 0 100 0 0 10 0\n"
        + "   1: 0100007F:07E5 00000000:0000 0A 00000000:00000000 00:00000000 00000000 10200        0 2222 1 0 100 0 0 10 0\n"
        + "   2: 0C00C80A:1F95 0100C80A:A028 01 00000000:00000000 00:00000000 00000000 10200        0 3333 1 0 20 4 30 10 -1\n";
    // :::8086 LISTEN by 10300
    private static final String TCP6 =
        "  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
        + "   0: 00000000000000000000000000000000:1F96 00000000000000000000000000000000:0000 0A 00000000:00000000 00:00000000 00000000 10300        0 4444 1 0 100 0 0 10 0\n";

    @Test public void ownListener() {
        assertTrue(Listeners.ownedBy(8085, 10123, TCP, TCP6));
        assertTrue(Listeners.ownedBy(8086, 10300, TCP, TCP6));
    }

    @Test public void anotherAppsListener() {
        assertFalse(Listeners.ownedBy(8085, 10200, TCP, TCP6));   // only an established socket is ours
        assertFalse(Listeners.ownedBy(2021, 10123, TCP, TCP6));
    }

    @Test public void nobodyListens() {
        assertFalse(Listeners.ownedBy(80, 10123, TCP, TCP6));
        assertFalse(Listeners.ownedBy(8085, 10123));
        assertFalse(Listeners.ownedBy(8085, 10123, (String) null));
    }

    @Test public void oursAndAnothers() {
        String both = TCP + "   3: 0C00C80A:1F95 00000000:0000 0A 00000000:00000000 00:00000000 00000000 10999        0 5555 1 0 100 0 0 10 0\n";
        assertFalse(Listeners.ownedBy(8085, 10123, both));
    }
}
