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
    void **vtbl; void *real;               /* must match WRAP; real = NULL for the virtual pad */
    LONG   refs;                            /* virtual pad only */
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

/* Records where the game wants each axis/button/POV in its state struct. */
static void xb_parse_format(XBDEV *d, LPCDIDATAFORMAT df, HRESULT hr)
{
    {
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
    }
}

static HRESULT STDMETHODCALLTYPE Dev_SetDataFormat(void *s, LPCDIDATAFORMAT df)
{
    HRESULT hr = ((F1)RV(s, 11))(REAL(s), (A)df);
    XBDEV *d = (XBDEV *)s;
    if (df) {
        DWORD i;
        xb_parse_format(d, df, hr);
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

/* Fills the game's state struct from XInput. Returns 0 if there's no pad (struct untouched). */
static int xb_fill(XBDEV *d, DWORD cb, LPVOID data)
{
    XSTATE st;
    int i;
    if (!data || !d->nfmt || cb < d->dataSize || xb_read(&st) < 0) return 0;
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
    return 1;
}

static HRESULT STDMETHODCALLTYPE Dev_GetDeviceState(void *s, DWORD cb, LPVOID data)
{
    HRESULT hr = ((F2)RV(s, 9))(REAL(s), (A)cb, (A)data);
    if (!xb_fill((XBDEV *)s, cb, data)) return hr;
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

/* ------------------------------------------------ virtual pad
 * Some XInput controllers (for example some ASUS ROG and other "Xbox-compatible" pads, or a pad
 * on a wireless dongle) never show up in DirectInput at all, so the game finds no controller and
 * the bridge above has nothing to attach to. In that case the enumeration gets one extra device,
 * "XInput Controller", which exists only in this DLL and is read entirely from XInput.
 * [Controller] VirtualPad=1 (default) adds it only when DirectInput has no game controller and
 * an XInput pad is connected; 2 = always add it when DirectInput has none; 3 = always add it,\n * even next to a DirectInput controller (for testing, or a pad whose DirectInput side is broken); 0 = never. */
static int g_xbVirtual = 1;
static int g_diIsW;
/* {5CDA0001-0000-4000-8000-5344415032500001} */
static const GUID GUID_XbVirtual = { 0x5cda0001, 0x0000, 0x4000, { 0x80, 0x00, 0x53, 0x44, 0x41, 0x50, 0x32, 0x50 } };
#define XB_VDEVTYPE (DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8))

static void xb_fill_instance(void *out, int wide)
{
    /* DIDEVICEINSTANCEA/W share the layout up to the names */
    DIDEVICEINSTANCEA *a = (DIDEVICEINSTANCEA *)out;
    DWORD size = wide ? sizeof(DIDEVICEINSTANCEW) : sizeof(DIDEVICEINSTANCEA);
    DWORD keep = a->dwSize && a->dwSize <= size ? a->dwSize : size;
    memset(out, 0, keep);
    a->dwSize = keep;
    a->guidInstance = GUID_XbVirtual;
    a->guidProduct = GUID_XbVirtual;
    a->dwDevType = XB_VDEVTYPE;
    if (wide) {
        DIDEVICEINSTANCEW *w = (DIDEVICEINSTANCEW *)out;
        if (keep >= sizeof(DIDEVICEINSTANCEW)) {
            wcscpy_s(w->tszInstanceName, MAX_PATH, L"XInput Controller");
            wcscpy_s(w->tszProductName, MAX_PATH, L"XInput Controller");
        }
    } else if (keep >= sizeof(DIDEVICEINSTANCEA)) {
        strcpy_s(a->tszInstanceName, MAX_PATH, "XInput Controller");
        strcpy_s(a->tszProductName, MAX_PATH, "XInput Controller");
    }
}

static HRESULT STDMETHODCALLTYPE V_QueryInterface(void *s, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirectInputDevice8A) ||
        IsEqualGUID(riid, &IID_IDirectInputDevice8W)) {
        InterlockedIncrement(&((XBDEV *)s)->refs);
        *out = s;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE V_AddRef(void *s) { return (ULONG)InterlockedIncrement(&((XBDEV *)s)->refs); }
static ULONG STDMETHODCALLTYPE V_Release(void *s)
{
    LONG n = InterlockedDecrement(&((XBDEV *)s)->refs);
    if (n == 0) free(s);
    return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE V_GetCapabilities(void *s, LPDIDEVCAPS caps)
{
    (void)s;
    if (!caps || caps->dwSize < sizeof(DIDEVCAPS_DX3)) return DIERR_INVALIDPARAM;
    { DWORD sz = caps->dwSize; memset(caps, 0, sz); caps->dwSize = sz; }
    caps->dwFlags = DIDC_ATTACHED | DIDC_EMULATED;
    caps->dwDevType = XB_VDEVTYPE;
    caps->dwAxes = 5;
    caps->dwButtons = 16;
    caps->dwPOVs = 0;
    return DI_OK;
}

/* objects: 5 axes (X, Y, Z, Rx, Ry) then 16 buttons */
static HRESULT STDMETHODCALLTYPE V_EnumObjects(void *s, A cbA, A ref, A flagsA)
{
    static const GUID *axisGuid[5] = { &GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis };
    static const char *axisName[5] = { "X Axis", "Y Axis", "Z Axis", "X Rotation", "Y Rotation" };
    typedef BOOL (CALLBACK *CB)(const void *, void *);
    CB cb = (CB)cbA;
    DWORD flags = (DWORD)flagsA;
    int i;
    (void)s;
    if (!cb) return DIERR_INVALIDPARAM;
    for (i = 0; i < 21; i++) {
        int isAxis = i < 5;
        DIDEVICEOBJECTINSTANCEW w;   /* the larger of A/W; A is filled in its first bytes */
        DIDEVICEOBJECTINSTANCEA *a = (DIDEVICEOBJECTINSTANCEA *)&w;
        char name[32];
        if (flags != DIDFT_ALL && !(flags & (isAxis ? DIDFT_AXIS : DIDFT_BUTTON))) continue;
        memset(&w, 0, sizeof(w));
        a->dwSize = g_diIsW ? sizeof(DIDEVICEOBJECTINSTANCEW) : sizeof(DIDEVICEOBJECTINSTANCEA);
        a->guidType = isAxis ? *axisGuid[i] : GUID_Button;
        a->dwOfs = isAxis ? (DWORD)(i * 4) : (DWORD)(48 + (i - 5));                  /* DIJOYSTATE offsets */
        a->dwType = isAxis ? (DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(i)) : (DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(i - 5));
        if (isAxis) strcpy_s(name, sizeof(name), axisName[i]);
        else sprintf_s(name, sizeof(name), "Button %d", i - 4);
        if (g_diIsW) MultiByteToWideChar(CP_ACP, 0, name, -1, w.tszName, MAX_PATH);
        else strcpy_s(a->tszName, MAX_PATH, name);
        if (cb(&w, (void *)ref) == DIENUM_STOP) break;
    }
    return DI_OK;
}

static HRESULT STDMETHODCALLTYPE V_GetProperty(void *s, REFGUID prop, LPDIPROPHEADER ph)
{
    XBDEV *d = (XBDEV *)s;
    if (!ph) return DIERR_INVALIDPARAM;
    if ((ULONG_PTR)prop == (ULONG_PTR)DIPROP_RANGE && ph->dwSize >= sizeof(DIPROPRANGE)) {
        ((DIPROPRANGE *)ph)->lMin = d->rmin; ((DIPROPRANGE *)ph)->lMax = d->rmax; return DI_OK;
    }
    if (((ULONG_PTR)prop == (ULONG_PTR)DIPROP_DEADZONE || (ULONG_PTR)prop == (ULONG_PTR)DIPROP_SATURATION ||
         (ULONG_PTR)prop == (ULONG_PTR)DIPROP_BUFFERSIZE) && ph->dwSize >= sizeof(DIPROPDWORD)) {
        ((DIPROPDWORD *)ph)->dwData = (ULONG_PTR)prop == (ULONG_PTR)DIPROP_SATURATION ? 10000 : 0; return DI_OK;
    }
    return DIERR_UNSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE V_SetProperty(void *s, REFGUID prop, LPCDIPROPHEADER ph)
{
    XBDEV *d = (XBDEV *)s;
    if (ph && (ULONG_PTR)prop == (ULONG_PTR)DIPROP_RANGE && ph->dwSize >= sizeof(DIPROPRANGE)) {
        const DIPROPRANGE *r = (const DIPROPRANGE *)ph;
        if (r->lMax > r->lMin) { d->rmin = r->lMin; d->rmax = r->lMax; logf("Controller: axis range %ld..%ld", r->lMin, r->lMax); }
    }
    return DI_OK;
}
static HRESULT STDMETHODCALLTYPE V_Ok0(void *s) { (void)s; return DI_OK; }
static HRESULT STDMETHODCALLTYPE V_Ok1(void *s, A a) { (void)s; (void)a; return DI_OK; }
static HRESULT STDMETHODCALLTYPE V_Ok2(void *s, A a, A b) { (void)s; (void)a; (void)b; return DI_OK; }
static HRESULT STDMETHODCALLTYPE V_Ok3(void *s, A a, A b, A c) { (void)s; (void)a; (void)b; (void)c; return DI_OK; }
static HRESULT STDMETHODCALLTYPE V_No1(void *s, A a) { (void)s; (void)a; return DIERR_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE V_No2(void *s, A a, A b) { (void)s; (void)a; (void)b; return DIERR_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE V_No3(void *s, A a, A b, A c) { (void)s; (void)a; (void)b; (void)c; return DIERR_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE V_No4(void *s, A a, A b, A c, A d) { (void)s; (void)a; (void)b; (void)c; (void)d; return DIERR_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE V_Poll(void *s) { (void)s; return DI_NOEFFECT; }

static HRESULT STDMETHODCALLTYPE V_GetDeviceState(void *s, DWORD cb, LPVOID data)
{
    XBDEV *d = (XBDEV *)s;
    int i;
    if (!data) return DIERR_INVALIDPARAM;
    if (xb_fill(d, cb, data)) return DI_OK;
    /* no pad right now: centred sticks, nothing pressed */
    memset(data, 0, cb);
    for (i = 0; i < d->nfmt; i++) {
        BYTE *p = (BYTE *)data + d->fmt[i].ofs;
        if (d->fmt[i].ofs >= cb) continue;
        if (d->fmt[i].kind == 1) *(LONG *)p = xb_scale(0, d);
        else if (d->fmt[i].kind == 3) *(DWORD *)p = 0xFFFFFFFF;
    }
    return DI_OK;
}
static HRESULT STDMETHODCALLTYPE V_GetDeviceData(void *s, DWORD cbObj, LPDIDEVICEOBJECTDATA od, LPDWORD inOut, DWORD flags)
{
    (void)s; (void)cbObj; (void)od; (void)flags;
    if (inOut) *inOut = 0;
    return DI_OK;
}
static HRESULT STDMETHODCALLTYPE V_SetDataFormat(void *s, LPCDIDATAFORMAT df)
{
    if (!df) return DIERR_INVALIDPARAM;
    xb_parse_format((XBDEV *)s, df, DI_OK);
    return DI_OK;
}
static HRESULT STDMETHODCALLTYPE V_GetDeviceInfo(void *s, A info)
{
    (void)s;
    if (!info) return DIERR_INVALIDPARAM;
    xb_fill_instance((void *)info, g_diIsW);
    return DI_OK;
}

static void *g_vdevVtbl[32] = {
    (void *)V_QueryInterface, (void *)V_AddRef, (void *)V_Release, (void *)V_GetCapabilities,
    (void *)V_EnumObjects, (void *)V_GetProperty, (void *)V_SetProperty, (void *)V_Ok0 /* Acquire */,
    (void *)V_Ok0 /* Unacquire */, (void *)V_GetDeviceState, (void *)V_GetDeviceData, (void *)V_SetDataFormat,
    (void *)V_Ok1 /* SetEventNotification */, (void *)V_Ok2 /* SetCooperativeLevel */, (void *)V_No3 /* GetObjectInfo */,
    (void *)V_GetDeviceInfo, (void *)V_Ok2 /* RunControlPanel */, (void *)V_Ok3 /* Initialize */, (void *)V_No4 /* CreateEffect */,
    (void *)V_Ok3 /* EnumEffects */, (void *)V_No2 /* GetEffectInfo */, (void *)V_No1 /* GetForceFeedbackState */,
    (void *)V_No1 /* SendForceFeedbackCommand */, (void *)V_Ok3 /* EnumCreatedEffectObjects */, (void *)V_No1 /* Escape */,
    (void *)V_Poll, (void *)V_No4 /* SendDeviceData */, (void *)V_No4 /* EnumEffectsInFile */, (void *)V_No4 /* WriteEffectToFile */,
    (void *)V_No3 /* BuildActionMap */, (void *)V_No3 /* SetActionMap */, (void *)V_No1 /* GetImageInfo */ };

static int xb_pad_connected(void)
{
    XSTATE st; DWORD i;
    if (!g_xget) return 0;
    for (i = 0; i < 4; i++) if (g_xget(i, &st) == ERROR_SUCCESS) return 1;
    return 0;
}

/* ------------------------------------------------ wrapped IDirectInput8 */

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
    HRESULT hr;
    if (guid && dev && IsEqualGUID(guid, &GUID_XbVirtual)) {
        XBDEV *d = (XBDEV *)calloc(1, sizeof(XBDEV));
        if (!d) return E_OUTOFMEMORY;
        d->vtbl = g_vdevVtbl; d->real = NULL; d->refs = 1; d->rmin = 0; d->rmax = 65535;
        *dev = d;
        logf("Controller: game opened the virtual XInput controller");
        return DI_OK;
    }
    hr = ((F3)RV(s, 3))(REAL(s), (A)guid, (A)dev, (A)outer);
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

typedef BOOL (CALLBACK *ENUMCB)(const void *inst, void *ref);
typedef struct { ENUMCB cb; void *ref; int gamectrl, stopped; } ENUMCTX;

static BOOL CALLBACK xb_enum_tramp(const void *inst, void *ctxp)
{
    ENUMCTX *c = (ENUMCTX *)ctxp;
    BYTE type = (BYTE)(((const DIDEVICEINSTANCEA *)inst)->dwDevType & 0xFF);
    if (type >= DI8DEVTYPE_JOYSTICK && type <= DI8DEVTYPE_1STPERSON) c->gamectrl = 1;
    if (c->cb(inst, c->ref) == DIENUM_STOP) { c->stopped = 1; return DIENUM_STOP; }
    return DIENUM_CONTINUE;
}

static HRESULT STDMETHODCALLTYPE DI_EnumDevices(void *s, A typeA, A cbA, A ref, A flagsA)
{
    static int logged;
    DWORD type = (DWORD)typeA, flags = (DWORD)flagsA;
    ENUMCTX c;
    HRESULT hr;
    int wantsPad = type == DI8DEVCLASS_ALL || type == DI8DEVCLASS_GAMECTRL ||
                   type == DI8DEVTYPE_GAMEPAD || type == DI8DEVTYPE_JOYSTICK;
    {
        static int seen;
        if (seen < 4) { seen++; logf("Controller: game lists DirectInput devices (type %lu, flags 0x%lx)", type, flags); }
    }
    if (!cbA || !g_xbVirtual || !wantsPad || (flags & (DIEDFL_FORCEFEEDBACK | DIEDFL_INCLUDEHIDDEN)))
        return ((F4)RV(s, 4))(REAL(s), typeA, cbA, ref, flagsA);
    c.cb = (ENUMCB)cbA; c.ref = (void *)ref; c.gamectrl = 0; c.stopped = 0;
    hr = ((F4)RV(s, 4))(REAL(s), typeA, (A)xb_enum_tramp, (A)&c, flagsA);
    if (FAILED(hr) || c.stopped || (c.gamectrl && g_xbVirtual != 3)) {
        if (c.gamectrl && !logged) { logged = 1; logf("Controller: DirectInput lists a game controller (no virtual pad needed)"); }
        return hr;
    }
    if (g_xbVirtual == 1 && !xb_pad_connected()) {
        if (!logged) { logged = 1; logf("Controller: no controller in DirectInput or XInput"); }
        return hr;
    }
    {
        DIDEVICEINSTANCEW inst;   /* big enough for either A or W */
        inst.dwSize = 0;
        xb_fill_instance(&inst, g_diIsW);
        if (logged != 2) { logged = 2; logf(c.gamectrl ? "Controller: offering the virtual XInput controller (VirtualPad=3)" : "Controller: DirectInput has no game controller; offering the virtual XInput controller"); }
        c.cb(&inst, c.ref);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DI_GetDeviceStatus(void *s, A guid)
{
    if (guid && IsEqualGUID((REFGUID)guid, &GUID_XbVirtual)) return xb_pad_connected() ? DI_OK : DI_NOTATTACHED;
    return ((F1)RV(s, 5))(REAL(s), guid);
}
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
static void *xb_wrap_dinput(void *di, REFIID riid)
{
    static int initDone;
    WRAP *w;
    g_diIsW = riid && IsEqualGUID(riid, &IID_IDirectInput8W);
    if (!initDone) {
        HMODULE x;
        initDone = 1;
        g_xbVirtual = GetPrivateProfileIntA("Controller", "VirtualPad", 1, g_iniPath);
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
    {
        static int created;
        if (!created++) logf("Controller: game created DirectInput (%s); waiting for it to look for a controller", g_diIsW ? "W" : "A");
    }
    w = (WRAP *)malloc(sizeof(WRAP));
    if (!w) return di;
    w->vtbl = g_diVtbl; w->real = di;
    return w;
}
