// ui/overlay_diag.h -- the overlay's DIAGNOSTIC instruments, off the hot file.
//
// Two instruments, both armed by environment variable and both silent otherwise:
//
//   * the KEY-MESSAGE trace (VOTVCOOP_INPUT_PROBE=1, shared arming with
//     coop/dev/input_focus_probe) -- one line per key message naming what arrived and
//     what the overlay decided to do with it. It exists because a keypress is a
//     SEQUENCE (KEYDOWN, then a WM_CHAR that `TranslateMessage` already queued in the
//     PUMP before `DispatchMessage` ever reached our detour) whose later members can be
//     decided by state an earlier member changed. Reasoning about one message hid that
//     for months; see docs/LESSONS.md and
//     research/findings/tooling/votv-input-ownership-FACTS-2026-07-31.md §8/M4.
//
//   * the CURSOR probe (VOTVCOOP_CURSOR_PROBE=1) -- one line per ~2s while a capturing
//     surface is up, naming every term of ImGui's own mouse-cursor draw guard, every
//     early-out of RenderMouseCursor, and the OS-side pointer/clip/raw-input state
//     around them. It rooted the "no cursor over the server browser" report: every
//     ImGui term is healthy and `io.MousePos` is the failing one, because the OS
//     pointer does not move.
//
// WHY IT LIVES HERE AND NOT IN THE OVERLAY: imgui_overlay.cpp owns the DXGI hooks, the
// WndProc and surface compositing. These two are instruments ABOUT that file, not part
// of it, and they pushed it past the 800-LOC soft cap. RULE-2 note: they are probes, so
// the "retire on replacement" rule does not reach them
// ([[feedback-rule2-exempts-probes-diagnostics-tools]]) -- but a probe still owes its
// own file once it is big enough to crowd the subsystem it watches.
//
// NO STATE REGISTRATION. Every gate these lines report (capture / chat / pause / the
// window / the original SetCursorPos) is passed in per call. A context struct handed
// over at init would be a second copy of the overlay's state with its own lifetime, and
// an instrument whose inputs can go stale reports a state nobody was in.

#pragma once

#include <windows.h>

namespace ui::overlay_diag {

// ---- counters (always on; each is one relaxed increment) ----------------------

// WndProc, every message. Counts WM_MOUSEMOVE / WM_INPUT / WM_SETCURSOR /
// WM_NCMOUSEMOVE; everything else is ignored. "Cursor over our foreground window and
// ZERO mouse messages" is a distinct diagnosis from "messages arrive, position ignored",
// and only a count can tell them apart.
void NoteWndProcMsg(UINT msg);

// The SetCursorPos detour, on EVERY call -- including the ones we no-op. The game
// recentres the pointer ~118x/s while playing; the count and the last coordinates are
// how the cursor line shows that the writes kept coming and the pointer still did not
// move.
void NoteSetCursorPos(int x, int y);

// ---- the key trace -----------------------------------------------------------

// The overlay's gate state at the moment of the decision. Passed rather than read:
// these predicates live in the overlay's anonymous namespace, and a key message is
// decided by the values that were true WHEN it arrived.
struct KeyGates {
    bool capture;  // an interactive surface owns input
    bool chat;     // our chat input is open
    bool pause;    // VOTV's own pause menu is up
};

// One line for one key message. Non-key messages are dropped HERE rather than at the
// call site: the overlay's swallow `switch` shares one body across the mouse and key
// cases, so the call site cannot filter without duplicating the case list.
void NoteKeyMsg(UINT msg, WPARAM wParam, const char* verdict, KeyGates gates);

// ---- the cursor probe --------------------------------------------------------

using SetCursorPosFn = BOOL(WINAPI*)(int, int);

// Call at the END of the frame, immediately before ImGui::Render() -- it must read
// exactly the state Render() itself will read. `origSetCursorPos` is the un-detoured
// entry, used ONLY by the positive control (VOTVCOOP_CURSOR_PROBE_WRITE=1).
void CursorFrame(HWND hwnd, bool captureActive, SetCursorPosFn origSetCursorPos);

}  // namespace ui::overlay_diag
