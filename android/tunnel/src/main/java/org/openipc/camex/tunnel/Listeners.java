/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * Listeners.java — who listens on a TCP port, from /proc/net/tcp and /proc/net/tcp6 (#187)
 */
package org.openipc.camex.tunnel;

/**
 * The owner of a TCP port's listening sockets as /proc/net/tcp and /proc/net/tcp6 show them:
 * apps can read them up to Android 9 (API 28); from Android 10 the library asks
 * ConnectivityManager.getConnectionOwnerUid() instead. A line: {@code sl local_address
 * rem_address st ... uid ...}, the address {@code hex-ip:hex-port}, state {@code 0A} = LISTEN,
 * the uid the 8th field.
 *
 * <p>Plain Java (no android.*): tested on the JVM.
 */
final class Listeners {
    private Listeners() {}

    /**
     * Whether {@code uid} owns the TCP port: at least one listening socket on {@code port} in
     * the given tables and all of them {@code uid}'s. Another uid's listener next to ours makes
     * it false: which of them takes a connection is the kernel's choice.
     */
    static boolean ownedBy(int port, int uid, String... tables) {
        boolean found = false;
        for (String table : tables) {
            if (table == null) continue;
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
                if (p != port) continue;
                if (u != uid) return false;
                found = true;
            }
        }
        return found;
    }
}
