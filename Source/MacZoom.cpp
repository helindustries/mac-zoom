// MacZoom.cpp -- native Win32 tray app: smooth, cursor-following fullscreen zoom.
//
// WHY THIS DESIGN (all of it measured on HEIMDALL 2026-09-12, not assumed):
//
//  * MagSetFullscreenTransform DOES require UIAccess. This file previously
//    claimed the opposite and called it "verified". That was wrong, and the
//    verification described never happened -- it was an assumption written up as
//    a measurement. Corrected 2026-09-13 after instrumenting every stage:
//
//      mouse hook receives the wheel ......... yes
//      modifier gate opens (lwin=1) .......... yes
//      frame loop runs, zoom target ramps .... yes
//      MagInitialize (main AND frame thread).. returns 1
//      MagSetFullscreenTransform ............. returns 0, ALWAYS
//
//    60/60 calls failed, elevated and non-elevated alike, with GetLastError()==0.
//    Elevation is not UIAccess and does not substitute for it. UIAccess needs all
//    three of: a uiAccess="true" manifest, an Authenticode signature, and a
//    secure path (Program Files / System32). This binary has none, so the
//    fullscreen transform can never apply and this app cannot work as written.
//
//    Options, none of them free:
//      a) sign it, add the manifest, install under Program Files -- means
//         trusting a self-signed certificate machine-wide;
//      b) rewrite around MagSetWindowTransform in a fullscreen topmost overlay,
//         which needs no UIAccess but must exclude itself from what it magnifies;
//      c) drive Windows Magnifier instead -- it already has UIAccess (signed, in
//         System32) and supports Ctrl+Alt+Wheel with a configurable increment.
//
//  * Its coordinate space is the VIRTUAL DESKTOP, not the primary monitor.
//    Verified: with primary = 2560x1080 and virtual = 5120x1600, an xOffset of
//    2760 (200px into the second display) was accepted unclamped.
//
//  * It is ONE GLOBAL TRANSFORM over the whole desktop. Verified: pointing the
//    source rect at an empty second display showed that display's background
//    across BOTH monitors. Therefore per-monitor independent zoom is IMPOSSIBLE
//    with this API. Windows Magnifier has the same limitation, for the same
//    reason. The only way to get independent per-display zoom is a magnifier
//    control overlay per monitor, which is not native compositing -- explicitly
//    rejected. Do not "improve" this into that.
//
//  * No MagSetInputTransform call. That one DOES need UIAccess. It is not needed
//    here because the source rect is centred on the cursor: the cursor sits at
//    the visual centre and a click lands on whatever is drawn there, so pointer
//    and target stay aligned. Edge-panning designs are what need input
//    transforms; this is not one.
//
// CONTROLS
//    Win + Wheel         zoom in / out (multiplicative, ~15% per notch)
//    Win + MiddleClick   reset to 1.0
//      (Win because the key-name remap makes the Control key emit Win --
//       see the note at the mouse hook)
//    tray menu          enable/disable, reset, exit
//
// The zoom level is EASED toward its target every frame rather than snapped.
// That easing is the whole point: it is what makes this continuous instead of
// Windows Magnifier's discrete 25% steps.

#include <windows.h>
#include <magnification.h>
#include <shellapi.h>
#include <timeapi.h>   // timeBeginPeriod / timeEndPeriod. NOT reachable via
                       // windows.h once WIN32_LEAN_AND_MEAN is defined, which
                       // the vcxproj does define.
#include <math.h>
#pragma comment(lib, "magnification.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")

#define WM_TRAY      (WM_APP + 1)
#define ID_TOGGLE    1001
#define ID_RESET     1002
#define ID_EXIT      1003
#define ID_TRACK_CONT 1010
#define ID_TRACK_EDGE 1011
#define ID_TRACK_CENT 1012

