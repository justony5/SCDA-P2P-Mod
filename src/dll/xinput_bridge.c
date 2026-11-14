/*
 * XInput bridge for SCDA_online.exe (included into dinput8_proxy.c).
 *
 * The game reads controllers through DirectInput 8. Through DirectInput, an Xbox
 * controller reports both triggers as ONE shared axis (Z) and has only 10 buttons,
 * so the triggers can't be bound. The game's own default bindings expect buttons
 * 11-16 (IK_Joy11..IK_Joy16), which a modern pad never sends.
 *
 * DirectInput8Create's object is wrapped (a forwarding COM object). When the game
 * creates a joystick/gamepad device, that device is wrapped too, and its
 * GetDeviceState is filled from XInput:
 *
 *   Axes:    X/Y = left stick, Rx/Ry = right stick (DirectInput direction convention),
 *            Z = centred (the triggers are buttons instead)
 *   Buttons: 1 A, 2 B, 3 X, 4 Y, 5 LB, 6 RB, 7 Back, 8 Start, 9 L-stick, 10 R-stick
 *            11 LT, 12 RT, 13 D-pad up, 14 D-pad right, 15 D-pad down, 16 D-pad left
 *   (game names: IK_Joy1 .. IK_Joy16)
 *
 * (Patching the real objects' vtables is not possible: DirectInput validates the vtable
 *  pointer of every object passed to it.)
 *
 * Config: [Controller] XInput=1 (0 = leave DirectInput alone), TriggerThreshold=30 (0-255)
 */
#include <dinput.h>

typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XGAMEPAD;
typedef struct { DWORD dwPacketNumber; XGAMEPAD Gamepad; } XSTATE;
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XSTATE *);

#define XB_DPAD_UP 0x0001
#define XB_DPAD_DOWN 0x0002
#define XB_DPAD_LEFT 0x0004
#define XB_DPAD_RIGHT 0x0008
#define XB_START 0x0010
#define XB_BACK 0x0020
#define XB_LTHUMB 0x0040
#define XB_RTHUMB 0x0080
#define XB_LB 0x0100
#define XB_RB 0x0200
#define XB_A 0x1000
#define XB_B 0x2000
#define XB_X 0x4000
#define XB_Y 0x8000

static XInputGetState_t g_xget;
static int   g_xbEnabled = 1, g_xbThreshold = 30;
static DWORD g_xbLastPad = 0xFFFFFFFF;

/* ------------------------------------------------ generic COM forwarding */
typedef struct { void **vtbl; void *real; } WRAP;
#define REAL(s)  (((WRAP *)(s))->real)
#define RV(s, i) ((*(void ***)REAL(s))[i])
typedef ULONG_PTR A;
typedef HRESULT (STDMETHODCALLTYPE *F0)(void *);
typedef HRESULT (STDMETHODCALLTYPE *F1)(void *, A);
typedef HRESULT (STDMETHODCALLTYPE *F2)(void *, A, A);
typedef HRESULT (STDMETHODCALLTYPE *F3)(void *, A, A, A);
typedef HRESULT (STDMETHODCALLTYPE *F4)(void *, A, A, A, A);
typedef HRESULT (STDMETHODCALLTYPE *F5)(void *, A, A, A, A, A);
#define FWD0(n, i) static HRESULT STDMETHODCALLTYPE n(void *s) { return ((F0)RV(s, i))(REAL(s)); }
#define FWD1(n, i) static HRESULT STDMETHODCALLTYPE n(void *s, A a) { return ((F1)RV(s, i))(REAL(s), a); }
#define FWD2(n, i) static HRESULT STDMETHODCALLTYPE n(void *s, A a, A b) { return ((F2)RV(s, i))(REAL(s), a, b); }
#define FWD3(n, i) static HRESULT STDMETHODCALLTYPE n(void *s, A a, A b, A c) { return ((F3)RV(s, i))(REAL(s), a, b, c); }
#define FWD4(n, i) static HRESULT STDMETHODCALLTYPE n(void *s, A a, A b, A c, A d) { return ((F4)RV(s, i))(REAL(s), a, b, c, d); }
#define FWD5(n, i) static HRESULT STDMETHODCALLTYPE n(void *s, A a, A b, A c, A d, A e) { return ((F5)RV(s, i))(REAL(s), a, b, c, d, e); }

