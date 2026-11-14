/*
 * SCDA P2P - dinput8.dll proxy for Splinter Cell Double Agent (SCDA_online.exe)
 *
 * Problem: the game's Demonware network layer calls GetAdaptersInfo() at
 * startup, builds a list of local IPv4 addresses and binds its UDP socket to
 * one of them (in practice the first adapter Windows reports). Virtual LAN
 * adapters such as ZeroTier are usually not first, so LAN broadcasts never
 * reach the virtual network and LAN games are never found.
 *
 * Fix: this DLL is loaded by the game in place of dinput8.dll. It forwards
 * DirectInput to the real system dinput8.dll and patches the game's import of
 * GetAdaptersInfo so the game only sees the adapter we choose.
 *
 * Second problem: the LAN session search uses a separate socket bound to
 * INADDR_ANY and sends to 255.255.255.255. Windows sends that limited broadcast
 * only out of the primary adapter, never ZeroTier. So sendto/WSASendTo are also
 * hooked: every packet to 255.255.255.255 is additionally sent to the chosen
 * adapter's subnet broadcast (e.g. 192.168.193.255), which goes out ZeroTier.
 *
 * Config: SCDA_P2P.ini next to the game exe, section [P2P]
 *   Enabled=1
 *   PreferredIP=10.147.17.5      ; exact IPv4 to use (written by the launcher)
 *   AdapterMatch=ZeroTier        ; fallback: substring of adapter description
 * Log:    SCDA_P2P.log next to the game exe.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

typedef DWORD (WINAPI *GetAdaptersInfo_t)(PIP_ADAPTER_INFO, PULONG);
typedef int (WSAAPI *sendto_t)(SOCKET, const char *, int, int, const struct sockaddr *, int);
typedef int (WSAAPI *WSASendTo_t)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const struct sockaddr *, int,
                                  LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

static sendto_t    g_realSendto;
static WSASendTo_t g_realWSASendTo;
static volatile ULONG g_bcastAddr;   /* network byte order; 0 = unknown */
static ULONG g_ownIP, g_ownMask;     /* chosen adapter, network byte order */
static char  g_peers[1024];          /* [P2P] Peers= extra IPs, comma separated */
static volatile LONG g_fanoutLogged;
static volatile LONG  g_bcastLogged;

static HMODULE g_realDinput;
static GetAdaptersInfo_t g_realGetAdaptersInfo;
static char g_dir[MAX_PATH];
static char g_iniPath[MAX_PATH];
static char g_logPath[MAX_PATH];
static char g_preferredIP[64];
static char g_adapterMatch[128];
static int  g_enabled = 1;
static CRITICAL_SECTION g_logLock;

/* ------------------------------------------------------------------ log */
static void logf(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME t;
    EnterCriticalSection(&g_logLock);
    f = fopen(g_logPath, "a");
    if (f) {
        GetLocalTime(&t);
        fprintf(f, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_logLock);
}

/* ---------------------------------------------------- real dinput8.dll */
static FARPROC real_proc(const char *name)
{
    if (!g_realDinput) {
        char path[MAX_PATH];
        /* In a 32-bit process on 64-bit Windows this resolves to SysWOW64. */
        GetSystemDirectoryA(path, MAX_PATH);
        strcat_s(path, MAX_PATH, "\\dinput8.dll");
        g_realDinput = LoadLibraryA(path);
        if (!g_realDinput) {
            logf("ERROR: could not load real %s (err %lu)", path, GetLastError());
            return NULL;
        }
    }
    return GetProcAddress(g_realDinput, name);
}

typedef HRESULT (WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, void *);
typedef HRESULT (WINAPI *DllCanUnloadNow_t)(void);
typedef HRESULT (WINAPI *DllGetClassObject_t)(REFCLSID, REFIID, LPVOID *);
typedef HRESULT (WINAPI *DllRegister_t)(void);
typedef LPCVOID (WINAPI *GetdfDIJoystick_t)(void);

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID *o, void *u)
{
    DirectInput8Create_t fn = (DirectInput8Create_t)real_proc("DirectInput8Create");
    return fn ? fn(h, v, r, o, u) : E_FAIL;
}
HRESULT WINAPI Proxy_DllCanUnloadNow(void)
{
    DllCanUnloadNow_t fn = (DllCanUnloadNow_t)real_proc("DllCanUnloadNow");
    return fn ? fn() : S_FALSE;
}
HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID c, REFIID r, LPVOID *o)
{
    DllGetClassObject_t fn = (DllGetClassObject_t)real_proc("DllGetClassObject");
    return fn ? fn(c, r, o) : E_FAIL;
}
HRESULT WINAPI Proxy_DllRegisterServer(void)
{
    DllRegister_t fn = (DllRegister_t)real_proc("DllRegisterServer");
    return fn ? fn() : E_FAIL;
}
HRESULT WINAPI Proxy_DllUnregisterServer(void)
{
    DllRegister_t fn = (DllRegister_t)real_proc("DllUnregisterServer");
    return fn ? fn() : E_FAIL;
}
LPCVOID WINAPI Proxy_GetdfDIJoystick(void)
{
    GetdfDIJoystick_t fn = (GetdfDIJoystick_t)real_proc("GetdfDIJoystick");
    return fn ? fn() : NULL;
}

