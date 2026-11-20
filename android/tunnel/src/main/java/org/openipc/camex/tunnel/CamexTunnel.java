/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * CamexTunnel.java — the camex tunnel for apps that build it in (#170): settings, VPN
 * consent, start and stop, state
 */
package org.openipc.camex.tunnel;

import android.app.Notification;
import android.content.Context;
import android.content.Intent;
import android.net.VpnService;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;

import java.util.List;
import java.util.Map;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicLong;

/**
 * camex (OpenIPC/camex) as a client in auto mode inside the app: the server gives the
 * address, MTU and routes; the app joins the server's network and reaches its other clients,
 * and they reach the app's listening sockets, through the tunnel.
 *
 * <p>The app stays inside its own VPN: camex's sockets are protected from it, and only the
 * tunnel subnet and the server's routes go through it, never a default route.
 *
 * <p>No Activity and no stored settings: the app keeps its settings, gets the VPN consent
 * ({@link #consentIntent}, or {@code adb shell appops set <package> ACTIVATE_VPN allow} on an
 * unattended device) and calls {@link #start} — from an Activity, a foreground service or a
 * BOOT_COMPLETED receiver. Once started, the tunnel keeps reconnecting until {@link #stop}.
 */
public final class CamexTunnel {
    /** camex's version (src/version.h): the library's version too. */
    public static final String VERSION = BuildConfig.CAMEX_VERSION;

    /** Keys of the values Map (see {@link #fields()}). */
    public static final String SERVER = Values.SERVER;
    public static final String NAME = Values.NAME;
    public static final String TRANSPORT = Values.TRANSPORT;
    public static final String PUBKEY = Values.PUBKEY;
    public static final String NETWORK = Values.NETWORK;
    public static final String PSK = Values.PSK;
    public static final String MTU = Values.MTU;
    public static final String KEEPALIVE = Values.KEEPALIVE;
    /** Seconds without an answer before camex reconnects (optional, default 20). */
    public static final String SERVER_TIMEOUT = Values.SERVER_TIMEOUT;
    /**
     * The port of the app's web page, for nginx on the server (optional, #174): nginx
     * reaches the app's name there instead of port 80. Needs a server 3.19.0 or later.
     */
    public static final String WEB_PORT = Values.WEB_PORT;

    public enum State {
        /** not started, or stopped by {@link #stop} */
        STOPPED,
        /** connecting, or reconnecting after a loss (the tunnel retries by itself) */
        CONNECTING,
        /** the tunnel is up: {@link #address()} is the app's address in it */
        CONNECTED,
        /** no VPN consent (never given, or revoked): give it and call {@link #start} again */
        NEEDS_CONSENT,
        /** the values were refused, or the VPN could not be made: {@link #lastError()} */
        FAILED,
    }

    /** Called on the main thread on every change, and once at once when added. */
    public interface Listener {
        void onState(State state, String address, String lastError);
    }

    /**
     * Makes the ongoing notification of the tunnel's foreground service. Called on the main
     * thread on each state change; the notification is posted with {@link #setNotification}'s
     * id. On Android 8+ the notification's channel must exist (the app creates it).
     */
    public interface NotificationFactory {
        Notification create(Context context, State state, String address, String lastError);
    }

    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static final List<Listener> LISTENERS = new CopyOnWriteArrayList<>();
    // The latest start; a stop or a refused start moves it on, so a start the service has not
    // seen yet is dropped when it arrives
    private static final AtomicLong TOKEN = new AtomicLong();
    private static final Object START_LOCK = new Object();

    // Read anywhere; written in post(), under STATE_LOCK
    private static final Object STATE_LOCK = new Object();
    private static volatile State state = State.STOPPED;
    private static volatile String address = "";
    private static volatile String lastError = "";

    static volatile CharSequence notificationTitle = "camex";
    static volatile int notificationIcon = 0;   // 0: the library's icon
    static volatile int notificationId = 0x63616d;   // "cam"
    static volatile NotificationFactory notificationFactory;

    private CamexTunnel() { }

    /** The settings, in the order of a settings screen. */
    public static List<Field> fields() {
        return Values.FIELDS;
    }

    /**
     * The settings with the client name starting as {@link #defaultName(Context, String)}
     * {@code (context, app)}: the app gives its own name (camex, vectis, microsnander,
     * decoder). The user may change it; an empty name is not saved (the app keeps or
     * restores the default).
     */
    public static List<Field> fields(Context context, String app) {
        return Values.fields(defaultName(context, app));
    }

    /**
     * The client name of a new setting: the app's name and the last 4 characters of the
     * device's ANDROID_ID, as {@code vectis-3f2a}, so the same app on two devices gets two
     * names and two apps on one device too.
     */
    @SuppressWarnings("HardwareIds")
    public static String defaultName(Context context, String app) {
        String id = android.provider.Settings.Secure.getString(context.getContentResolver(),
                android.provider.Settings.Secure.ANDROID_ID);
        return Values.defaultName(app, id);
    }

    /**
     * Checks the values: null when they are fine, otherwise {@code {key, reason}} of the first
     * wrong field.
     */
    public static String[] check(Map<String, String> values) {
        return Values.check(values);
    }

    /**
     * The system's VPN consent dialog to show (startActivityForResult), or null when the app
     * has the consent already (VpnService.prepare).
     */
    public static Intent consentIntent(Context context) {
        return VpnService.prepare(context.getApplicationContext());
    }

    /**
     * Starts the tunnel with these values, or restarts it with new ones. Never throws for a
     * missing consent or bad values: the state says it ({@link State#NEEDS_CONSENT},
     * {@link State#FAILED} with {@link #lastError()}) and false is returned; the app may call
     * start again later (a consent given over adb meanwhile). Nothing is stored: after the
     * process is gone, the app starts the tunnel again.
     *
     * @return true when the tunnel service was started
     */
    public static boolean start(Context context, Map<String, String> values) {
        Context app = context.getApplicationContext();
        String[] bad = Values.check(values);
        if (bad != null) {
            // a running tunnel stops: it is not what the app asks for now; the reason names
            // the field, never its value (a psk)
            refuse(0, State.FAILED, "bad " + bad[0] + ": " + bad[1]);
            return false;
        }
        if (VpnService.prepare(app) != null) {
            refuse(0, State.NEEDS_CONSENT, "VPN consent needed");
            return false;
        }
        long token;
        // the token and the state move together: a stop() meanwhile comes after both
        synchronized (START_LOCK) {
            token = TOKEN.incrementAndGet();
            post(State.CONNECTING, "", "");
        }
        Intent i = new Intent(app, CamexVpnService.class)
                .setAction(CamexVpnService.ACTION_START)
                .putExtra(CamexVpnService.EXTRA_ARGV, Values.argv(values))
                .putExtra(CamexVpnService.EXTRA_TOKEN, token);
        try {
            if (Build.VERSION.SDK_INT >= 26) app.startForegroundService(i);
            else app.startService(i);
        } catch (RuntimeException e) {
            // e.g. a start from the background the system does not allow; a running tunnel
            // with the old values stops as well
            refuse(token, State.FAILED, "cannot start the service: " + e.getClass().getSimpleName());
            return false;
        }
        return true;
    }

    /** Stops the tunnel and its reconnects; the state becomes STOPPED. */
    public static void stop() {
        refuse(0, State.STOPPED, "");
    }

    /**
     * Cancels the starts so far, sets the state and stops the running tunnel; the token and
     * the state move together under START_LOCK. With a token, only when no newer start came.
     */
    private static void refuse(long token, State s, String error) {
        synchronized (START_LOCK) {
            if (token != 0 && TOKEN.get() != token) return;   // a newer start stands
            TOKEN.incrementAndGet();
            post(s, "", error);
            // in the lock: a start after this one is not stopped by it
            CamexVpnService.stopTunnel();
        }
    }

    /** The number of the latest start: a start with another number is stale. */
    static long token() {
        return TOKEN.get();
    }

    /** True from {@link #start} to {@link #stop}, while the tunnel connects or is up. */
    public static boolean running() {
        State s = state;
        return s == State.CONNECTING || s == State.CONNECTED;
    }

    public static State state() {
        return state;
    }

    /** The app's address in the tunnel while CONNECTED, otherwise "". */
    public static String address() {
        return address;
    }

    /** A short reason of the last failure or reconnect, "" when there is none. */
    public static String lastError() {
        return lastError;
    }

    public static void addListener(Listener l) {
        if (l == null) return;
        LISTENERS.add(l);
        MAIN.post(() -> {
            if (LISTENERS.contains(l)) l.onState(state, address, lastError);
        });
    }

    public static void removeListener(Listener l) {
        LISTENERS.remove(l);
    }

    /**
     * The look of the library's own notification: its title (default "camex") and small icon
     * (0: the library's). Set before {@link #start}.
     */
    public static void setNotificationLook(CharSequence title, int smallIcon) {
        notificationTitle = title == null ? "camex" : title;
        notificationIcon = smallIcon;
    }

    /**
     * The app's own notification instead of the library's: posted with this id. An app that
     * runs its own foreground service passes that service's id, and the two share one
     * notification. Null factory: the library's notification again. Set before {@link #start}.
     */
    public static void setNotification(int id, NotificationFactory factory) {
        notificationId = id;
        notificationFactory = factory;
    }

    /**
     * A new state: the getters have it at once; the listeners and the notification get it on
     * the main thread, in the order of the posts.
     */
    static void post(State s, String addr, String error) {
        synchronized (STATE_LOCK) {
            state = s;
            address = addr == null ? "" : addr;
            if (error != null) lastError = error;
            final State st = state;
            final String a = address;
            final String e = lastError;
            MAIN.post(() -> {
                CamexVpnService.onState();
                for (Listener l : LISTENERS) l.onState(st, a, e);
            });
        }
    }
}
