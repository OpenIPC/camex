/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * camex-jni.c — JNI bridge for camex Android VpnService
 *
 * camex runs in-process (camex_main) on the service's worker thread.
 *
 * The TUN device is created by TunnelService through a provider callback
 * once the tunnel parameters are known: in auto mode the address, MTU and
 * routes arrive in the server's CONFIG, after camex has registered. The
 * callback runs on the same thread as nativeRun(), so the JNIEnv and the
 * service object saved there stay valid.
 *
 * Legacy path: a non-negative tun_fd is passed through CAMEX_TUN_FD.
 *
 * camex keeps process-wide state, so only one run at a time: a service
 * instance takes the run slot with nativeAcquire() (which also arms the
 * `running` flag), runs camex with nativeRun() and gives the slot back with
 * nativeRelease(). A Stop that arrives between acquire and run clears
 * `running`, and nativeRun() does not set it again, so camex exits at once.
 */
#include <jni.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "camex.h"
#include "tun.h"

/* camex entry point */
extern int camex_main(int argc, char *argv[]);

static pthread_mutex_t g_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_slot_busy = 0;
static JNIEnv *g_env = NULL;
static jobject g_service = NULL;

/* Calls TunnelService.establishTun(ip, prefix, mtu, routes) -> fd or -1 */
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
Java_org_openipc_camex_TunnelService_nativeAcquire(JNIEnv *env, jclass cls)
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
Java_org_openipc_camex_TunnelService_nativeRelease(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    pthread_mutex_lock(&g_slot_lock);
    g_slot_busy = 0;
    pthread_mutex_unlock(&g_slot_lock);
}

/* Run camex; the caller holds the run slot. */
JNIEXPORT jint JNICALL
Java_org_openipc_camex_TunnelService_nativeRun(
    JNIEnv *env, jobject thiz,
    jint tun_fd,
    jobjectArray jargv)
{
    jsize argc;
    char **argv;
    int i, ret;
    char fd_str[32];

    if (tun_fd >= 0) {
        /* Legacy path: ready-made fd. Clear FD_CLOEXEC so camex owns it. */
        fcntl((int)tun_fd, F_SETFD, 0);
        tun_set_provider(NULL, NULL);
    } else {
        g_env = env;
        g_service = thiz;
        tun_set_provider(jni_tun_provider, NULL);
        unsetenv("CAMEX_TUN_FD");
    }

    /* Convert Java string array to C argv */
    argc = (*env)->GetArrayLength(env, jargv);
    argv = malloc((size_t)(argc + 1) * sizeof(char *));
    if (argv == NULL) return -1;

    for (i = 0; i < argc; i++) {
        jstring js = (jstring)(*env)->GetObjectArrayElement(env, jargv, i);
        const char *utf = (*env)->GetStringUTFChars(env, js, NULL);
        argv[i] = strdup(utf);
        (*env)->ReleaseStringUTFChars(env, js, utf);
    }
    argv[argc] = NULL;

    if (tun_fd >= 0) {
        /* Pass the TUN fd via environment */
        snprintf(fd_str, sizeof(fd_str), "%d", (int)tun_fd);
        setenv("CAMEX_TUN_FD", fd_str, 1);
    }

    /* Run camex (blocks until tunnel exits) */
    ret = camex_main((int)argc, argv);

    /* Cleanup */
    tun_set_provider(NULL, NULL);
    g_env = NULL;
    g_service = NULL;
    for (i = 0; i < argc; i++) free(argv[i]);
    free(argv);

    return (jint)ret;
}

/* Ask camex to stop; nativeRun() returns within about a second. */
JNIEXPORT void JNICALL
Java_org_openipc_camex_TunnelService_nativeStop(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    running = 0;
}
