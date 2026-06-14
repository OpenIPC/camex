/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * wintun.c — WinTUN dynamic DLL loading (Windows only)
 *
 */

#ifdef _WIN32

#include "wintun.h"
#include "log.h"

#include <stdlib.h>

/* Global function pointer definitions */
WintunCreateAdapter_t           pWintunCreateAdapter           = NULL;
WintunOpenAdapter_t             pWintunOpenAdapter             = NULL;
WintunCloseAdapter_t            pWintunCloseAdapter            = NULL;
WintunDeleteAdapter_t           pWintunDeleteAdapter           = NULL;
WintunDeletePoolDriver_t        pWintunDeletePoolDriver        = NULL;
WintunGetAdapterLUID_t          pWintunGetAdapterLUID          = NULL;
WintunGetRunningDriverVersion_t pWintunGetRunningDriverVersion = NULL;
WintunSetAdapterLogging_t       pWintunSetAdapterLogging       = NULL;
WintunStartSession_t            pWintunStartSession            = NULL;
WintunEndSession_t              pWintunEndSession              = NULL;
WintunGetReadWaitEvent_t        pWintunGetReadWaitEvent        = NULL;
WintunReceivePacket_t           pWintunReceivePacket           = NULL;
WintunReleaseReceivePacket_t    pWintunReleaseReceivePacket    = NULL;
WintunAllocateSendPacket_t      pWintunAllocateSendPacket      = NULL;
WintunSendPacket_t              pWintunSendPacket              = NULL;

WINTUN_ADAPTER_HANDLE g_wintun_adapter = NULL;
WINTUN_SESSION_HANDLE g_wintun_sess = NULL;
HANDLE g_wintun_thread = NULL;
volatile int g_wintun_running = 0;

static HMODULE g_wintun_dll = NULL;

/*
 * Helper: resolve a function pointer from wintun.dll.
 * Returns 0 on success, -1 on error.
 */
static int resolve_func(const char *name, FARPROC *out)
{
    *out = GetProcAddress(g_wintun_dll, name);
    if (*out == NULL) {
        log_message(LOG_ERR, "WinTUN: missing function '%s' in wintun.dll",
                    name);
        return -1;
    }
    return 0;
}

#define RESOLVE(name) \
    do { \
        if (resolve_func(#name, (FARPROC *)&p##name) != 0) { \
            wintun_unload_dll(); \
            return -1; \
        } \
    } while (0)

int wintun_load_dll(void)
{
    wchar_t dll_path[MAX_PATH];
    DWORD n;

    if (g_wintun_dll != NULL) {
        return 0;  /* already loaded */
    }

    /* Look for wintun.dll next to the executable */
    n = GetModuleFileNameW(NULL, dll_path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        log_message(LOG_ERR, "WinTUN: GetModuleFileNameW failed");
        return -1;
    }

    /* Replace executable name with wintun.dll */
    {
        wchar_t *sep = wcsrchr(dll_path, L'\\');
        if (sep != NULL) {
            *(sep + 1) = L'\0';
        } else {
            dll_path[0] = L'\0';
        }
    }
    wcsncat(dll_path, L"wintun.dll", MAX_PATH - wcslen(dll_path) - 1);

    g_wintun_dll = LoadLibraryExW(dll_path, NULL, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (g_wintun_dll == NULL) {
        /* Fallback: try just "wintun.dll" in search path */
        g_wintun_dll = LoadLibraryExW(L"wintun.dll", NULL,
                                       LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    }
    if (g_wintun_dll == NULL) {
        log_message(LOG_ERR, "WinTUN: failed to load wintun.dll (error %lu)",
                    GetLastError());
        return -1;
    }

    /* Resolve all function pointers */
    RESOLVE(WintunCreateAdapter);
    RESOLVE(WintunOpenAdapter);
    RESOLVE(WintunCloseAdapter);
    RESOLVE(WintunDeleteAdapter);
    RESOLVE(WintunDeletePoolDriver);
    RESOLVE(WintunGetAdapterLUID);
    RESOLVE(WintunGetRunningDriverVersion);
    RESOLVE(WintunSetAdapterLogging);
    RESOLVE(WintunStartSession);
    RESOLVE(WintunEndSession);
    RESOLVE(WintunGetReadWaitEvent);
    RESOLVE(WintunReceivePacket);
    RESOLVE(WintunReleaseReceivePacket);
    RESOLVE(WintunAllocateSendPacket);
    RESOLVE(WintunSendPacket);

    log_message(LOG_INFO, "WinTUN: successfully loaded wintun.dll");
    return 0;
}

void wintun_unload_dll(void)
{
    if (g_wintun_sess != NULL) {
        pWintunEndSession(g_wintun_sess);
        g_wintun_sess = NULL;
    }

    pWintunCreateAdapter           = NULL;
    pWintunOpenAdapter             = NULL;
    pWintunCloseAdapter            = NULL;
    pWintunDeleteAdapter           = NULL;
    pWintunDeletePoolDriver        = NULL;
    pWintunGetAdapterLUID          = NULL;
    pWintunGetRunningDriverVersion = NULL;
    pWintunSetAdapterLogging       = NULL;
    pWintunStartSession            = NULL;
    pWintunEndSession              = NULL;
    pWintunGetReadWaitEvent        = NULL;
    pWintunReceivePacket           = NULL;
    pWintunReleaseReceivePacket    = NULL;
    pWintunAllocateSendPacket      = NULL;
    pWintunSendPacket              = NULL;

    if (g_wintun_dll != NULL) {
        FreeLibrary(g_wintun_dll);
        g_wintun_dll = NULL;
    }
}

#endif /* _WIN32 */
