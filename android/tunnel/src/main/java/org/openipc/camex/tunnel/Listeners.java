/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * Listeners.java — who listens on a TCP port, from /proc/net/tcp and /proc/net/tcp6 (#187)
 */
package org.openipc.camex.tunnel;

/**
 * The owner of the TCP listeners that could take a connection, as /proc/net/tcp and
 * /proc/net/tcp6 show them: apps can read them up to Android 9 (API 28); from Android 10 the
 * library asks ConnectivityManager.getConnectionOwnerUid() instead. A line: {@code sl
 * local_address rem_address st ... uid ...}, the address {@code hex-ip:hex-port} with the IPv4
 * address (and each 32-bit word of an IPv6 one) in the kernel's little-endian order, state
 * {@code 0A} = LISTEN, the uid the 8th field.
 *
 * <p>Plain Java (no android.*): tested on the JVM.
 */
final class Listeners {
    private Listeners() {}

    /**
     * Whether {@code uid} owns the listeners that could take a TCP connection to the IPv4
     * address {@code dst} (host byte order) and {@code port}: those bound to {@code dst} or to
     * any address, IPv4 or IPv6 ({@code ::}, {@code ::ffff:dst}). At least one, and all of them
     * {@code uid}'s: which one takes it is the kernel's choice. A table that could not be read
     * (null) makes it false: nothing is decided from half the sockets.
     */
    static boolean ownedBy(int port, int dst, int uid, String tcp, String tcp6) {
        if (tcp == null || tcp6 == null) return false;
        String me = hex(Integer.reverseBytes(dst));
        boolean found = false;
        for (String table : new String[]{tcp, tcp6}) {
            for (String line : table.split("\n")) {
                String[] f = line.trim().split("\\s+");
                if (f.length < 8 || !"0A".equalsIgnoreCase(f[3])) continue;
                int colon = f[1].lastIndexOf(':');
                if (colon < 0) continue;
                int p, u;
                try {
                    p = Integer.parseInt(f[1].substring(colon + 1), 16);
                    u = Integer.parseInt(f[7]);
                } catch (NumberFormatException e) {
                    continue;   // the header line
                }
                if (p != port || !reaches(f[1].substring(0, colon).toUpperCase(), me)) continue;
                if (u != uid) return false;
                found = true;
            }
        }
        return found;
    }

    /** Whether a listener on this address (as in the table) takes IPv4 {@code me}'s traffic. */
    private static boolean reaches(String addr, String me) {
        if (addr.length() == 8) return addr.equals("00000000") || addr.equals(me);
        if (addr.length() != 32) return false;
        if (addr.equals("00000000000000000000000000000000")) return true;     // ::
        return addr.startsWith("0000000000000000FFFF0000") && addr.substring(24).equals(me);
    }

    private static String hex(int v) {
        return String.format("%08X", v);
    }
}