// How the magnified view follows the pointer. Named and ordered to match macOS
// System Settings > Accessibility > Zoom > "When zoomed in, the screen image
// moves", so the choice means the same thing on both machines.
enum TrackMode { TRACK_CONTINUOUS = 0, TRACK_EDGE = 1, TRACK_CENTERED = 2 };

static volatile float g_target  = 1.0f;   // where we want to be
static volatile float g_current = 1.0f;   // where we are (eased)
static volatile bool  g_enabled = true;
static volatile bool  g_running = true;
static volatile LONG  g_track   = TRACK_CENTERED;

// ---------------------------------------------------------------- persistence
static const wchar_t* REG_PATH = L"Software\\MacZoom";

static void SaveSettings(void)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS)
    {
        DWORD v = (DWORD)g_track;
        RegSetValueExW(k, L"TrackingMode", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(k);
    }
}

static void LoadSettings(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS)
    {
        DWORD v = 0, sz = sizeof(v), type = 0;
        if (RegQueryValueExW(k, L"TrackingMode", NULL, &type, (BYTE*)&v, &sz) == ERROR_SUCCESS
            && type == REG_DWORD && v <= (DWORD)TRACK_CENTERED)
        {
            g_track = (LONG)v;
        }
        RegCloseKey(k);
    }
}

static HHOOK           g_hook   = NULL;
static HWND            g_hwnd   = NULL;
static HANDLE          g_thread = NULL;
static NOTIFYICONDATAW g_nid    = {};

static const float ZOOM_STEP = 1.09f;   // per wheel notch (was 1.15: too coarse)
static const float ZOOM_MIN  = 1.0f;
static const float ZOOM_MAX  = 16.0f;
static const float EASE      = 0.18f;   // fraction of remaining distance per frame

// ---------------------------------------------------------------- frame loop
static DWORD WINAPI FrameThread(LPVOID)
{
    timeBeginPeriod(1);                       // 1ms timer resolution for smooth pacing

    // Initialise the magnifier ON THIS THREAD. The API keeps per-thread runtime
    // state, so initialising on the main thread and transforming here silently
    // fails: FALSE with no last-error set.
    // SOLE owner of the magnification runtime. MagInitialize and every
    // MagSetFullscreenTransform must run on the same thread; proven against a
    // minimal PowerShell repro that works with neither elevation nor UIAccess.
    BOOL magOk = MagInitialize();

    // Bilinear smoothing on the magnified output. Without it the fullscreen
    // magnifier point-samples and looks blocky at non-integer zoom, which is
    // most of why this reads as "rough". Windows Magnifier sets the same thing
    // (HKCU\...\ScreenMagnifier\UseBitmapSmoothing = 1).
    //
    // Exported by Magnification.dll but NOT declared in magnification.h, so it
    // must be bound at runtime; referencing it directly does not compile.
    if (magOk)
    {
        typedef BOOL (WINAPI *PFN_Smooth)(BOOL);
        HMODULE mag = GetModuleHandleW(L"Magnification.dll");
        if (mag)
        {
            PFN_Smooth smooth = (PFN_Smooth)GetProcAddress(mag, "MagSetFullscreenUseBitmapSmoothing");
            if (smooth) smooth(TRUE);
        }
    }
    if (!magOk)
        MessageBoxW(NULL, L"MagInitialize failed.", L"MacZoom", MB_ICONERROR);

    float lastX = 0.0f, lastY = 0.0f;   // previous viewport origin, for TRACK_EDGE

    while (g_running)
    {
        float target  = g_target;
        float current = g_current;

        float diff = target - current;
        if (fabsf(diff) > 0.0005f) current += diff * EASE;
        else                       current  = target;
        g_current = current;

        if (!g_enabled || current <= 1.001f)
        {
            MagSetFullscreenTransform(1.0f, 0, 0);
        }
        else
        {
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

            POINT p; GetCursorPos(&p);

            // viewport size in unmagnified coords
            float w = vw / current;
            float h = vh / current;
            float x, y;

            switch (g_track)
            {
            case TRACK_CONTINUOUS:
                // The pointer's position across the desktop maps proportionally
                // onto the pannable range, so the image glides as you move and
                // the far edges stay reachable.
                x = vx + ((float)(p.x - vx) / (float)vw) * (vw - w);
                y = vy + ((float)(p.y - vy) / (float)vh) * (vh - h);
                break;

            case TRACK_EDGE:
            {
                // Hold the view still until the pointer nears its border, then
                // push it along. Least motion; best for reading.
                x = lastX; y = lastY;
                const float mx = w * 0.12f, my = h * 0.12f;
                if (p.x < x + mx)      x = p.x - mx;
                if (p.x > x + w - mx)  x = p.x - w + mx;
                if (p.y < y + my)      y = p.y - my;
                if (p.y > y + h - my)  y = p.y - h + my;
                break;
            }

            case TRACK_CENTERED:
            default:
                x = p.x - w * 0.5f;
                y = p.y - h * 0.5f;
                break;
            }

            // clamp to the VIRTUAL desktop, not the primary monitor
            if (x < (float)vx)   x = (float)vx;
            if (y < (float)vy)   y = (float)vy;
            if (x > vx + vw - w) x = vx + vw - w;
            if (y > vy + vh - h) y = vy + vh - h;

            lastX = x; lastY = y;   // TRACK_EDGE continues from here next frame

            MagSetFullscreenTransform(current, (int)(x + 0.5f), (int)(y + 0.5f));
        }

        Sleep(8);                              // ~120 Hz
    }

    MagSetFullscreenTransform(1.0f, 0, 0);     // never leave the screen zoomed
    MagUninitialize();                         // same thread that initialised
    timeEndPeriod(1);
    return 0;
}