/* ------------------------------------------------ wrapped device */
#define XB_MAXFMT 64
typedef struct {
    void **vtbl; void *real;               /* must match WRAP */
    LONG   rmin, rmax;                      /* axis range the game asked for */
    DWORD  dataSize;
    int    nfmt;
    struct { int kind; int index; DWORD ofs; } fmt[XB_MAXFMT];   /* kind: 1 axis, 2 button, 3 pov */
} XBDEV;

/* 0..3 = first connected pad, -1 = none. Looks for a new pad at most every 2 s. */
static int xb_read(XSTATE *st)
{
    static DWORD lastScan;
    DWORD i;
    if (!g_xget) return -1;
    if (g_xbLastPad != 0xFFFFFFFF && g_xget(g_xbLastPad, st) == ERROR_SUCCESS) return (int)g_xbLastPad;
    if (GetTickCount() - lastScan < 2000) return -1;
    lastScan = GetTickCount();
    for (i = 0; i < 4; i++)
        if (g_xget(i, st) == ERROR_SUCCESS) {
            if (g_xbLastPad != i) logf("Controller: Xbox controller %lu connected", i + 1);
            g_xbLastPad = i;
            return (int)i;
        }
    if (g_xbLastPad != 0xFFFFFFFF) logf("Controller: Xbox controller disconnected");
    g_xbLastPad = 0xFFFFFFFF;
    return -1;
}

/* Start (Menu) button = Esc key, so it opens/closes the in-game menu. The key is only sent
 * while one of the game's windows is in the foreground. [Controller] StartIsEscape=0 turns it off. */
static int g_xbStartEsc = 1;
static void xb_send_esc(int down)
{
    INPUT in;
    memset(&in, 0, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = VK_ESCAPE;
    in.ki.wScan = 0x01;                                  /* DirectInput keyboards read scan codes */
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(in));
}

static void xb_start_as_escape(int pressed)
{
    static int wasPressed, sentDown;
    DWORD pid = 0;
    if (!g_xbStartEsc) return;
    if (pressed && !wasPressed) {
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid == GetCurrentProcessId()) { xb_send_esc(1); sentDown = 1; }
    } else if (!pressed && wasPressed && sentDown) {
        xb_send_esc(0); sentDown = 0;
    }
    wasPressed = pressed;
}

static LONG xb_scale(LONG v /* -32768..32767 */, const XBDEV *d)
{
    double t = (v + 32768.0) / 65535.0;
    return d->rmin + (LONG)(t * (double)(d->rmax - d->rmin) + 0.5);
}

static HRESULT STDMETHODCALLTYPE Dev_QueryInterface(void *s, REFIID riid, void **out)
{
    if (out && (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirectInputDevice8A) ||
                IsEqualGUID(riid, &IID_IDirectInputDevice8W))) {
        ((F0)RV(s, 1))(REAL(s));       /* AddRef */
        *out = s;
        return S_OK;
    }
    return ((F2)RV(s, 0))(REAL(s), (A)riid, (A)out);
}
static ULONG STDMETHODCALLTYPE Dev_AddRef(void *s) { return (ULONG)((F0)RV(s, 1))(REAL(s)); }
static ULONG STDMETHODCALLTYPE Dev_Release(void *s)
{
    ULONG n = (ULONG)((F0)RV(s, 2))(REAL(s));
    if (n == 0) free(s);
    return n;
}

