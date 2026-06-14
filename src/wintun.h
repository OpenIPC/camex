/*
 *
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * wintun.h — WinTUN API function pointer definitions (Windows only)
 *
 */

#ifndef CAMEX_WINTUN_H
#define CAMEX_WINTUN_H

#ifdef _WIN32

/* winsock2.h MUST come before windows.h (MinGW requirement) */
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>   /* inet_pton on MinGW */

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

/*
 * WinTUN function pointer typedefs.
 * See https://www.wintun.net/ for the original C API definitions.
 */

/* Handle types (opaque pointers) */
typedef void *WINTUN_ADAPTER_HANDLE;
typedef void *WINTUN_SESSION_HANDLE;

/* Type: GUID pointer for adapter creation (optional, can be NULL) */
/* Type: WintunCreateAdapter — create or open a Wintun adapter */
typedef WINTUN_ADAPTER_HANDLE (WINAPI *WintunCreateAdapter_t)(
    LPCWSTR Name, LPCWSTR TunnelType, const GUID *RequestedGUID);

/* Type: WintunOpenAdapter — open an existing adapter by name */
typedef WINTUN_ADAPTER_HANDLE (WINAPI *WintunOpenAdapter_t)(
    LPCWSTR Name);

/* Type: WintunCloseAdapter — close/open adapter handle */
typedef void (WINAPI *WintunCloseAdapter_t)(
    WINTUN_ADAPTER_HANDLE Adapter);

/* Type: WintunDeleteAdapter — delete adapter (requires admin) */
typedef void (WINAPI *WintunDeleteAdapter_t)(
    WINTUN_ADAPTER_HANDLE Adapter, BOOL CloseWhenExternalSession);

/* Type: WintunDeletePoolDriver — uninstall driver for a given pool */
typedef void (WINAPI *WintunDeletePoolDriver_t)(
    LPCWSTR Pool);

/* Type: WintunGetRunningDriverVersion — get installed driver version */
typedef DWORD (WINAPI *WintunGetRunningDriverVersion_t)(void);

/* Type: WintunSetAdapterLogging — enable/disable driver logging */
typedef void (WINAPI *WintunSetAdapterLogging_t)(
    WINTUN_ADAPTER_HANDLE Adapter, DWORD Level);

/* Type: WintunStartSession — start a Wintun session (ring buffer) */
typedef WINTUN_SESSION_HANDLE (WINAPI *WintunStartSession_t)(
    WINTUN_ADAPTER_HANDLE Adapter, DWORD Capacity);

/* Type: WintunEndSession — end/close a session */
typedef void (WINAPI *WintunEndSession_t)(
    WINTUN_SESSION_HANDLE Session);

/* Type: WintunGetReadWaitEvent — get event HANDLE for read readiness */
typedef HANDLE (WINAPI *WintunGetReadWaitEvent_t)(
    WINTUN_SESSION_HANDLE Session);

/* Type: WintunReceivePacket — read a packet from the ring buffer */
typedef BYTE *(WINAPI *WintunReceivePacket_t)(
    WINTUN_SESSION_HANDLE Session, DWORD *PacketSize);

/* Type: WintunReleaseReceivePacket — release a received packet */
typedef void (WINAPI *WintunReleaseReceivePacket_t)(
    WINTUN_SESSION_HANDLE Session, const BYTE *Packet);

/* Type: WintunAllocateSendPacket — allocate space for an outgoing packet */
typedef BYTE *(WINAPI *WintunAllocateSendPacket_t)(
    WINTUN_SESSION_HANDLE Session, DWORD PacketSize);

/* Type: WintunSendPacket — send an allocated packet */
typedef void (WINAPI *WintunSendPacket_t)(
    WINTUN_SESSION_HANDLE Session, const BYTE *Packet);

/* Log level constants */
#define WINTUN_LOG_VERBOSE     0
#define WINTUN_LOG_INFO        1
#define WINTUN_LOG_WARN        2
#define WINTUN_LOG_ERR         3

/* Minimum ring capacity */
#define WINTUN_MIN_RING_CAPACITY     0x20000   /* 128 KiB */
#define WINTUN_MAX_RING_CAPACITY     0x4000000 /* 64 MiB */

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

/*
 * CRITICAL SECTION: wintun.dll must be in the same directory as the EXE,
 * or we load it from an absolute path. All function pointers are resolved
 * via GetProcAddress at runtime.
 *
 * Initialise with wintun_load_dll() before creating adapters.
 */

/* Global WinTUN function pointers (resolved at runtime) */
extern WintunCreateAdapter_t             pWintunCreateAdapter;
extern WintunOpenAdapter_t               pWintunOpenAdapter;
extern WintunCloseAdapter_t              pWintunCloseAdapter;
extern WintunDeleteAdapter_t             pWintunDeleteAdapter;
extern WintunDeletePoolDriver_t          pWintunDeletePoolDriver;
extern WintunGetRunningDriverVersion_t   pWintunGetRunningDriverVersion;
extern WintunSetAdapterLogging_t         pWintunSetAdapterLogging;
extern WintunStartSession_t              pWintunStartSession;
extern WintunEndSession_t                pWintunEndSession;
extern WintunGetReadWaitEvent_t          pWintunGetReadWaitEvent;
extern WintunReceivePacket_t             pWintunReceivePacket;
extern WintunReleaseReceivePacket_t      pWintunReleaseReceivePacket;
extern WintunAllocateSendPacket_t        pWintunAllocateSendPacket;
extern WintunSendPacket_t                pWintunSendPacket;

/* Global WinTUN handles (defined in wintun.c, used by tun.c) */
extern WINTUN_ADAPTER_HANDLE g_wintun_adapter;
extern WINTUN_SESSION_HANDLE g_wintun_sess;
extern HANDLE g_wintun_thread;
extern volatile int g_wintun_running;

/* Load wintun.dll and resolve all function pointers. Returns 0 on success. */
int wintun_load_dll(void);

/* Unload wintun.dll and clear all function pointers. */
void wintun_unload_dll(void);

#endif /* _WIN32 */
#endif /* CAMEX_WINTUN_H */