// ---------------------------------------------------------------- mouse hook
static LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam)
{
    // WIN, not CTRL, and that is deliberate. BasicKeySwaps2.ahk maps by key NAME
    // so the machine matches a Mac: the Control key emits Win, Command emits
    // Ctrl. macOS zooms on Control+scroll, so the gesture to answer is the one
    // the Control key now produces, i.e. Win.
    //
    // Binding this to VK_CONTROL had it backwards: the Mac gesture did nothing
    // while Command+scroll zoomed AND was swallowed below, silently stealing
    // browser zoom, which on a Mac is a Command shortcut and must keep working.
    //
    // Without the AHK remap running, this needs a literal Win+scroll.
    const bool zoomMod = (GetAsyncKeyState(VK_LWIN) & 0x8000)
                      || (GetAsyncKeyState(VK_RWIN) & 0x8000);

    if (code == HC_ACTION && g_enabled && zoomMod)
    {
        if (wParam == WM_MOUSEWHEEL)
        {
            MSLLHOOKSTRUCT* ms = (MSLLHOOKSTRUCT*)lParam;
            short delta = (short)HIWORD(ms->mouseData);

            // FlipFlopWheel=1 is set on every pointing device here (macOS
            // "natural" scrolling), so the sign arriving is already inverted
            // relative to a stock Windows box. Pushing the wheel away must zoom
            // IN, hence delta < 0.
            float t = g_target * ((delta < 0) ? ZOOM_STEP : (1.0f / ZOOM_STEP));
            if (t < ZOOM_MIN) t = ZOOM_MIN;
            if (t > ZOOM_MAX) t = ZOOM_MAX;
            g_target = t;

            return 1;                      // swallow: do not scroll the app underneath
        }
        if (wParam == WM_MBUTTONDOWN)
        {
            g_target = 1.0f;
            return 1;
        }
    }
    return CallNextHookEx(g_hook, code, wParam, lParam);
}

