// MacZoom.cpp -- native Win32 tray app: smooth, cursor-following fullscreen zoom.
//
// WHY THIS DESIGN (all of it measured on HEIMDALL 2026-09-12, not assumed):
//
//  * MagSetFullscreenTransform does NOT require UIAccess. A minimal PowerShell
//    repro calls it successfully from a plain, non-elevated process with no
//    manifest flag. The failures once blamed on UIAccess -- 60/60 calls returning
//    FALSE with GetLastError()==0 -- came from initialising the magnifier on one
//    thread and transforming on another; see the note on FrameThread.
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
//    tray menu          enable/disable, reset, follow mouse, follow text cursor, exit
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
#include <stdlib.h>
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
#define ID_FOLLOW_TEXT 1020

// How the magnified view follows the pointer. Named and ordered to match macOS
// System Settings > Accessibility > Zoom > "When zoomed in, the screen image
// moves", so the choice means the same thing on both machines.
enum TrackMode { TRACK_CONTINUOUS = 0, TRACK_EDGE = 1, TRACK_CENTERED = 2 };

static volatile float g_target  = 1.0f;   // where we want to be
static volatile float g_current = 1.0f;   // where we are (eased)
static volatile bool  g_enabled = true;   // the user's tray toggle
static volatile bool  g_running = true;
static volatile LONG  g_track   = TRACK_CENTERED;
static volatile bool  g_followText = false;   // tray option: follow the text caret while typing

// Suppressed because a stream is running. DELIBERATELY SEPARATE from g_enabled:
// macOS does its own screen zoom CLIENT-side, before anything reaches Windows,
// so zooming here as well would zoom twice. Keeping the two flags apart means a
// stream starting and ending does not silently discard the user's own on/off
// choice -- exactly the same reasoning as MacKeySwaps.ahk, which suppresses its
// magnifier bindings while streaming rather than rewriting them.
static volatile bool  g_streamOff = false;

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
        DWORD text = g_followText ? 1 : 0;
        RegSetValueExW(k, L"FollowText", 0, REG_DWORD, (const BYTE*)&text, sizeof(text));
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
        DWORD text = 0;
        sz = sizeof(text);
        if (RegQueryValueExW(k, L"FollowText", NULL, &type, (BYTE*)&text, &sz) == ERROR_SUCCESS
            && type == REG_DWORD)
        {
            g_followText = text != 0;
        }
        RegCloseKey(k);
    }
}

// ---------------------------------------------------------- stream signalling
// CONTRACT (shared with MacKeySwaps.ahk and anything else that cares):
//
//     HKLM\SOFTWARE\MacParity : StreamActive  REG_DWORD  0 = local, 1 = streaming
//
// Apollo's global_prep_cmd writes it from stream-on.cmd / stream-off.cmd with a
// plain `reg add`, and we subscribe with RegNotifyChangeKeyValue.
//
// WHY THIS AND NOT THE OBVIOUS ALTERNATIVES
// Apollo's prep command runs as SYSTEM in session 0 and exits immediately, so:
//   * PostMessage(HWND_BROADCAST, ...) cannot reach us -- window broadcasts do
//     not cross the session boundary, and we live in the interactive session.
//   * a named Event or Mutex is destroyed when the last handle closes, i.e. the
//     instant the prep command exits, so it cannot carry state for a whole
//     stream.
//   * a fire-and-forget pipe write is LOST if nothing is listening at that
//     moment, and gives a process starting mid-stream no way to learn the
//     current state -- a real case when autostart races Apollo at logon.
//
// A registry value solves all three: the write is atomic (no torn reads, no
// sharing violations, no text parsing), the state is durable so a late starter
// reads the truth, RegNotifyChangeKeyValue is a genuine kernel push rather than
// a poll, and any number of consumers can subscribe independently.
#define WM_STREAMSTATE (WM_APP + 2)

// Declared up here rather than with the other handles below, because
// StreamWatchThread posts to it.
static HWND g_hwnd = NULL;

static const wchar_t* STATE_KEY = L"SOFTWARE\\MacParity";
static const wchar_t* STATE_VAL = L"StreamActive";

static HANDLE g_quitEvt   = NULL;   // signalled at teardown to release the watcher
static HANDLE g_watchThrd = NULL;

