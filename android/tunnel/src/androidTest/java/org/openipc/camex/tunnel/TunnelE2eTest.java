/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * TunnelE2eTest.java — the library on a device (#170), driven by tests/android/tunnel-e2e.sh:
 * a camex server and a client "peer" in Docker, this test the second client
 */
package org.openipc.camex.tunnel;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.RouteInfo;
import android.os.Bundle;
import android.util.Log;

import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.Assume;
import org.junit.Test;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

public class TunnelE2eTest {
    private static final String TAG = "CAMEX_E2E";

    private final Bundle args = InstrumentationRegistry.getArguments();
    private final Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();

    private Map<String, String> values() {
        Map<String, String> v = new HashMap<>();
        v.put(CamexTunnel.SERVER, args.getString("server"));
        v.put(CamexTunnel.NAME, args.getString("name", "app"));
        v.put(CamexTunnel.TRANSPORT, args.getString("transport", "UDP"));
        v.put(CamexTunnel.PUBKEY, args.getString("pubkey"));
        v.put(CamexTunnel.PSK, args.getString("psk"));
        return v;
    }

    /** Waits for the state; fails with the last one after the timeout. */
    private void await(CamexTunnel.State want, int seconds) throws InterruptedException {
        await(want, null, seconds);
    }

    /** Waits for the state with an address other than notAddress (null: any). */
    private void await(CamexTunnel.State want, String notAddress, int seconds)
            throws InterruptedException {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<String> last = new AtomicReference<>("");
        CamexTunnel.Listener l = (s, address, error) -> {
            last.set(s + " " + address + " " + error);
            Log.i(TAG, "state " + last.get());
            if (s == want && (notAddress == null || !notAddress.equals(address))) done.countDown();
        };
        CamexTunnel.addListener(l);
        try {
            if (!done.await(seconds, TimeUnit.SECONDS)) {
                fail("no " + want + " in " + seconds + " s, last: " + last.get());
            }
        } finally {
            CamexTunnel.removeListener(l);
        }
    }

    /** step=consent: no VPN consent (appops ignore) — no crash, NEEDS_CONSENT, no service. */
    @Test
    public void consent() throws Exception {
        Assume.assumeTrue("consent".equals(args.getString("step")));
        assertNull(CamexTunnel.check(values()));
        assertNotNull(CamexTunnel.consentIntent(context));
        assertFalse(CamexTunnel.start(context, values()));
        await(CamexTunnel.State.NEEDS_CONSENT, 5);
        assertFalse(CamexTunnel.running());
        assertEquals("VPN consent needed", CamexTunnel.lastError());
        Log.i(TAG, "consent OK");
    }

    /**
     * step=dialog: shows the system's consent dialog for the script to confirm, where adb
     * cannot give the consent (Android 5.0 has no appops ACTIVATE_VPN).
     */
    @Test
    public void dialog() {
        Assume.assumeTrue("dialog".equals(args.getString("step")));
        boolean show = CamexTunnel.consentIntent(context) != null;
        if (show) {
            context.startActivity(new android.content.Intent(context, ConsentActivity.class)
                    .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK));
        }
        Log.i(TAG, "dialog " + show);
    }

    /** step=bad: bad values — FAILED, the reason names the field, never the psk. */
    @Test
    public void badValues() throws Exception {
        Assume.assumeTrue("bad".equals(args.getString("step")));
        Map<String, String> v = values();
        v.put(CamexTunnel.SERVER, "no-port");
        assertFalse(CamexTunnel.start(context, v));
        await(CamexTunnel.State.FAILED, 5);
        assertTrue(CamexTunnel.lastError().startsWith("bad server"));
        assertFalse(CamexTunnel.lastError().contains(v.get(CamexTunnel.PSK)));
        Log.i(TAG, "bad OK");
    }

    /**
     * step=run: CONNECTED with an address of the server's pool (or the one named); the VPN has the tunnel subnet and the
     * server's routes but no default route; the peer is reached over TCP through the tunnel;
     * the peer reaches this app's listening socket (0.0.0.0:port) through it; new values restart
 * it; a lost server gives CONNECTING and CONNECTED again when it is back; stop() ends it, a
 * start after it works, and a stop right after a start wins.
     */
    @Test
    public void run() throws Exception {
        Assume.assumeTrue("run".equals(args.getString("step")));
        String expect = args.getString("address");
        String peer = args.getString("peer");
        int listen = Integer.parseInt(args.getString("listen", "8080"));

        // the app's listening socket, as microsnander's web page: answers "app-ok"
        ServerSocket server = new ServerSocket();
        server.setReuseAddress(true);
        server.bind(new InetSocketAddress("0.0.0.0", listen));
        CountDownLatch incoming = new CountDownLatch(1);
        AtomicReference<String> from = new AtomicReference<>("");
        Thread accept = new Thread(() -> {
            try (Socket s = server.accept()) {
                from.set(s.getInetAddress().getHostAddress());
                OutputStream o = s.getOutputStream();
                o.write("app-ok\n".getBytes(StandardCharsets.US_ASCII));
                o.flush();
                incoming.countDown();
            } catch (Exception e) {
                Log.w(TAG, "accept: " + e);
            }
        });
        accept.start();

        assertNull(CamexTunnel.consentIntent(context));
        assertTrue(CamexTunnel.start(context, values()));
        await(CamexTunnel.State.CONNECTED, 90);
        // the address the server gave: its auto pool, or the one named
        if (expect != null) assertEquals(expect, CamexTunnel.address());
        else assertTrue(CamexTunnel.address(), CamexTunnel.address().startsWith(args.getString("pool", "10.200.0.")));
        assertTrue(CamexTunnel.running());
        String me = CamexTunnel.address();
        Log.i(TAG, "connected " + CamexTunnel.address());

        // the VPN's routes: the subnet and the server's, the one with host bits as its network,
        // never a default route
        // the system registers the VPN network a moment after establish()
        List<String> routes = vpnRoutes();
        for (int i = 0; i < 20 && routes.isEmpty(); i++) {
            Thread.sleep(500);
            routes = vpnRoutes();
        }
        Log.i(TAG, "routes " + routes);
        for (String r : routes) assertFalse("default route " + r, r.endsWith("/0"));
        for (String want : args.getString("routes", "").split(",")) {
            if (!want.isEmpty()) assertTrue("no route " + want + " in " + routes, routes.contains(want));
        }

        // out: the peer through the tunnel
        assertEquals("peer-ok", askPeer(peer));
        Log.i(TAG, "outgoing OK");

        // in: the host script makes the peer connect to this address and port
        Log.i(TAG, "listening " + me + ":" + listen);
        assertTrue("nobody came to " + me + ":" + listen,
                incoming.await(90, TimeUnit.SECONDS));
        Log.i(TAG, "incoming OK from " + from.get());
        server.close();

        // new values while it runs: camex again in the same process (its options read anew),
        // a new name gets a new address from the pool
        Map<String, String> v2 = values();
        v2.put(CamexTunnel.NAME, "app2");
        assertTrue(CamexTunnel.start(context, v2));
        await(CamexTunnel.State.CONNECTED, me, 90);
        assertEquals("peer-ok", askPeer(peer));
        Log.i(TAG, "restart OK " + CamexTunnel.address());

        // the server goes away (the script stops it): CONNECTING, the VPN stays; back: CONNECTED
        Log.i(TAG, "server-loss ready");
        await(CamexTunnel.State.CONNECTING, 120);
        Log.i(TAG, "lost OK: " + CamexTunnel.lastError());
        await(CamexTunnel.State.CONNECTED, 180);
        assertEquals("peer-ok", askPeer(peer));
        Log.i(TAG, "back OK " + CamexTunnel.address());

        CamexTunnel.stop();
        await(CamexTunnel.State.STOPPED, 15);
        assertFalse(CamexTunnel.running());

        // a start after a stop, then a stop at once: the stop wins over the start not yet run
        assertTrue(CamexTunnel.start(context, values()));
        await(CamexTunnel.State.CONNECTED, 90);
        if (android.os.Build.VERSION.SDK_INT > 21) {
            assertEquals("peer-ok", askPeer(peer));
            Log.i(TAG, "start after stop OK");
        } else {
            // Android 5.0 sometimes routes the app's own sockets outside a VPN made again in
            // one process (OpenIPC/camex#173): in seconds, or not at all. Recorded, not failed
            long t0 = android.os.SystemClock.elapsedRealtime();
            String r = null;
            while (r == null && android.os.SystemClock.elapsedRealtime() - t0 < 60000) {
                r = askPeer(peer);
            }
            long s = (android.os.SystemClock.elapsedRealtime() - t0) / 1000;
            if ("peer-ok".equals(r)) {
                Log.i(TAG, "start after stop OK (after " + s + " s on Android 5.0)");
            } else {
                Log.i(TAG, "start after stop: peer not reached in " + s + " s on Android 5.0 (#173)");
            }
        }
        CamexTunnel.stop();
        await(CamexTunnel.State.STOPPED, 15);
        assertTrue(CamexTunnel.start(context, values()));
        CamexTunnel.stop();
        Thread.sleep(5000);
        assertEquals(CamexTunnel.State.STOPPED, CamexTunnel.state());
        Log.i(TAG, "quick stop OK");
        // the VPN is gone
        for (int i = 0; i < 20 && !vpnRoutes().isEmpty(); i++) Thread.sleep(500);
        assertTrue("the VPN is still up", vpnRoutes().isEmpty());
        Log.i(TAG, "stop OK");
    }

    // "peer-ok" from the peer's TCP server, through the tunnel; null when it never answers
    private static String askPeer(String peer) throws InterruptedException {
        String[] hp = peer.split(":");
        for (int i = 0; i < 20; i++) {
            try (Socket s = new Socket()) {
                s.connect(new InetSocketAddress(hp[0], Integer.parseInt(hp[1])), 3000);
                s.setSoTimeout(5000);
                return new BufferedReader(new InputStreamReader(s.getInputStream(),
                        StandardCharsets.US_ASCII)).readLine();
            } catch (Exception e) {
                Log.i(TAG, "peer not yet: " + e);
                Thread.sleep(1000);
            }
        }
        return null;
    }

    // The routes of the VPN network (empty when there is none)
    @SuppressWarnings("deprecation")
    private List<String> vpnRoutes() {
        ConnectivityManager cm = (ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
        List<String> out = new ArrayList<>();
        for (Network n : cm.getAllNetworks()) {
            NetworkCapabilities c = cm.getNetworkCapabilities(n);
            if (c == null || !c.hasTransport(NetworkCapabilities.TRANSPORT_VPN)) continue;
            LinkProperties lp = cm.getLinkProperties(n);
            if (lp == null) continue;
            for (RouteInfo r : lp.getRoutes()) out.add(r.getDestination().toString());
        }
        return out;
    }
}