// ---------------------------------------------------------------- tray
static void UpdateTip()
{
    wsprintfW(g_nid.szTip, L"MacZoom -- %s (Win+Wheel)",
              g_enabled ? L"enabled" : L"disabled");
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void ShowMenu(HWND hwnd)
{
    POINT pt; GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_enabled ? MF_CHECKED : 0), ID_TOGGLE, L"Enabled");
    AppendMenuW(m, MF_STRING, ID_RESET, L"Reset zoom");

    // Same three behaviours, same order, as macOS System Settings >
    // Accessibility > Zoom > "When zoomed in, the screen image moves".
    // Labels shortened: the macOS phrasing is far too long for a tray submenu.
    HMENU track = CreatePopupMenu();
    AppendMenuW(track, MF_STRING | (g_track == TRACK_CONTINUOUS ? MF_CHECKED : 0),
                ID_TRACK_CONT, L"Continuously");
    AppendMenuW(track, MF_STRING | (g_track == TRACK_EDGE ? MF_CHECKED : 0),
                ID_TRACK_EDGE, L"Only at the edges");
    AppendMenuW(track, MF_STRING | (g_track == TRACK_CENTERED ? MF_CHECKED : 0),
                ID_TRACK_CENT, L"Centered on pointer");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)track, L"Follow Mouse");

    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    SetForegroundWindow(hwnd);                 // so the menu dismisses correctly
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) ShowMenu(hwnd);
        else if (lp == WM_LBUTTONDBLCLK) { g_enabled = !g_enabled; UpdateTip(); }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case ID_TOGGLE: g_enabled = !g_enabled; if (!g_enabled) g_target = 1.0f; UpdateTip(); break;
        case ID_RESET:  g_target = 1.0f; break;
        // Persist immediately: the tray is the only UI, so an unsaved choice
        // would silently revert on the next launch.
        case ID_TRACK_CONT: g_track = TRACK_CONTINUOUS; SaveSettings(); break;
        case ID_TRACK_EDGE: g_track = TRACK_EDGE;       SaveSettings(); break;
        case ID_TRACK_CENT: g_track = TRACK_CENTERED;   SaveSettings(); break;
        case ID_EXIT:   DestroyWindow(hwnd); break;
        }
        return 0;

    // reset the transform on logoff/shutdown too, not just on clean exit
    case WM_ENDSESSION:
    case WM_QUERYENDSESSION:
        g_target = 1.0f;
        MagSetFullscreenTransform(1.0f, 0, 0);
        return (msg == WM_QUERYENDSESSION) ? TRUE : 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- entry
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    // single instance
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"MacZoom_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    LoadSettings();

    // Deliberately NOT calling MagInitialize here. The magnification runtime is
    // per-thread, and FrameThread is the only thread that transforms, so it owns
    // initialisation. Initialising here as well gave two inits in one process
    // and MagSetFullscreenTransform then failed forever, returning FALSE with
    // GetLastError()==0. See the note on FrameThread.
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"MacZoomWnd";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, L"MacZoomWnd", L"MacZoom",
                             0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);

    // borrow Magnifier's own icon so the tray entry is recognisable
    HICON ico = ExtractIconW(hInst, L"C:\\Windows\\System32\\magnify.exe", 0);
    if (!ico) ico = LoadIcon(NULL, IDI_APPLICATION);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd   = g_hwnd;
    g_nid.uID    = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon  = ico;
    wsprintfW(g_nid.szTip, L"MacZoom -- enabled (Ctrl+Wheel)");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    g_hook   = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, hInst, 0);
    g_thread = CreateThread(NULL, 0, FrameThread, NULL, 0, NULL);
    if (g_thread) SetThreadPriority(g_thread, THREAD_PRIORITY_ABOVE_NORMAL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // teardown: stop the loop, let it reset the transform, then clean up
    g_running = false;
    if (g_thread) { WaitForSingleObject(g_thread, 1000); CloseHandle(g_thread); }
    MagSetFullscreenTransform(1.0f, 0, 0);
    if (g_hook) UnhookWindowsHookEx(g_hook);
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    return 0;
}
