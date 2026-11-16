/*
 * Frame-rate cap (included into dinput8_proxy.c).
 *
 * SCDA's multiplayer was built around ~30 FPS. At high frame rates movement turns stiff (4-way
 * feel, weak strafing/diagonals) and aiming gets spongy. With V-Sync off, or in a borderless
 * window, nothing stops the game running at hundreds of FPS on a modern PC.
 *
 * The game exe imports Direct3DCreate9 itself. We patch that import, then the IDirect3D9
 * CreateDevice slot, then the device's Present slot, and wait in Present until the next frame
 * is due. [Display] FrameLimit=60 (0 = no cap). Average FPS is logged every StatsSeconds.
 */
#include <mmsystem.h>

typedef void   *(WINAPI  *D3DCreate9_t)(UINT);
typedef HRESULT (WINAPI  *CreateDevice_t)(void *, UINT, DWORD, HWND, DWORD, void *, void **);
typedef HRESULT (WINAPI  *Present_t)(void *, const RECT *, const RECT *, HWND, const void *);
typedef MMRESULT (WINAPI *timeBeginPeriod_t)(UINT);

#define D3D9_CREATEDEVICE_SLOT 16   /* IDirect3D9::CreateDevice */
#define D3DDEV_PRESENT_SLOT    17   /* IDirect3DDevice9::Present */

static D3DCreate9_t   g_realD3DCreate9;
static CreateDevice_t g_realCreateDevice;
static Present_t      g_realPresent;
static int            g_fpsLimit;              /* 0 = no cap */
static LONGLONG       g_qpf, g_period, g_next;
static HANDLE         g_capTimer;
static volatile LONG  g_frames, g_framesTotal;
static volatile LONG  g_slowestUs;             /* longest frame in this interval, microseconds */
static LONGLONG       g_lastFrame;
static volatile LONG  g_devices;

static void fc_wait(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_next || now.QuadPart - g_next > g_period * 4) {   /* first frame, or a long stall (loading) */
        g_next = now.QuadPart + g_period;
        return;
    }
    for (;;) {
        LONGLONG left = g_next - now.QuadPart;
        if (left <= 0) break;
        if (left * 1000 / g_qpf >= 2) {
            if (g_capTimer) {
                LARGE_INTEGER due;
                due.QuadPart = -((left * 10000000 / g_qpf) - 10000);   /* wake ~1 ms early, then spin */
                if (due.QuadPart < 0 && SetWaitableTimer(g_capTimer, &due, 0, NULL, NULL, FALSE))
                    WaitForSingleObject(g_capTimer, 50);
            } else {
                Sleep(1);
            }
        } else {
            YieldProcessor();
        }
        QueryPerformanceCounter(&now);
    }
    g_next += g_period;
    if (now.QuadPart - g_next > g_period) g_next = now.QuadPart + g_period;   /* fell behind: don't race to catch up */
}

static HRESULT WINAPI Hook_Present(void *dev, const RECT *src, const RECT *dst, HWND wnd, const void *dirty)
{
    LARGE_INTEGER now;
    if (g_fpsLimit > 0) fc_wait();
    QueryPerformanceCounter(&now);
    if (g_lastFrame) {
        LONG us = (LONG)((now.QuadPart - g_lastFrame) * 1000000 / g_qpf);
        if (us > g_slowestUs) g_slowestUs = us;
    }
    g_lastFrame = now.QuadPart;
    InterlockedIncrement(&g_frames);
    InterlockedIncrement(&g_framesTotal);
    return g_realPresent(dev, src, dst, wnd, dirty);
}

