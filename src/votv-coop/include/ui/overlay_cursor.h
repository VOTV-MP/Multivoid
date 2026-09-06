// ui/overlay_cursor.h -- WHO owns the OS pointer position while a surface is up.
//
// The overlay suppresses the game's ~120 Hz `SetCursorPos` recentre while an interactive surface
// has input (imgui_overlay's `SetCursorPosDetour`) -- that is how ownership TRANSFERS, and
// without it the pointer is pinned to the window centre. What it does not give is either END of
// the transfer: close a surface with the pointer parked in a corner and the game resumes
// mouselook with the whole accumulated offset.
//
// WHY A TRANSITION AND NOT AN INVARIANT. A per-frame "put the pointer somewhere sane" check
// fights the player's own mouse, and would be a second compensation layer over a suppression
// that already works. Only a REAL transition acts, so the edge is safe to poll per frame: two
// surfaces handing off within one frame leave capture true throughout, and doing nothing is
// then correct.
//
// Not the fix for "no cursor showing" -- that one is `RebuildScaledStyle` in imgui_overlay.cpp.

#pragma once

#include <windows.h>

namespace ui::overlay_cursor {

using SetCursorPosFn = BOOL(WINAPI*)(int, int);

// The pointer-write observation. Called from the SetCursorPos detour (any thread), before the
// swallow -- a suppressed write is still a write. Cheap: one relaxed atomic store.
//
// It is EVIDENCE and never a gate; overlay_cursor.cpp says at the gate itself why the world,
// not this number, decides whether a transition may warp.
void NoteGameCursorWrite();

// Render thread, once per frame, BEFORE the frame's drawing reads the pointer. `captureActive`
// is the overlay's own "an interactive surface owns input" predicate; `origSetCursorPos` is
// the un-detoured entry (writes must bypass our own suppression, exactly as MTA's
// `CallSetCursorPos` does); `hwnd` supplies the client rect for the exit recentre.
//
// This performs two steps of MTA's transition (CLocalGUI::UpdateCursor, reached per frame from
// CLocalGUI::Draw): restore the stored position on ENTER, store it and recentre on EXIT. MTA's
// Enable/DisableSetCursorPos and ClearSystemKeys have no counterpart here, because the
// overlay's own detour owns suppression and keys on captureActive itself. Does nothing while
// the state is unchanged, and nothing at all outside gameplay -- see the world gate in
// overlay_cursor.cpp.
void FrameTransition(HWND hwnd, bool captureActive, SetCursorPosFn origSetCursorPos);

}  // namespace ui::overlay_cursor
