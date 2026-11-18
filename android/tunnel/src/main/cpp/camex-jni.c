/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * camex-jni.c — JNI bridge of the tunnel library (#170): camex runs in-process
 * (camex_main) on the worker thread of org.openipc.camex.tunnel.CamexVpnService
 *
 * The TUN device is made by CamexVpnService.establishTun() through a provider
 * callback once the tunnel parameters are known: in auto mode the address, MTU
 * and routes arrive in the server's CONFIG, after camex has registered. The
 * callback runs on the thread of nativeRun(), so the JNIEnv and the service
 * object saved there stay valid.
 *
 * Unlike camex's own app, the app that takes the library stays inside its VPN:
 * libcamex.so is linked with -Wl,--wrap=socket, and __wrap_socket() protects
 * each AF_INET/AF_INET6 socket camex makes (the server connection) from the
 * VPN, on the thread of nativeRun() only.
 *
 * camex keeps process-wide state, so only one run at a time: the service takes
 * the run slot with nativeAcquire() (which also arms the `running` flag), runs
 * camex with nativeRun() and gives the slot back with nativeRelease(). A Stop
 * between acquire and run clears `running`, and nativeRun() does not set it
 * again, so camex exits at once.
 */
#include <errno.h>
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>

#include "camex.h"
#include "client.h"
#include "log.h"
#include "tcpgate.h"
#include "tun.h"

extern int camex_main(int argc, char *argv[]);

static pthread_mutex_t g_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_slot_busy = 0;
static JNIEnv *g_env = NULL;
static jobject g_service = NULL;
static pthread_t g_thread;      /* the thread of nativeRun(), where g_env is valid */

int __real_socket(int domain, int type, int protocol);

int __wrap_socket(int domain, int type, int protocol)
{
    int fd = __real_socket(domain, type, protocol);
    JNIEnv *env = g_env;

    if (fd >= 0 && (domain == AF_INET || domain == AF_INET6) && env != NULL
        && g_service != NULL && pthread_equal(pthread_self(), g_thread)) {
        jclass cls = (*env)->GetObjectClass(env, g_service);
        jmethodID mid = cls ? (*env)->GetMethodID(env, cls, "protect", "(I)Z") : NULL;
        jboolean ok = JNI_FALSE;

        if (mid != NULL) {
            ok = (*env)->CallBooleanMethod(env, g_service, mid, fd);
        }
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            ok = JNI_FALSE;
        }
        if (cls != NULL) {
            (*env)->DeleteLocalRef(env, cls);
        }
        if (!ok) {
            /* An unprotected socket would send the server connection into
             * the tunnel once a route covers the server: camex retries */
            log_message(LOG_WARNING, "socket %d not protected from the VPN", fd);
            close(fd);
            errno = EPERM;
            return -1;
        }
    }
    return fd;
}

/* The gate for new TCP connections from the tunnel (#187): only to the web
 * port, and only when CamexVpnService.ownsListener() finds this app's socket
 * listening there; other apps' servers on the device stay out of reach.
 *
 * It runs on the packet thread: the answer is kept for GATE_CACHE_MS, so a
 * page's burst of connections (or a flood of SYNs) asks Java once, and the
 * refusals are logged at most once per GATE_LOG_S with their count. */
#define GATE_CACHE_MS 2000
#define GATE_LOG_S 60

static long long gate_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int gate_cached = -1;            /* -1 none, else the last answer */
static long long gate_cached_at;
static int gate_cached_port;
static unsigned long gate_refused;      /* refusals since the last log line */
static long long gate_logged_at = -GATE_LOG_S * 1000LL;

static int jni_owns_listener(uint32_t dst, uint16_t dport, uint32_t src, uint16_t sport)
{
    JNIEnv *env = g_env;
    jclass cls;
    jmethodID mid;
    jboolean ok = JNI_FALSE;
    long long now = gate_now_ms();

    if (gate_cached >= 0 && gate_cached_port == dport && now - gate_cached_at < GATE_CACHE_MS) {
        return gate_cached;
    }
    if (env == NULL || g_service == NULL || !pthread_equal(pthread_self(), g_thread)) {
        return 0;
    }
    cls = (*env)->GetObjectClass(env, g_service);
    mid = cls ? (*env)->GetMethodID(env, cls, "ownsListener", "(IIII)Z") : NULL;
    if (mid != NULL) {
        ok = (*env)->CallBooleanMethod(env, g_service, mid, (jint)ntohl(dst), (jint)dport,
                                       (jint)ntohl(src), (jint)sport);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        ok = JNI_FALSE;
    }
    if (cls != NULL) {
        (*env)->DeleteLocalRef(env, cls);
    }
    gate_cached = ok ? 1 : 0;
    gate_cached_port = dport;
    gate_cached_at = now;
    return gate_cached;
}

static int jni_tcp_gate(uint32_t dst, uint16_t dport, uint32_t src, uint16_t sport)
{
    int web = current_config.web_port > 0 && dport == (uint16_t)current_config.web_port;
    long long now;

    if (web && jni_owns_listener(dst, dport, src, sport)) {
        return 1;
    }
    gate_refused++;
    now = gate_now_ms();
    if (now - gate_logged_at >= GATE_LOG_S * 1000LL) {
        log_message(LOG_INFO, "TCP from the tunnel refused (%lu since the last note), "
                    "the last to port %u: %s", gate_refused, (unsigned)dport,
                    web ? "not this app's listener, or none" : "not this app's web port");
        gate_refused = 0;
        gate_logged_at = now;
    }
    return 0;
}