static int fc_patch_slot(void *obj, int slot, void *hook, void **orig)
{
    void **vtbl = *(void ***)obj;
    DWORD old;
    if (vtbl[slot] == hook) return 1;
    if (!VirtualProtect(&vtbl[slot], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) return 0;
    if (!*orig) *orig = vtbl[slot];
    vtbl[slot] = hook;
    VirtualProtect(&vtbl[slot], sizeof(void *), old, &old);
    return 1;
}

static HRESULT WINAPI Hook_CreateDevice(void *d3d, UINT adapter, DWORD type, HWND wnd, DWORD flags,
                                        void *pp, void **outDev)
{
    HRESULT hr = g_realCreateDevice(d3d, adapter, type, wnd, flags, pp, outDev);
    if (SUCCEEDED(hr) && outDev && *outDev) {
        int ok = fc_patch_slot(*outDev, D3DDEV_PRESENT_SLOT, (void *)Hook_Present, (void **)&g_realPresent);
        if (InterlockedIncrement(&g_devices) == 1) {
            if (!ok) logf("Frame rate: could not hook Direct3D Present - no cap");
            else if (g_fpsLimit > 0) logf("Frame rate: capped at %d FPS (Direct3D device created)", g_fpsLimit);
            else logf("Frame rate: no cap (FrameLimit=0); FPS is still logged");
        }
    }
    return hr;
}

static void *WINAPI Hook_Direct3DCreate9(UINT sdk)
{
    void *d3d = g_realD3DCreate9(sdk);
    if (d3d && !fc_patch_slot(d3d, D3D9_CREATEDEVICE_SLOT, (void *)Hook_CreateDevice, (void **)&g_realCreateDevice))
        logf("Frame rate: could not hook Direct3D CreateDevice - no cap");
    return d3d;
}

static DWORD WINAPI fps_thread(LPVOID p)
{
    int secs = (int)(INT_PTR)p, warned = 0, quietLogged = 0;
    for (;;) {
        LONG n, slow;
        Sleep(secs * 1000);
        n = InterlockedExchange(&g_frames, 0);
        slow = InterlockedExchange(&g_slowestUs, 0);
        if (n == 0) {
            if (g_devices && !g_framesTotal && !warned++)
                logf("Frame rate: the game hasn't presented a frame through the hooked device yet");
            if (g_framesTotal && !quietLogged++) logf("FPS: no frames (minimised or loading)");
            continue;
        }
        quietLogged = 0;
        logf("FPS %.1f average, slowest frame %ld ms%s", n / (double)secs, slow / 1000,
             g_fpsLimit > 0 ? "" : " (no cap)");
    }
}

static void framecap_init(int statsSecs)
{
    LARGE_INTEGER f;
    HMODULE d3d9, winmm;
    int n;
    g_fpsLimit = GetPrivateProfileIntA("Display", "FrameLimit", 60, g_iniPath);
    if (g_fpsLimit < 0) g_fpsLimit = 0;
    if (g_fpsLimit > 0 && g_fpsLimit < 15) g_fpsLimit = 15;
    if (g_fpsLimit > 1000) g_fpsLimit = 0;
    QueryPerformanceFrequency(&f);
    g_qpf = f.QuadPart;
    if (g_fpsLimit > 0) g_period = g_qpf / g_fpsLimit;

    d3d9 = LoadLibraryA("d3d9.dll");
    g_realD3DCreate9 = d3d9 ? (D3DCreate9_t)GetProcAddress(d3d9, "Direct3DCreate9") : NULL;
    if (!g_realD3DCreate9) { logf("Frame rate: d3d9.dll not found - no cap"); return; }
    n = patch_iat(GetModuleHandleA(NULL), "d3d9.dll", "Direct3DCreate9", 0, (void *)Hook_Direct3DCreate9, NULL);
    if (!n) { logf("Frame rate: game doesn't import Direct3DCreate9 - no cap"); return; }

    if (g_fpsLimit > 0) {
        /* precise sleeps: high-resolution timer (Windows 10 1803+), else 1 ms timer resolution */
        g_capTimer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                            TIMER_ALL_ACCESS);
        winmm = LoadLibraryA("winmm.dll");
        if (winmm) {
            timeBeginPeriod_t tbp = (timeBeginPeriod_t)GetProcAddress(winmm, "timeBeginPeriod");
            if (tbp) tbp(1);
        }
        logf("Frame rate limit: %d FPS (the game's multiplayer misbehaves at high frame rates)", g_fpsLimit);
    } else {
        logf("Frame rate limit: off");
    }
    {
        HANDLE t = CreateThread(NULL, 0, fps_thread, (LPVOID)(INT_PTR)(statsSecs > 0 ? statsSecs : 30), 0, NULL);
        if (t) CloseHandle(t);
    }
}
