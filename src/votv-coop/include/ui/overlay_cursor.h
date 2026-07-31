// ui/overlay_cursor.h -- WHO owns the OS pointer position while a surface is up.
//
// The overlay already suppresses the game's ~120 Hz `SetCursorPos` recentre while an
// interactive surface has input (imgui_overlay's `SetCursorPosDetour`). That is how
// ownership TRANSFERS -- without it the pointer is pinned to the window centre and cannot
// track the mouse. What was missing is both ENDS of the transfer, and the user found the
// second one by hand (2026-07-31): *"when I closed the multiplayer screen and went into
// ESC the cursor was acting weird and not respecting my mouse movements."*
//
// MTA has shipped the answer for fifteen years (RULE 2026-05-28 -- follow MTA architecture
// when possible). `CLocalGUI::Draw`, `reference/mtasa-blue/Client/core/CGUI.cpp:800-848`,
// runs a per-frame transition:
//
//   ENTERING GUI mode:  SetCursorPos(m_StoredMousePosition);   // restore where it was
//                       DisableSetCursorPos();                 // then suppress the game
//   LEAVING  GUI mode:  GetCursorPos(&p); m_StoredMousePosition = p;
//                       SetCursorPos(width/2, height/2);       // MTA's own comment:
//                       EnableSetCursorPos();                  //   "to prevent the game
//                       pGUI->ClearSystemKeys();               //    from reacting to its
//                                                              //    movement"
//
// The EXIT recentre is the part that answers the user's report: leaving the pointer
// wherever the player parked it means the game resumes mouselook with a large accumulated
// offset. The ENTER restore is why MTA's cursor appears where you left it instead of
// wherever the game last recentred to.
//
// WHY A TRANSITION AND NOT AN INVARIANT. A per-frame "put the pointer somewhere sane"
// check was designed and rejected: it fights the player's own mouse, and it would be a
// second compensation layer over a suppression that already works. The edge is safe to
// poll per frame precisely because only a REAL transition acts -- two surfaces handing off
// within one frame leaves capture true throughout, and doing nothing is then correct.
//
// This is NOT the fix for "no cursor showing". That was `ImGuiStyle::ScaleAllSizes`
// truncating `MouseCursorScale` to 0 -- see `RebuildScaledStyle` in imgui_overlay.cpp.
// Two symptoms, two roots; conflating them cost this arc several rounds.

#pragma once

#include <windows.h>

namespace ui::overlay_cursor {

using SetCursorPosFn = BOOL(WINAPI*)(int, int);

// Render thread, once per frame, BEFORE the frame's drawing reads the pointer.
// `captureActive` is the overlay's own "an interactive surface owns input" predicate;
// `origSetCursorPos` is the un-detoured entry (writes must bypass our own suppression,
// exactly as MTA's `CallSetCursorPos` does). `hwnd` supplies the client rect for the
// exit recentre. Does nothing at all while the state is unchanged.
void FrameTransition(HWND hwnd, bool captureActive, SetCursorPosFn origSetCursorPos);

}  // namespace ui::overlay_cursor