/* --------------------------------------------- GetAdaptersInfo filter */
static int adapter_has_ip(const IP_ADAPTER_INFO *a, const char *ip)
{
    const IP_ADDR_STRING *s;
    for (s = &a->IpAddressList; s; s = s->Next)
        if (strcmp(s->IpAddress.String, ip) == 0)
            return 1;
    return 0;
}

static int ci_contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle), i;
    if (!n) return 0;
    for (i = 0; hay[i]; i++)
        if (_strnicmp(hay + i, needle, n) == 0)
            return 1;
    return 0;
}

static int adapter_has_valid_ip(const IP_ADAPTER_INFO *a)
{
    const IP_ADDR_STRING *s;
    for (s = &a->IpAddressList; s; s = s->Next)
        if (s->IpAddress.String[0] && strcmp(s->IpAddress.String, "0.0.0.0") != 0 &&
            strncmp(s->IpAddress.String, "169.254.", 8) != 0)   /* not connected yet */
            return 1;
    return 0;
}

static DWORD WINAPI Hook_GetAdaptersInfo(PIP_ADAPTER_INFO out, PULONG outLen)
{
    ULONG len = 16 * 1024;
    PIP_ADAPTER_INFO all = NULL, a, chosen = NULL;
    DWORD rc;
    int tries;
    ULONG_PTR caller = (ULONG_PTR)_ReturnAddress();

    if (!outLen)
        return ERROR_INVALID_PARAMETER;

    /* SCDA_online.exe 1.02a: the function at 0x10ad45c0 reads the MAC address of the
     * LAST adapter in the list (machine ID stored in the registry). Changing the list
     * there makes the game crash a moment later, so that caller gets the real list.
     * Only the Demonware network init (0x10d10810..0x10d10c00) needs filtering. */
    if (caller == 0x10ad45e0) {
        logf("GetAdaptersInfo from 0x%08lx (machine-ID check): real list", (unsigned long)caller);
        return g_realGetAdaptersInfo(out, outLen);
    }
    logf("GetAdaptersInfo from 0x%08lx, buffer %lu bytes", (unsigned long)caller, (unsigned long)*outLen);

    for (tries = 0; tries < 4; tries++) {
        free(all);
        all = (PIP_ADAPTER_INFO)malloc(len);
        if (!all) return ERROR_NOT_ENOUGH_MEMORY;
        rc = g_realGetAdaptersInfo(all, &len);
        if (rc != ERROR_BUFFER_OVERFLOW) break;
    }
    if (rc != ERROR_SUCCESS) {
        free(all);
        logf("GetAdaptersInfo: real call failed (%lu), passing through", rc);
        return g_realGetAdaptersInfo(out, outLen);
    }

    for (a = all; a; a = a->Next) {
        logf("  adapter: \"%s\" ip=%s gw=%s", a->Description,
             a->IpAddressList.IpAddress.String, a->GatewayList.IpAddress.String);
    }

    if (g_preferredIP[0])
        for (a = all; a && !chosen; a = a->Next)
            if (adapter_has_ip(a, g_preferredIP)) chosen = a;
    if (!chosen && g_adapterMatch[0])
        for (a = all; a && !chosen; a = a->Next)
            if (ci_contains(a->Description, g_adapterMatch) && adapter_has_valid_ip(a)) chosen = a;

    if (!chosen) {
        logf("GetAdaptersInfo: no adapter matched PreferredIP=\"%s\" AdapterMatch=\"%s\"; game sees all adapters unchanged",
             g_preferredIP, g_adapterMatch);
        free(all);
        return g_realGetAdaptersInfo(out, outLen);
    }

    /* The game's Demonware init first calls with a 640-byte stack buffer and, when that
     * succeeds, later frees a pointer it never allocated -> DebugBreak crash. Real
     * Windows always overflows that first call on PCs with several adapters, so we
     * do the same: demand a buffer bigger than one entry, which makes the game
     * allocate its own buffer and take the path that works. */
    if (!out || *outLen < 2 * sizeof(IP_ADAPTER_INFO)) {
        *outLen = 2 * sizeof(IP_ADAPTER_INFO);
        free(all);
        return ERROR_BUFFER_OVERFLOW;
    }

    /* Return only the chosen adapter, with the chosen IP as its only address. */
    memcpy(out, chosen, sizeof(IP_ADAPTER_INFO));
    out->Next = NULL;
    if (g_preferredIP[0] && adapter_has_ip(chosen, g_preferredIP)) {
        strncpy_s(out->IpAddressList.IpAddress.String, sizeof(out->IpAddressList.IpAddress.String),
                  g_preferredIP, _TRUNCATE);
        {   /* copy matching mask */
            const IP_ADDR_STRING *s;
            for (s = &chosen->IpAddressList; s; s = s->Next)
                if (strcmp(s->IpAddress.String, g_preferredIP) == 0)
                    out->IpAddressList.IpMask = s->IpMask;
        }
    }
    out->IpAddressList.Next = NULL;
    out->GatewayList.Next = NULL;
    out->DhcpServer.Next = NULL;
    out->PrimaryWinsServer.Next = NULL;
    out->SecondaryWinsServer.Next = NULL;
    out->CurrentIpAddress = NULL;
    *outLen = sizeof(IP_ADAPTER_INFO);

    logf("GetAdaptersInfo: game will use \"%s\" ip=%s", out->Description, out->IpAddressList.IpAddress.String);
    free(all);
    return ERROR_SUCCESS;
}