static HRESULT STDMETHODCALLTYPE Dev_GetCapabilities(void *s, LPDIDEVCAPS caps)
{
    HRESULT hr = ((F1)RV(s, 3))(REAL(s), (A)caps);
    if (SUCCEEDED(hr) && caps) {
        if (caps->dwButtons < 16) caps->dwButtons = 16;   /* triggers + D-pad as buttons 11-16 */
        if (caps->dwAxes < 5) caps->dwAxes = 5;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Dev_SetProperty(void *s, REFGUID prop, LPCDIPROPHEADER ph)
{
    XBDEV *d = (XBDEV *)s;
    if (ph && (ULONG_PTR)prop == (ULONG_PTR)DIPROP_RANGE && ph->dwSize >= sizeof(DIPROPRANGE)) {
        const DIPROPRANGE *r = (const DIPROPRANGE *)ph;
        if (r->lMax > r->lMin && (r->lMin != d->rmin || r->lMax != d->rmax)) {
            d->rmin = r->lMin; d->rmax = r->lMax;
            logf("Controller: axis range %ld..%ld", r->lMin, r->lMax);
        }
    }
    return ((F2)RV(s, 6))(REAL(s), (A)prop, (A)ph);
}

static HRESULT STDMETHODCALLTYPE Dev_SetDataFormat(void *s, LPCDIDATAFORMAT df)
{
    HRESULT hr = ((F1)RV(s, 11))(REAL(s), (A)df);
    XBDEV *d = (XBDEV *)s;
    if (df) {
        DWORD i; int axis = 0, button = 0, pov = 0;
        d->dataSize = df->dwDataSize;
        d->nfmt = 0;
        for (i = 0; i < df->dwNumObjs && d->nfmt < XB_MAXFMT; i++) {
            const DIOBJECTDATAFORMAT *o = &df->rgodf[i];
            int kind = 0, index = 0;
            if (o->pguid && IsEqualGUID(o->pguid, &GUID_XAxis))       { kind = 1; index = 0; }
            else if (o->pguid && IsEqualGUID(o->pguid, &GUID_YAxis))  { kind = 1; index = 1; }
            else if (o->pguid && IsEqualGUID(o->pguid, &GUID_ZAxis))  { kind = 1; index = 2; }
            else if (o->pguid && IsEqualGUID(o->pguid, &GUID_RxAxis)) { kind = 1; index = 3; }
            else if (o->pguid && IsEqualGUID(o->pguid, &GUID_RyAxis)) { kind = 1; index = 4; }
            else if (o->pguid && IsEqualGUID(o->pguid, &GUID_RzAxis)) { kind = 1; index = 5; }
            else if (o->pguid && IsEqualGUID(o->pguid, &GUID_Slider)) { kind = 1; index = 6; }
            else if ((o->pguid && IsEqualGUID(o->pguid, &GUID_POV)) || (o->dwType & DIDFT_POV)) { kind = 3; index = pov++; }
            else if ((o->pguid && IsEqualGUID(o->pguid, &GUID_Button)) || (o->dwType & DIDFT_BUTTON)) { kind = 2; index = button++; }
            else if (o->dwType & DIDFT_AXIS) { kind = 1; index = 7 + axis; }
            if (kind == 1) axis++;
            if (!kind) continue;
            d->fmt[d->nfmt].kind = kind; d->fmt[d->nfmt].index = index; d->fmt[d->nfmt].ofs = o->dwOfs;
            d->nfmt++;
        }
        logf("Controller: game data format %lu bytes, %d axes, %d buttons, %d POV (hr 0x%08lx)",
             df->dwDataSize, axis, button, pov, (unsigned long)hr);
        /* The game may never set a range; use whatever range DirectInput reports for the X axis. */
        for (i = 0; i < (DWORD)d->nfmt; i++)
            if (d->fmt[i].kind == 1 && d->fmt[i].index == 0) {
                DIPROPRANGE r;
                memset(&r, 0, sizeof(r));
                r.diph.dwSize = sizeof(r); r.diph.dwHeaderSize = sizeof(DIPROPHEADER);
                r.diph.dwHow = DIPH_BYOFFSET; r.diph.dwObj = d->fmt[i].ofs;
                if (SUCCEEDED(((F2)RV(s, 5))(REAL(s), (A)DIPROP_RANGE, (A)&r.diph)) && r.lMax > r.lMin) {
                    d->rmin = r.lMin; d->rmax = r.lMax;
                    logf("Controller: axis range %ld..%ld (from DirectInput)", r.lMin, r.lMax);
                }
                break;
            }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Dev_GetDeviceState(void *s, DWORD cb, LPVOID data)
{
    HRESULT hr = ((F2)RV(s, 9))(REAL(s), (A)cb, (A)data);
    XBDEV *d = (XBDEV *)s;
    XSTATE st;
    int i;
    if (!data || !d->nfmt || cb < d->dataSize || xb_read(&st) < 0) return hr;
    {
        const XGAMEPAD *g = &st.Gamepad;
        WORD b = g->wButtons;
        BYTE btn[16];
        LONG axes[7];
        LONG ly = -(LONG)g->sThumbLY - 1, ry = -(LONG)g->sThumbRY - 1;   /* XInput up = +, DirectInput up = - */
        if (ly < -32768) ly = -32768;
        if (ry < -32768) ry = -32768;
        axes[0] = xb_scale(g->sThumbLX, d);
        axes[1] = xb_scale(ly, d);
        axes[2] = xb_scale(0, d);
        axes[3] = xb_scale(g->sThumbRX, d);
        axes[4] = xb_scale(ry, d);
        axes[5] = xb_scale(0, d);
        axes[6] = xb_scale(0, d);
        btn[0] = (b & XB_A) != 0;      btn[1] = (b & XB_B) != 0;      btn[2] = (b & XB_X) != 0;
        btn[3] = (b & XB_Y) != 0;      btn[4] = (b & XB_LB) != 0;     btn[5] = (b & XB_RB) != 0;
        btn[6] = (b & XB_BACK) != 0;   btn[7] = (b & XB_START) != 0;  btn[8] = (b & XB_LTHUMB) != 0;
        btn[9] = (b & XB_RTHUMB) != 0;
        btn[10] = g->bLeftTrigger > g_xbThreshold;
        btn[11] = g->bRightTrigger > g_xbThreshold;
        btn[12] = (b & XB_DPAD_UP) != 0;   btn[13] = (b & XB_DPAD_RIGHT) != 0;
        btn[14] = (b & XB_DPAD_DOWN) != 0; btn[15] = (b & XB_DPAD_LEFT) != 0;
        xb_start_as_escape((b & XB_START) != 0);

        for (i = 0; i < d->nfmt; i++) {
            BYTE *p = (BYTE *)data + d->fmt[i].ofs;
            int ix = d->fmt[i].index;
            if (d->fmt[i].ofs >= cb) continue;
            if (d->fmt[i].kind == 1)      *(LONG *)p = ix < 7 ? axes[ix] : xb_scale(0, d);
            else if (d->fmt[i].kind == 2) *p = (ix < 16 && btn[ix]) ? 0x80 : 0;
            else if (d->fmt[i].kind == 3) *(DWORD *)p = 0xFFFFFFFF;   /* D-pad is buttons 13-16 */
        }
    }
    return DI_OK;   /* XInput state is valid even if DirectInput lost the device */
}

FWD3(Dev_EnumObjects, 4)
FWD2(Dev_GetProperty, 5)
FWD0(Dev_Acquire, 7)
FWD0(Dev_Unacquire, 8)
FWD4(Dev_GetDeviceData, 10)
FWD1(Dev_SetEventNotification, 12)
FWD2(Dev_SetCooperativeLevel, 13)
FWD3(Dev_GetObjectInfo, 14)
FWD1(Dev_GetDeviceInfo, 15)
FWD2(Dev_RunControlPanel, 16)
FWD3(Dev_Initialize, 17)
FWD4(Dev_CreateEffect, 18)
FWD3(Dev_EnumEffects, 19)
FWD2(Dev_GetEffectInfo, 20)
FWD1(Dev_GetForceFeedbackState, 21)
FWD1(Dev_SendForceFeedbackCommand, 22)
FWD3(Dev_EnumCreatedEffectObjects, 23)
FWD1(Dev_Escape, 24)
FWD0(Dev_Poll, 25)
FWD4(Dev_SendDeviceData, 26)
FWD4(Dev_EnumEffectsInFile, 27)
FWD4(Dev_WriteEffectToFile, 28)
FWD3(Dev_BuildActionMap, 29)
FWD3(Dev_SetActionMap, 30)
FWD1(Dev_GetImageInfo, 31)

static void *g_devVtbl[32] = {
    (void *)Dev_QueryInterface, (void *)Dev_AddRef, (void *)Dev_Release, (void *)Dev_GetCapabilities,
    (void *)Dev_EnumObjects, (void *)Dev_GetProperty, (void *)Dev_SetProperty, (void *)Dev_Acquire,
    (void *)Dev_Unacquire, (void *)Dev_GetDeviceState, (void *)Dev_GetDeviceData, (void *)Dev_SetDataFormat,
    (void *)Dev_SetEventNotification, (void *)Dev_SetCooperativeLevel, (void *)Dev_GetObjectInfo,
    (void *)Dev_GetDeviceInfo, (void *)Dev_RunControlPanel, (void *)Dev_Initialize, (void *)Dev_CreateEffect,
    (void *)Dev_EnumEffects, (void *)Dev_GetEffectInfo, (void *)Dev_GetForceFeedbackState,
    (void *)Dev_SendForceFeedbackCommand, (void *)Dev_EnumCreatedEffectObjects, (void *)Dev_Escape,
    (void *)Dev_Poll, (void *)Dev_SendDeviceData, (void *)Dev_EnumEffectsInFile, (void *)Dev_WriteEffectToFile,
    (void *)Dev_BuildActionMap, (void *)Dev_SetActionMap, (void *)Dev_GetImageInfo };

/* ------------------------------------------------ wrapped IDirectInput8 */
static int g_diIsW;

static HRESULT STDMETHODCALLTYPE DI_QueryInterface(void *s, REFIID riid, void **out)
{
    if (out && (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirectInput8A) ||
                IsEqualGUID(riid, &IID_IDirectInput8W))) {
        ((F0)RV(s, 1))(REAL(s));
        *out = s;
        return S_OK;
    }
    return ((F2)RV(s, 0))(REAL(s), (A)riid, (A)out);
}
static ULONG STDMETHODCALLTYPE DI_AddRef(void *s) { return (ULONG)((F0)RV(s, 1))(REAL(s)); }
static ULONG STDMETHODCALLTYPE DI_Release(void *s)
{
    ULONG n = (ULONG)((F0)RV(s, 2))(REAL(s));
    if (n == 0) free(s);
    return n;
}

static HRESULT STDMETHODCALLTYPE DI_CreateDevice(void *s, REFGUID guid, void **dev, void *outer)
{
    HRESULT hr = ((F3)RV(s, 3))(REAL(s), (A)guid, (A)dev, (A)outer);
    if (SUCCEEDED(hr) && dev && *dev) {
        void *real = *dev;
        DIDEVCAPS caps;
        BYTE type;
        memset(&caps, 0, sizeof(caps)); caps.dwSize = sizeof(caps);
        if (FAILED(((F1)((*(void ***)real)[3]))(real, (A)&caps))) return hr;
        type = (BYTE)(caps.dwDevType & 0xFF);
        if (type >= DI8DEVTYPE_JOYSTICK && type <= DI8DEVTYPE_1STPERSON) {
            XBDEV *d = (XBDEV *)calloc(1, sizeof(XBDEV));
            if (d) {
                d->vtbl = g_devVtbl; d->real = real; d->rmin = 0; d->rmax = 65535;
                *dev = d;
                logf("Controller: XInput bridge attached to game controller (DirectInput reports %lu axes, %lu buttons)",
                     caps.dwAxes, caps.dwButtons);
            }
        }
    }
    return hr;
}

FWD4(DI_EnumDevices, 4)
FWD1(DI_GetDeviceStatus, 5)
FWD2(DI_RunControlPanel, 6)
FWD2(DI_Initialize, 7)
FWD3(DI_FindDevice, 8)
FWD5(DI_EnumDevicesBySemantics, 9)
FWD4(DI_ConfigureDevices, 10)

static void *g_diVtbl[11] = {
    (void *)DI_QueryInterface, (void *)DI_AddRef, (void *)DI_Release, (void *)DI_CreateDevice,
    (void *)DI_EnumDevices, (void *)DI_GetDeviceStatus, (void *)DI_RunControlPanel, (void *)DI_Initialize,
    (void *)DI_FindDevice, (void *)DI_EnumDevicesBySemantics, (void *)DI_ConfigureDevices };

/* Called after the real DirectInput8Create succeeds; returns the object to hand to the game. */
static void *xb_wrap_dinput(void *di)
{
    static int initDone;
    WRAP *w;
    if (!initDone) {
        HMODULE x;
        initDone = 1;
        g_xbEnabled = GetPrivateProfileIntA("Controller", "XInput", 1, g_iniPath);
        g_xbThreshold = GetPrivateProfileIntA("Controller", "TriggerThreshold", 30, g_iniPath);
        g_xbStartEsc = GetPrivateProfileIntA("Controller", "StartIsEscape", 1, g_iniPath);
        if (!g_xbEnabled) { logf("Controller: XInput bridge disabled in ini"); return di; }
        x = LoadLibraryA("xinput1_4.dll");
        if (!x) x = LoadLibraryA("xinput1_3.dll");
        if (!x) x = LoadLibraryA("xinput9_1_0.dll");
        if (x) g_xget = (XInputGetState_t)GetProcAddress(x, "XInputGetState");
        if (!g_xget) { logf("Controller: XInput not available"); return di; }
    }
    if (!g_xbEnabled || !g_xget || !di) return di;
    w = (WRAP *)malloc(sizeof(WRAP));
    if (!w) return di;
    w->vtbl = g_diVtbl; w->real = di;
    return w;
}
