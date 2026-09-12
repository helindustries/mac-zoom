// MacZoom.cpp -- native Win32 tray app: smooth, cursor-following fullscreen zoom.
//
// WHY THIS DESIGN (all of it measured on HEIMDALL 2026-09-12, not assumed):
//
//  * MagSetFullscreenTransform does NOT require UIAccess. Verified: called from a
//    plain non-elevated process it returns TRUE and the transform reads back
//    applied. So there is no manifest, no code signing and no Program Files
//    requirement. (An earlier assumption to the contrary was wrong.)
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

static volatile float g_target  = 1.0f;   // where we want to be
static volatile float g_current = 1.0f;   // where we are (eased)
static volatile bool  g_enabled = true;
static volatile bool  g_running = true;

static HHOOK           g_hook   = NULL;
static HWND            g_hwnd   = NULL;
static HANDLE          g_thread = NULL;
static NOTIFYICONDATAW g_nid    = {};

static const float ZOOM_STEP = 1.15f;   // per wheel notch
static const float ZOOM_MIN  = 1.0f;
static const float ZOOM_MAX  = 16.0f;
static const float EASE      = 0.25f;   // fraction of remaining distance per frame

// ---------------------------------------------------------------- frame loop
static DWORD WINAPI FrameThread(LPVOID)
{
    timeBeginPeriod(1);                       // 1ms timer resolution for smooth pacing

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

            // viewport size in unmagnified coords, centred on the cursor
            float w = vw / current;
            float h = vh / current;
            float x = p.x - w * 0.5f;
            float y = p.y - h * 0.5f;

            // clamp to the VIRTUAL desktop, not the primary monitor
            if (x < (float)vx)          x = (float)vx;
            if (y < (float)vy)          y = (float)vy;
            if (x > vx + vw - w)        x = vx + vw - w;
            if (y > vy + vh - h)        y = vy + vh - h;

            MagSetFullscreenTransform(current, (int)(x + 0.5f), (int)(y + 0.5f));
        }

        Sleep(8);                              // ~120 Hz
    }

    MagSetFullscreenTransform(1.0f, 0, 0);     // never leave the screen zoomed
    timeEndPeriod(1);
    return 0;
}

// ---------------------------------------------------------------- mouse hook
static LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam)
{
    // WIN, not CTRL -- and that is deliberate. BasicKeySwaps2.ahk maps by key
    // NAME so the machine matches a Mac: the Control key emits Win, and Command
    // emits Ctrl. macOS zooms the screen on Control+scroll, so the gesture we
    // must answer to is the one the Control key now produces, i.e. Win.
    //
    // Triggering on VK_CONTROL had it exactly backwards: the Mac gesture did
    // nothing, while Command+scroll zoomed AND was swallowed below -- silently
    // stealing browser zoom, which on a Mac is a Command shortcut and must keep
    // working.
    //
    // Consequence worth knowing: without the AHK remap running, this needs a
    // literal Win+scroll.
    const bool zoomMod = (GetAsyncKeyState(VK_LWIN) & 0x8000)
                      || (GetAsyncKeyState(VK_RWIN) & 0x8000);
    if (code == HC_ACTION && g_enabled && zoomMod)
    {
        if (wParam == WM_MOUSEWHEEL)
        {
            MSLLHOOKSTRUCT* ms = (MSLLHOOKSTRUCT*)lParam;
            short delta = (short)HIWORD(ms->mouseData);

            float t = g_target * ((delta > 0) ? ZOOM_STEP : (1.0f / ZOOM_STEP));
            if (t < ZOOM_MIN) t = ZOOM_MIN;
            if (t > ZOOM_MAX) t = ZOOM_MAX;
            g_target = t;

            return 1;                          // swallow: do not scroll the app underneath
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

    if (!MagInitialize())
    {
        MessageBoxW(NULL, L"MagInitialize failed.", L"MacZoom", MB_ICONERROR);
        return 1;
    }

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
    MagUninitialize();
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    return 0;
}