/* ------------------------------------------------- broadcast redirect */
/* Works out the subnet broadcast address (ip | ~mask) of the chosen adapter. */
static ULONG compute_bcast(void)
{
    ULONG len = 16 * 1024, result = 0;
    PIP_ADAPTER_INFO all = NULL, a;
    const IP_ADDR_STRING *s, *pick = NULL;
    DWORD rc = ERROR_BUFFER_OVERFLOW;
    int tries;

    for (tries = 0; tries < 4 && rc == ERROR_BUFFER_OVERFLOW; tries++) {
        free(all);
        all = (PIP_ADAPTER_INFO)malloc(len);
        if (!all) return 0;
        rc = g_realGetAdaptersInfo(all, &len);
    }
    if (rc == ERROR_SUCCESS) {
        if (g_preferredIP[0])
            for (a = all; a && !pick; a = a->Next)
                for (s = &a->IpAddressList; s && !pick; s = s->Next)
                    if (strcmp(s->IpAddress.String, g_preferredIP) == 0) pick = s;
        if (!pick && g_adapterMatch[0])
            for (a = all; a && !pick; a = a->Next)
                if (ci_contains(a->Description, g_adapterMatch))
                    for (s = &a->IpAddressList; s && !pick; s = s->Next)
                        if (strcmp(s->IpAddress.String, "0.0.0.0") && strncmp(s->IpAddress.String, "169.254.", 8))
                            pick = s;
        if (pick) {
            ULONG ip = inet_addr(pick->IpAddress.String), mask = inet_addr(pick->IpMask.String);
            if (ip != INADDR_NONE && mask && mask != INADDR_NONE) {
                result = ip | ~mask;
                g_ownIP = ip;
                g_ownMask = mask;
            }
        }
    }
    free(all);
    return result;
}

static ULONG get_bcast(void)
{
    static DWORD lastTry;
    if (!g_bcastAddr && GetTickCount() - lastTry > 5000) {
        struct in_addr ia;
        lastTry = GetTickCount();
        g_bcastAddr = compute_bcast();
        ia.s_addr = g_bcastAddr;
        if (g_bcastAddr) logf("LAN broadcasts will also be sent to %s", inet_ntoa(ia));
        else logf("WARNING: could not work out the ZeroTier broadcast address; LAN search will only use the main adapter");
    }
    return g_bcastAddr;
}

