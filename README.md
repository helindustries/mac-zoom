# MacZoom

Smooth, cursor-following fullscreen zoom for Windows — the thing macOS does with
pinch-zoom, done natively at the DWM composition layer.

Native Win32 / C++, no .NET, no AutoHotkey, no code signing, no UIAccess manifest.
Single tray executable, roughly 100 KB.

## Why it exists

Windows Magnifier zooms in **discrete steps** (`ZoomIncrement`, default 100%,
minimum 5%) and pans by snapping when the cursor hits a screen edge. There is no
setting that makes it continuous. MacZoom drives the same underlying API directly
and **eases** the zoom level toward its target every frame, which is what turns it
from stepped into continuous.

## Controls

| | |
|---|---|
| `Alt` + Wheel | zoom in / out — multiplicative, ~15% per notch |
| `Alt` + Middle click | reset to 1.0 |
| Tray double-click | enable / disable |
| Tray right-click | menu: Enabled, Reset zoom, Exit |

## Build

```cmd
msbuild MacZoom.sln /p:Configuration=Release /p:Platform=x64
```

or just open `MacZoom.sln` in Visual Studio 2022.

Requires the MSVC v143 x64 toolset (VS 2022 BuildTools or Professional).
Output: `build\x64\Release\MacZoom.exe` — ~106 KB, static CRT, so no VC++
redistributable is needed on the target machine.

Note `WIN32_LEAN_AND_MEAN` is set project-wide, which means `timeBeginPeriod`
is **not** reachable through `windows.h`; `<timeapi.h>` is included explicitly.

## What was measured, and what it constrains

Every claim below was verified on the target machine rather than taken from
documentation or recollection. Several contradict the "common knowledge" about
this API.

### `MagSetFullscreenTransform` does NOT require UIAccess

Called from an ordinary, non-elevated process it returns `TRUE` and the transform
reads back correctly applied:

```
MagSetFullscreenTransform(1.08, 40, 20)  ->  True
MagGetFullscreenTransform                ->  level=1.08 xOff=40 yOff=20
```

So there is **no manifest, no self-signed certificate, no signtool step, and no
`Program Files` install requirement**. Much of the material written about this API
asserts otherwise. `MagSetInputTransform` *does* need UIAccess — which is why
MacZoom does not call it (see below).

### The coordinate space is the VIRTUAL DESKTOP, not the primary monitor

With primary `2560x1080` and virtual desktop `5120x1600`, an `xOffset` of `2760`
— 200 px inside the *second* display — was accepted unclamped. A primary-bound
implementation would have clamped it to ~1280.

This is why the clamping code uses `SM_XVIRTUALSCREEN` / `SM_CXVIRTUALSCREEN`
rather than `SM_CXSCREEN`.

### It is ONE GLOBAL TRANSFORM — per-monitor zoom is impossible

Pointing the source rect at an empty second display resulted in **that display's
background being shown across both monitors**. The transform applies to the whole
desktop as a single surface; it cannot address displays independently.

Windows Magnifier has exactly the same limitation, for exactly this reason.

> **Do not try to "add" per-monitor zoom.** The only way to achieve it is a
> magnifier-control overlay window per display (`MagSetWindowSource` +
> `MagSetWindowTransform`), which is a capture-and-redraw overlay rather than
> native compositing. That approach was considered and deliberately rejected.

### No `MagSetInputTransform`, deliberately

That call is UIAccess-gated. It is unnecessary here because the source rect is
**centred on the cursor**: the cursor sits at the visual centre of the magnified
view, so a click lands on whatever is drawn there and pointer/target stay aligned.

Input transforms are needed by *edge-panning* magnifiers, where the view and the
cursor diverge. This is not one.

## Implementation notes

- **Easing** (`EASE = 0.25`, ~120 Hz) is the feature. Snapping `current` straight
  to `target` would reproduce Magnifier's stepping exactly.
- `timeBeginPeriod(1)` is set for the frame loop — without it `Sleep(8)` drifts to
  ~15 ms and the motion visibly stutters.
- The transform is reset to `1.0` on exit, on `WM_ENDSESSION` and on
  `WM_QUERYENDSESSION`. **Leaving it set would leave the desktop permanently
  zoomed with no UI to undo it.**
- Zoom is multiplicative per notch, not additive — constant *perceptual* rate,
  which is what makes it feel like macOS rather than like Magnifier.
- Single-instance guarded by a named mutex: two instances would fight over one
  global transform.

## Interaction with other tooling

- **Replaces Windows Magnifier.** Only one fullscreen magnifier can own the
  transform. Do not run both.
- If an `Alt`+Wheel binding exists in an AutoHotkey script for the Magnifier
  (`Win`+`NumpadAdd`/`NumpadSub`), remove it or the two will fight.
- While streaming with Apollo, the virtual display is primary and part of the
  virtual desktop, so the magnified output is what gets captured — the client
  sees it already zoomed.
