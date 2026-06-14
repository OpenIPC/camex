/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * camex-jni.c — JNI bridge for camex Android VpnService
 *
 * Android VpnService TUN fds have FD_CLOEXEC and cannot be
 * inherited by subprocesses.  This JNI shim clears CLOEXEC,
 * sets CAMEX_TUN_FD, and calls camex_main() in-process.
 */
#include <jni.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* camex entry point */
extern int camex_main(int argc, char *argv[]);

JNIEXPORT jint JNICALL
Java_org_openipc_camex_TunnelService_nativeStart(
    JNIEnv *env, jobject thiz,
    jint tun_fd,
    jobjectArray jargv)
{
    jsize argc;
    char **argv;
    int i, ret;
    char fd_str[32];

    (void)thiz;

    /* Clear FD_CLOEXEC so camex owns the fd */
    fcntl((int)tun_fd, F_SETFD, 0);

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

    /* Pass the TUN fd via environment */
    snprintf(fd_str, sizeof(fd_str), "%d", (int)tun_fd);
    setenv("CAMEX_TUN_FD", fd_str, 1);

    /* Run camex (blocks until tunnel exits) */
    ret = camex_main((int)argc, argv);

    /* Cleanup */
    for (i = 0; i < argc; i++) free(argv[i]);
    free(argv);

    return (jint)ret;
}