/* Returns a copy of 'to' pointed at the ZeroTier broadcast address if 'to' is 255.255.255.255. */
static int redirect_target(const struct sockaddr *to, int tolen, struct sockaddr_in *out)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)to;
    ULONG b;
    if (!to || tolen < (int)sizeof(struct sockaddr_in) || sin->sin_family != AF_INET) return 0;
    if (sin->sin_addr.s_addr != INADDR_BROADCAST) return 0;
    b = get_bcast();
    if (!b) return 0;
    *out = *sin;
    out->sin_addr.s_addr = b;
    if (InterlockedIncrement(&g_bcastLogged) <= 5)
        logf("LAN broadcast to port %u copied to the ZeroTier network", (unsigned)ntohs(sin->sin_port));
    return 1;
}

/* Unicast fan-out: ZeroTier networks may not pass broadcasts, so the LAN search is
 * also sent directly to every address in the ZeroTier subnet (at most 1022 hosts;
 * for bigger subnets, the /24 around our own address) plus any [P2P] Peers= IPs.
 * Returns the number of targets written to 'out' (network byte order). */
#define MAX_FANOUT 1100
static int build_fanout(ULONG *out)
{
    int n = 0;
    if (g_ownIP && g_ownMask) {
        ULONG ip = ntohl(g_ownIP), mask = ntohl(g_ownMask), net, last, h;
        if (~mask > 1023) mask = 0xFFFFFF00;      /* huge subnet: sweep only our /24 */
        net = ip & mask;
        last = net | ~mask;
        for (h = net + 1; h < last && n < MAX_FANOUT; h++)
            if (h != ip) out[n++] = htonl(h);
    }
    if (g_peers[0]) {
        char tmp[1024], *tok, *ctx = NULL;
        strcpy_s(tmp, sizeof(tmp), g_peers);
        for (tok = strtok_s(tmp, ", ;", &ctx); tok && n < MAX_FANOUT; tok = strtok_s(NULL, ", ;", &ctx)) {
            ULONG a = inet_addr(tok);
            if (a != INADDR_NONE && a != g_ownIP) out[n++] = a;
        }
    }
    return n;
}

static void log_fanout(int n, const struct sockaddr *to)
{
    if (InterlockedIncrement(&g_fanoutLogged) <= 5)
        logf("LAN search to port %u also sent directly to %d ZeroTier address(es)",
             (unsigned)ntohs(((const struct sockaddr_in *)to)->sin_port), n);
}

static ULONG g_fanout[MAX_FANOUT];
static CRITICAL_SECTION g_fanoutLock;

/* ------------------------------------------------- traffic log (debug) */
typedef int (WSAAPI *recvfrom_t)(SOCKET, char *, int, int, struct sockaddr *, int *);
typedef int (WSAAPI *WSARecvFrom_t)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, struct sockaddr *, LPINT,
                                    LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
static recvfrom_t    g_realRecvfrom;
static WSARecvFrom_t g_realWSARecvFrom;
static int           g_traceMax = 300;      /* [P2P] TraceLines=, 0 disables */
static volatile LONG g_traceCount;

static void trace(const char *dir, SOCKET s, const struct sockaddr *peer, int bytes)
{
    const struct sockaddr_in *p = (const struct sockaddr_in *)peer;
    struct sockaddr_in local; int ll = sizeof(local);
    char peerStr[32];
    if (!g_traceMax || !peer || p->sin_family != AF_INET || bytes < 0) return;
    if ((ntohl(p->sin_addr.s_addr) >> 24) == 127) return;                 /* loopback */
    if (InterlockedIncrement(&g_traceCount) > g_traceMax) return;
    memset(&local, 0, sizeof(local));
    getsockname(s, (struct sockaddr *)&local, &ll);
    strcpy_s(peerStr, sizeof(peerStr), inet_ntoa(p->sin_addr));
    logf("NET %s %s:%u  (local port %u, %d bytes)", dir, peerStr, (unsigned)ntohs(p->sin_port),
         (unsigned)ntohs(local.sin_port), bytes);
    if (g_traceCount == g_traceMax) logf("NET trace limit reached (%d lines)", g_traceMax);
}