static bool ReadStreamActive(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, STATE_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return false;                       // absent => not streaming, the safe default

    DWORD v = 0, sz = sizeof(v), type = 0;
    LONG  r = RegQueryValueExW(k, STATE_VAL, NULL, &type, (BYTE*)&v, &sz);
    RegCloseKey(k);
    return (r == ERROR_SUCCESS && type == REG_DWORD && v != 0);
}

// Blocks in the kernel until the value changes; no polling anywhere.
static DWORD WINAPI StreamWatchThread(LPVOID)
{
    for (;;)
    {
        HKEY k;
        // KEY_NOTIFY is part of KEY_READ, which every user holds on HKLM\SOFTWARE.
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, STATE_KEY, 0,
                          KEY_NOTIFY | KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        {
            // Key not created yet (fresh install, installer not run). Retry
            // rather than giving up for the lifetime of the process.
            if (WaitForSingleObject(g_quitEvt, 5000) == WAIT_OBJECT_0) return 0;
            continue;
        }

        HANDLE evt = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!evt) { RegCloseKey(k); return 0; }
        HANDLE h[2] = { evt, g_quitEvt };

        for (;;)
        {
            ResetEvent(evt);
            // Re-arm BEFORE reading, so a change landing between the read and
            // the next arm is not missed.
            if (RegNotifyChangeKeyValue(k, FALSE, REG_NOTIFY_CHANGE_LAST_SET, evt, TRUE) != ERROR_SUCCESS)
                break;
            if (WaitForMultipleObjects(2, h, FALSE, INFINITE) != WAIT_OBJECT_0)
            {
                CloseHandle(evt); RegCloseKey(k);
                return 0;                   // quit signalled
            }
            PostMessageW(g_hwnd, WM_STREAMSTATE, 0, 0);
        }

        CloseHandle(evt);
        RegCloseKey(k);
        if (WaitForSingleObject(g_quitEvt, 1000) == WAIT_OBJECT_0) return 0;
    }
}

// Zoom answers the wheel only when the user has it on AND no stream is running.
static inline bool ZoomOn(void) { return g_enabled && !g_streamOff; }

static HHOOK           g_hook   = NULL;
static HANDLE          g_thread = NULL;
static NOTIFYICONDATAW g_nid    = {};

static const float ZOOM_STEP = 1.09f;   // per wheel notch (was 1.15: too coarse)
static const float ZOOM_MIN  = 1.0f;
static const float ZOOM_MAX  = 16.0f;
static const float EASE      = 0.18f;   // fraction of remaining distance per frame

// ---------------------------------------------------------------- text caret
// "Follow Text Cursor": while typing, the view follows the text caret the way macOS
// Zoom follows keyboard focus, and moving the pointer hands the view back. The
// caret is the system caret, which Win32 edit controls and most editors maintain for
// accessibility tools; its owner reports every move as a caret location change, and
// the position is read from that thread's GUI state. Applications that draw their
// own cursor without a system caret are not followed.
static HWINEVENTHOOK g_caretHook = NULL;
static volatile bool g_onCaret   = false;    // the view currently follows the caret
static volatile LONG g_caretX    = 0;        // caret centre, screen coordinates
static volatile LONG g_caretY    = 0;
static volatile LONG g_anchorX   = 0;        // pointer position when the caret took over
static volatile LONG g_anchorY   = 0;
static const LONG    POINTER_TAKEOVER = 8;   // pixels the pointer must move to take the view back