/* Calls CamexVpnService.establishTun(ip, prefix, mtu, routes) -> fd or -1 */
static int jni_tun_provider(const char *local_ip, int prefix_len, int mtu,
                            const char routes[][32], uint8_t route_count,
                            void *ctx)
{
    JNIEnv *env = g_env;
    jclass cls;
    jclass str_cls;
    jmethodID mid;
    jobjectArray jroutes;
    jstring jip;
    jint fd;
    uint8_t i;

    (void)ctx;
    if (env == NULL || g_service == NULL) {
        return -1;
    }

    cls = (*env)->GetObjectClass(env, g_service);
    mid = (*env)->GetMethodID(env, cls, "establishTun",
                              "(Ljava/lang/String;II[Ljava/lang/String;)I");
    if (mid == NULL) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, cls);
        return -1;
    }
    str_cls = (*env)->FindClass(env, "java/lang/String");
    jroutes = (*env)->NewObjectArray(env, route_count, str_cls, NULL);
    for (i = 0; i < route_count; ++i) {
        jstring r = (*env)->NewStringUTF(env, routes[i]);
        (*env)->SetObjectArrayElement(env, jroutes, i, r);
        (*env)->DeleteLocalRef(env, r);
    }
    jip = (*env)->NewStringUTF(env, local_ip);

    fd = (*env)->CallIntMethod(env, g_service, mid, jip,
                               (jint)prefix_len, (jint)mtu, jroutes);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        fd = -1;
    }

    (*env)->DeleteLocalRef(env, jip);
    (*env)->DeleteLocalRef(env, jroutes);
    (*env)->DeleteLocalRef(env, str_cls);
    (*env)->DeleteLocalRef(env, cls);
    return (int)fd;
}

/* Take the run slot and arm camex; JNI_FALSE if another run holds it. */
JNIEXPORT jboolean JNICALL
Java_org_openipc_camex_tunnel_CamexVpnService_nativeAcquire(JNIEnv *env, jclass cls)
{
    jboolean ok = JNI_FALSE;

    (void)env;
    (void)cls;
    pthread_mutex_lock(&g_slot_lock);
    if (!g_slot_busy) {
        g_slot_busy = 1;
        running = 1;
        ok = JNI_TRUE;
    }
    pthread_mutex_unlock(&g_slot_lock);
    return ok;
}

/* Give the run slot back (after nativeRun() returned). */
JNIEXPORT void JNICALL
Java_org_openipc_camex_tunnel_CamexVpnService_nativeRelease(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    pthread_mutex_lock(&g_slot_lock);
    g_slot_busy = 0;
    pthread_mutex_unlock(&g_slot_lock);
}

/* Run camex until it exits; the caller holds the run slot. */
JNIEXPORT jint JNICALL
Java_org_openipc_camex_tunnel_CamexVpnService_nativeRun(JNIEnv *env, jobject thiz,
                                                        jobjectArray jargv)
{
    jsize argc;
    char **argv;
    int i, ret;

    g_env = env;
    g_service = thiz;
    g_thread = pthread_self();
    tun_set_provider(jni_tun_provider, NULL);
    gate_cached = -1;
    tcpgate_set(jni_tcp_gate);
    unsetenv("CAMEX_TUN_FD");

    argc = (*env)->GetArrayLength(env, jargv);
    argv = calloc((size_t)argc + 1, sizeof(char *));
    if (argv == NULL) {
        ret = -1;
        goto out;
    }
    for (i = 0; i < argc; i++) {
        jstring js = (jstring)(*env)->GetObjectArrayElement(env, jargv, i);
        const char *utf = (*env)->GetStringUTFChars(env, js, NULL);

        argv[i] = utf ? strdup(utf) : NULL;
        if (utf) {
            (*env)->ReleaseStringUTFChars(env, js, utf);
        }
        (*env)->DeleteLocalRef(env, js);
        if (argv[i] == NULL) {
            ret = -1;
            goto out;
        }
    }

    /* blocks until camex exits */
    ret = camex_main((int)argc, argv);

out:
    tcpgate_set(NULL);
    tun_set_provider(NULL, NULL);
    g_env = NULL;
    g_service = NULL;
    if (argv != NULL) {
        for (i = 0; i < argc; i++) {
            if (argv[i] != NULL) {
                /* the psk is among them: wipe it */
                memset(argv[i], 0, strlen(argv[i]));
                free(argv[i]);
            }
        }
        free(argv);
    }
    return (jint)ret;
}

/* Whether camex has a session with its server now: cleared when it starts to
 * reconnect, set when the handshake is done; camex stores it atomically. */
JNIEXPORT jboolean JNICALL
Java_org_openipc_camex_tunnel_CamexVpnService_nativeEstablished(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    return __atomic_load_n(&client_state.established, __ATOMIC_ACQUIRE) ? JNI_TRUE : JNI_FALSE;
}

/* Ask camex to stop; nativeRun() returns within about a second. */
JNIEXPORT void JNICALL
Java_org_openipc_camex_tunnel_CamexVpnService_nativeStop(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    running = 0;
}