static int WSAAPI Hook_recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen)
{
    int rc = g_realRecvfrom(s, buf, len, flags, from, fromlen);
    if (rc > 0) trace("recv <-", s, from, rc);
    return rc;
}

static int WSAAPI Hook_WSARecvFrom(SOCKET s, LPWSABUF bufs, DWORD n, LPDWORD got, LPDWORD flags,
                                   struct sockaddr *from, LPINT fromlen,
                                   LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    int rc = g_realWSARecvFrom(s, bufs, n, got, flags, from, fromlen, ov, cr);
    if (rc == 0 && !ov && !cr && got) trace("recv <-", s, from, (int)*got);
    return rc;
}

static int WSAAPI Hook_sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
    struct sockaddr_in alt;
    int rc = g_realSendto(s, buf, len, flags, to, tolen);
    trace(rc == SOCKET_ERROR ? "send FAILED ->" : "send ->", s, to, len);
    if (redirect_target(to, tolen, &alt)) {
        int rc2 = g_realSendto(s, buf, len, flags, (struct sockaddr *)&alt, sizeof(alt));
        int i, n;
        if (rc == SOCKET_ERROR) rc = rc2;   /* report success if either copy went out */
        EnterCriticalSection(&g_fanoutLock);
        n = build_fanout(g_fanout);
        for (i = 0; i < n; i++) {
            alt.sin_addr.s_addr = g_fanout[i];
            g_realSendto(s, buf, len, flags, (struct sockaddr *)&alt, sizeof(alt));
        }
        LeaveCriticalSection(&g_fanoutLock);
        log_fanout(n, to);
    }
    return rc;
}

static int WSAAPI Hook_WSASendTo(SOCKET s, LPWSABUF bufs, DWORD n, LPDWORD sent, DWORD flags,
                                 const struct sockaddr *to, int tolen,
                                 LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    struct sockaddr_in alt;
    int rc;
    if (ov || cr)   /* overlapped sends are passed through untouched */
        return g_realWSASendTo(s, bufs, n, sent, flags, to, tolen, ov, cr);
    rc = g_realWSASendTo(s, bufs, n, sent, flags, to, tolen, NULL, NULL);
    {
        DWORD i, total = 0;
        for (i = 0; i < n; i++) total += bufs[i].len;
        trace(rc == SOCKET_ERROR ? "send FAILED ->" : "send ->", s, to, (int)total);
    }
    if (redirect_target(to, tolen, &alt)) {
        DWORD sent2 = 0;
        int rc2 = g_realWSASendTo(s, bufs, n, &sent2, flags, (struct sockaddr *)&alt, sizeof(alt), NULL, NULL);
        int i, cnt;
        if (rc == SOCKET_ERROR && rc2 == 0) { rc = 0; if (sent) *sent = sent2; }
        EnterCriticalSection(&g_fanoutLock);
        cnt = build_fanout(g_fanout);
        for (i = 0; i < cnt; i++) {
            alt.sin_addr.s_addr = g_fanout[i];
            g_realWSASendTo(s, bufs, n, &sent2, flags, (struct sockaddr *)&alt, sizeof(alt), NULL, NULL);
        }
        LeaveCriticalSection(&g_fanoutLock);
        log_fanout(cnt, to);
    }
    return rc;
}

/* ---------------------------------------------------------- IAT patch */
/* funcName != NULL: match by name; funcName == NULL: match by ordinal. */
static int patch_iat(HMODULE mod, const char *dllName, const char *funcName, WORD ordinal, void *hook, void **orig)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    int patched = 0;

    if (!dir.VirtualAddress) return 0;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
        IMAGE_THUNK_DATA *names, *funcs;
        if (_stricmp((char *)(base + imp->Name), dllName) != 0) continue;
        names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        funcs = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, funcs++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                if (funcName || IMAGE_ORDINAL(names->u1.Ordinal) != ordinal) continue;
            } else {
                if (!funcName) continue;
                ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
                if (strcmp((char *)ibn->Name, funcName) != 0) continue;
            }
            if (VirtualProtect(&funcs->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                if (orig && !*orig) *orig = (void *)funcs->u1.Function;
                funcs->u1.Function = (ULONG_PTR)hook;
                VirtualProtect(&funcs->u1.Function, sizeof(void *), old, &old);
                patched++;
            }
        }
    }
    return patched;
}