static void CALLBACK CaretProc(HWINEVENTHOOK, DWORD, HWND, LONG idObject, LONG, DWORD thread, DWORD)
{
    if (idObject != OBJID_CARET || !g_followText || !ZoomOn()) return;

    GUITHREADINFO gti = {};
    gti.cbSize = sizeof(gti);
    if (GetGUIThreadInfo(thread, &gti) && gti.hwndCaret && gti.rcCaret.bottom > gti.rcCaret.top)
    {
        POINT c = { (gti.rcCaret.left + gti.rcCaret.right) / 2,
                    (gti.rcCaret.top + gti.rcCaret.bottom) / 2 };
        ClientToScreen(gti.hwndCaret, &c);
        POINT p; GetCursorPos(&p);
        g_caretX  = c.x;  g_caretY  = c.y;
        g_anchorX = p.x;  g_anchorY = p.y;
        g_onCaret = true;
    }
}

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

        if (!ZoomOn() || current <= 1.001f)
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
            if (g_onCaret) { p.x = g_caretX; p.y = g_caretY; }   // every tracking mode follows it

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

    // The pointer takes the view back from the text caret once it has really
    // moved, not on the jitter of a hand resting on the mouse.
    if (code == HC_ACTION && wParam == WM_MOUSEMOVE && g_onCaret)
    {
        const MSLLHOOKSTRUCT* ms = (const MSLLHOOKSTRUCT*)lParam;
        if (labs(ms->pt.x - g_anchorX) > POINTER_TAKEOVER
            || labs(ms->pt.y - g_anchorY) > POINTER_TAKEOVER)
            g_onCaret = false;
    }

    // ZoomOn(), not g_enabled: while a stream is up the wheel must fall through
    // to the application underneath, NOT be swallowed by the return 1 below.
    if (code == HC_ACTION && ZoomOn() && zoomMod)
    {
        if (wParam == WM_MOUSEWHEEL)
        {
            MSLLHOOKSTRUCT* ms = (MSLLHOOKSTRUCT*)lParam;
            short delta = (short)HIWORD(ms->mouseData);

            // FlipFlopWheel=1 is set on every pointing device here (macOS
            // "natural" scrolling), so the sign arriving is already inverted
            // relative to a stock Windows box. Pushing the wheel away must zoom
            // IN, hence a negative delta zooms in.
            //
            // Scale by how far this event scrolled, NOT by how many events
            // arrived. A wheel notch is WHEEL_DELTA (120) per event, but a
            // precision touchpad sends a rapid stream of small deltas for one
            // swipe. Applying a whole ZOOM_STEP per event made touchpad zoom race
            // while the MX Master felt right. With the exponent, one notch is
            // still exactly ZOOM_STEP and small deltas contribute their fraction,
            // so both devices zoom the same amount for the same scroll distance.
            float t = g_target * powf(ZOOM_STEP, -(float)delta / (float)WHEEL_DELTA);
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
    const wchar_t* state = g_streamOff ? L"suppressed (streaming)"
                                       : (g_enabled ? L"enabled" : L"disabled");
    wsprintfW(g_nid.szTip, L"MacZoom -- %s (Win+Wheel)", state);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void ShowMenu(HWND hwnd)
{
    POINT pt; GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    // The tick still reflects the user's own choice while a stream suppresses
    // it, so ending the stream restores exactly what they had set.
    AppendMenuW(m, MF_STRING | (g_enabled ? MF_CHECKED : 0), ID_TOGGLE,
                g_streamOff ? L"Enabled (suppressed while streaming)" : L"Enabled");
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
    AppendMenuW(m, MF_STRING | (g_followText ? MF_CHECKED : 0), ID_FOLLOW_TEXT,
                L"Follow Text Cursor");

    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    SetForegroundWindow(hwnd);                 // so the menu dismisses correctly
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// Explorer broadcasts "TaskbarCreated" whenever it builds the taskbar: at logon,
// and again after an Explorer crash or restart. Every tray icon is gone at that
// point and each application must add its own back. MacZoom used to add its icon
// exactly once at startup, so when a logon task launched it before the taskbar
// existed the icon was silently lost for the whole session.
static UINT g_wmTaskbarCreated = 0;
#define ID_TIMER_TRAY 2

// Adds the tray icon, retrying every 2 s while the shell is not ready. NIM_MODIFY
// as the fallback treats "icon already present" as success, so a duplicate add
// cannot leave the retry timer running forever.
static void AddTrayIcon(HWND hwnd)
{
    if (Shell_NotifyIconW(NIM_ADD, &g_nid) || Shell_NotifyIconW(NIM_MODIFY, &g_nid))
    {
        KillTimer(hwnd, ID_TIMER_TRAY);
        UpdateTip();
    }
    else
    {
        SetTimer(hwnd, ID_TIMER_TRAY, 2000, NULL);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // Registered message ids are not compile-time constants, so this cannot be a case.
    if (g_wmTaskbarCreated && msg == g_wmTaskbarCreated)
    {
        AddTrayIcon(hwnd);
        return 0;
    }

    switch (msg)
    {
    case WM_TIMER:
        if (wp == ID_TIMER_TRAY) AddTrayIcon(hwnd);
        return 0;

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
        case ID_FOLLOW_TEXT:
            g_followText = !g_followText;
            if (!g_followText) g_onCaret = false;
            SaveSettings();
            break;
        case ID_EXIT:   DestroyWindow(hwnd); break;
        }
        return 0;

    // Pushed by StreamWatchThread the moment HKLM\SOFTWARE\MacParity changes.
    // Re-read rather than trusting a payload: the value is the single source of
    // truth, and coalesced notifications would otherwise be ambiguous.
    case WM_STREAMSTATE:
    {
        bool s = ReadStreamActive();
        if (s != g_streamOff)
        {
            g_streamOff = s;
            // Collapse immediately when a stream starts, otherwise the Mac
            // would be magnifying an already-magnified desktop.
            if (s) g_target = 1.0f;
            UpdateTip();
        }
        return 0;
    }

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

    // The mouse hook and the frame loop must keep running while the machine is
    // loaded: a low-level hook that is starved stalls the pointer for everyone,
    // and a late MagSetFullscreenTransform leaves the zoom stuck behind it. Task
    // Scheduler starts its tasks BELOW_NORMAL, and Windows puts a process without
    // a visible window under EcoQoS (efficiency cores at reduced clock) whenever
    // it likes; neither is acceptable here, so both are overridden. The transform
    // itself is carried out by dwm.exe, which Windows already runs at HIGH.
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    PROCESS_POWER_THROTTLING_STATE throttling = {};
    throttling.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask   = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));

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

    // Registered BEFORE the window exists, so the first broadcast is not missed.
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // A hidden top-level tool window, NOT a message-only (HWND_MESSAGE) window.
    // Message-only windows never receive broadcasts, so neither TaskbarCreated
    // nor WM_QUERYENDSESSION / WM_ENDSESSION could ever reach the old window -
    // which also means the "reset the transform on logoff" case in WndProc was
    // dead code until now. WS_POPUP without WS_VISIBLE is never shown, and
    // WS_EX_TOOLWINDOW keeps it out of Alt+Tab and the taskbar regardless.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"MacZoomWnd", L"MacZoom",
                             WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInst, NULL);

    // Explorer runs at medium integrity. Should MacZoom ever be started elevated,
    // UIPI would silently drop the broadcast without this.
    if (g_wmTaskbarCreated)
        ChangeWindowMessageFilterEx(g_hwnd, g_wmTaskbarCreated, MSGFLT_ALLOW, NULL);

    // borrow Magnifier's own icon so the tray entry is recognisable
    HICON ico = ExtractIconW(hInst, L"C:\\Windows\\System32\\magnify.exe", 0);
    if (!ico) ico = LoadIcon(NULL, IDI_APPLICATION);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd   = g_hwnd;
    g_nid.uID    = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon  = ico;
    wsprintfW(g_nid.szTip, L"MacZoom");
    AddTrayIcon(g_hwnd);   // retries every 2 s until the shell accepts it

    // Read the state BEFORE the hook goes in, so starting mid-stream (autostart
    // racing Apollo, or a crash-restart) comes up suppressed rather than fighting
    // the Mac's own zoom for a moment. UpdateTip needs the icon to exist already.
    g_streamOff = ReadStreamActive();
    UpdateTip();

    g_quitEvt   = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_watchThrd = CreateThread(NULL, 0, StreamWatchThread, NULL, 0, NULL);

    g_hook   = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, hInst, 0);
    // Out of context, so nothing is injected into other processes: the callback
    // runs on this thread, from the message loop below.
    g_caretHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                                  NULL, CaretProc, 0, 0,
                                  WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
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
    // Release the watcher out of its INFINITE wait before joining it.
    if (g_quitEvt) SetEvent(g_quitEvt);
    if (g_watchThrd) { WaitForSingleObject(g_watchThrd, 2000); CloseHandle(g_watchThrd); }
    if (g_quitEvt) CloseHandle(g_quitEvt);
    if (g_thread) { WaitForSingleObject(g_thread, 1000); CloseHandle(g_thread); }
    MagSetFullscreenTransform(1.0f, 0, 0);
    if (g_hook) UnhookWindowsHookEx(g_hook);
    if (g_caretHook) UnhookWinEvent(g_caretHook);
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    return 0;
}
