# MULTIPLAYER popup shows, no cursor — REPRODUCED, and the filed hypothesis is FALSE

> **SUPERSEDED 2026-07-31 — the ROOT IS FOUND. Read
> `research/findings/tooling/votv-input-ownership-FACTS-2026-07-31.md` §7 + §8 instead.**
> Root: cursor OWNERSHIP, not drawing. VOTV calls `SetCursorPos` ~118x/s at the window
> centre and our `SetCursorPosDetour` no-ops every one while a surface is up, so the OS
> pointer sits wherever the game last left it — in the failing run the client corner,
> where a 12x19 arrow is invisible. Every ImGui drawing term is healthy.
>
> **All three "where to look next" candidates below are FALSIFIED by measurement:**
> (1) `bd->hWnd == GetForegroundWindow() == g_hwnd` in every sample; (2) `MouseDrawCursor`
> reads 1 at the render site; (3) no DPI mismatch inside the process (`phys == osScreen`).
> The separate "PowerShell says (426,344), the game says (0,24)" anomaly was an ARTEFACT —
> the game process is `dpiAware=0` and PowerShell is not, so the two were never comparable.
> Still NOT FIXED; the fix is designed in the FACTS doc and did not ship 2026-07-31.
> Kept for its instrument design (the focus control) and its falsified-hypothesis record.

**Status: REPRODUCED autonomously 2026-07-28. Root FOUND 2026-07-31 (see the box above). Not fixed.**
Reported by the user 2026-07-27: *"clicking multiplayer shows multiplayer pop up but no CURSOR showing"*.

## What the previously-filed hypothesis said, and why it is wrong

The bug was filed with an inferred root, explicitly tagged NOT REPRODUCED:

> `CaptureActive()` (`imgui_overlay.cpp:129-138`) lists the surfaces that take cursor+input and
> `PauseMenuOpen()` is NOT among them, so with `CaptureActive()` false the `SetCursorPosDetour`
> does not swallow UE4's per-tick recenter.

**Both halves are falsified.**

1. The MULTIPLAYER button opens the **server browser** — `multiplayer_menu.cpp:251-252`,
   `"multiplayer_menu: MULTIPLAYER clicked -> opening server browser"` → `ui::server_browser::Open()`.
   And `BrowserOpen()` **is** in `CaptureActive()` (`imgui_overlay.cpp:134`). Capture is ON.
2. Measured with `GetCursorInfo` while the browser was up: the cursor position **does not move**
   (383,275 held across 1.5 s). The recenter *is* being swallowed. There is no recenter problem.

## What was actually measured

| measurement | result |
|---|---|
| `GetCursorInfo().flags` | **0** — i.e. NOT `CURSOR_SHOWING`. The OS cursor is hidden. |
| cursor position over 1.5 s | 383,275 → 383,275. Stable, so `SetCursorPosDetour` is active. |
| game window is foreground | **True** (verified before capturing — see the control below). |
| ImGui software cursor in the frame | **absent** (`research/cursor_probe/browser_focused_cursor.png`) |

The OS cursor being hidden is *by design* — `imgui_overlay.cpp:270` sets `io.MouseDrawCursor = true`
and `:349` re-sets it per frame to `CaptureActive()`, because "UE4 keeps the OS cursor hidden during
play". ImGui is supposed to draw its **own** software cursor. **It does not.** That is the bug.

## The instrument, and the control that makes it valid

- **A screenshot cannot see the OS cursor**: `tools/capture-window.ps1` does no `GetCursorInfo` /
  `DrawIconEx` compositing. It *can* see the ImGui software cursor, because that is drawn into the
  frame — which is exactly why the software-cursor question is the answerable one.
- **The focus control is mandatory.** ImGui's Win32 backend only updates `io.MousePos` when
  `GetForegroundWindow() == bd->hWnd`. An unattended capture therefore shows no cursor *whether or
  not the bug exists*. The first probe run hit exactly this and was discarded. The valid run
  asserts `foreground == game window? True` before moving the cursor and capturing.

Repro, no clicking required: launch with `VOTVCOOP_BROWSER_OPEN=1` (`imgui_overlay.cpp:588-594`),
`SetForegroundWindow`, `SetCursorPos` into the panel, capture. Note this skips the *click* path, so
it proves the browser-open STATE is enough — the pause-menu route is not required to trigger it.

## Where to look next (not yet done)

The remaining candidates, in order of cheapness:

1. **`io.MousePos` is never valid.** ImGui skips drawing the software cursor when the position is
   unset. The backend reads `GetCursorPos` + `ScreenToClient` against `bd->hWnd` — if the HWND
   `ImGui_ImplWin32_Init` was given is not the window that is actually foreground (VOTV creating
   more than one window, or the hook attaching to a splash/parent HWND), `is_app_focused` is false
   forever and `MousePos` never updates. **Log `io.MousePos` + `bd->hWnd` vs `GetForegroundWindow()`
   for one frame — that single line decides it.**
2. `MouseDrawCursor` being reset between `:349` and the ImGui render (some other surface writing it).
3. The cursor drawn outside the captured client rect (a scaling/DPI mismatch between
   `ScreenToClient` and the swapchain size).

Do (1) first: it is one log line and it discriminates all three.

## Related

- `feedback_show_screens` — this is why the answer came from photographing rather than reading.
- The lesson worth keeping regardless of the root: **an instrument that cannot see the phenomenon
  will always report "not present"**. The first capture here was indistinguishable from a pass.