/* ------------------------------------------------------------ startup */
static void init(HMODULE self)
{
    char exePath[MAX_PATH], *slash;
    HMODULE iphlp;
    int n;

    InitializeCriticalSection(&g_logLock);
    InitializeCriticalSection(&g_fanoutLock);
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    strcpy_s(g_dir, MAX_PATH, exePath);
    slash = strrchr(g_dir, '\\');
    if (slash) slash[1] = 0;
    sprintf_s(g_iniPath, MAX_PATH, "%sSCDA_P2P.ini", g_dir);
    sprintf_s(g_logPath, MAX_PATH, "%sSCDA_P2P.log", g_dir);
    DeleteFileA(g_logPath);

    g_enabled = GetPrivateProfileIntA("P2P", "Enabled", 1, g_iniPath);
    GetPrivateProfileStringA("P2P", "PreferredIP", "", g_preferredIP, sizeof(g_preferredIP), g_iniPath);
    GetPrivateProfileStringA("P2P", "AdapterMatch", "ZeroTier", g_adapterMatch, sizeof(g_adapterMatch), g_iniPath);
    GetPrivateProfileStringA("P2P", "Peers", "", g_peers, sizeof(g_peers), g_iniPath);

    logf("SCDA P2P loaded into %s", exePath);
    logf("Enabled=%d PreferredIP=\"%s\" AdapterMatch=\"%s\"", g_enabled, g_preferredIP, g_adapterMatch);
    (void)self;

    if (!g_enabled) {
        logf("Disabled in ini; acting as plain dinput8 passthrough");
        return;
    }

    iphlp = LoadLibraryA("iphlpapi.dll");
    if (!g_realGetAdaptersInfo && iphlp)
        g_realGetAdaptersInfo = (GetAdaptersInfo_t)GetProcAddress(iphlp, "GetAdaptersInfo");
    if (!g_realGetAdaptersInfo) {
        logf("ERROR: could not resolve GetAdaptersInfo");
        return;
    }
    n = patch_iat(GetModuleHandleA(NULL), "iphlpapi.dll", "GetAdaptersInfo", 0,
                  (void *)Hook_GetAdaptersInfo, NULL);
    logf("Patched %d GetAdaptersInfo import(s)", n);

    {
        HMODULE ws = LoadLibraryA("ws2_32.dll");
        HMODULE exe = GetModuleHandleA(NULL);
        int a = 0, b = 0;
        g_realSendto = (sendto_t)GetProcAddress(ws, "sendto");
        g_realWSASendTo = (WSASendTo_t)GetProcAddress(ws, "WSASendTo");
        if (g_realSendto) {
            a  = patch_iat(exe, "ws2_32.dll", "sendto", 0, (void *)Hook_sendto, NULL);
            a += patch_iat(exe, "ws2_32.dll", NULL, 20, (void *)Hook_sendto, NULL);       /* sendto by ordinal */
        }
        if (g_realWSASendTo)
            b = patch_iat(exe, "ws2_32.dll", "WSASendTo", 0, (void *)Hook_WSASendTo, NULL);
        logf("Patched %d sendto and %d WSASendTo import(s) for LAN broadcast redirect", a, b);

        g_traceMax = GetPrivateProfileIntA("P2P", "TraceLines", 300, g_iniPath);
        g_realRecvfrom = (recvfrom_t)GetProcAddress(ws, "recvfrom");
        g_realWSARecvFrom = (WSARecvFrom_t)GetProcAddress(ws, "WSARecvFrom");
        a = b = 0;
        if (g_realRecvfrom) {
            a  = patch_iat(exe, "ws2_32.dll", "recvfrom", 0, (void *)Hook_recvfrom, NULL);
            a += patch_iat(exe, "ws2_32.dll", NULL, 17, (void *)Hook_recvfrom, NULL);    /* recvfrom by ordinal */
        }
        if (g_realWSARecvFrom)
            b = patch_iat(exe, "ws2_32.dll", "WSARecvFrom", 0, (void *)Hook_WSARecvFrom, NULL);
        logf("Traffic log: %d line(s) max; patched %d recvfrom and %d WSARecvFrom import(s)", g_traceMax, a, b);
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        init(h);
    }
    return TRUE;
}
