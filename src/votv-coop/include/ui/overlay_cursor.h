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

// WHAT THE WARP IS FOR, AND THEREFORE WHEN IT MUST NOT HAPPEN (2026-08-26).
//
// Everything above is about handing the pointer back and forth WITH THE GAME, and MTA's
// own comment says why the exit recentre exists: "to prevent the game from reacting to
// its movement". The game only reacts to pointer movement while it is MOUSELOOKING -- and
// while it mouselooks it drives the pointer itself, ~118 times a second
// (`overlay_diag.h`'s own measured note). At the main menu it does no such thing: it
// shows a cursor and lets Slate track it, so there is nothing to hand back and a warp is
// pure damage.
//
// `captureActive` cannot tell those apart -- it means "an ImGui surface owns input",
// which is true at the menu too. Keying on it was harmless only while every interactive
// surface was ImGui. The native server browser breaks that: it holds no ImGui capture, so
// pressing its Host button (which opens the still-ImGui save picker) would flip capture
// mid-click and TELEPORT the pointer, then recentre it again when the picker closed.
//
// SO THE GATE IS THE WORLD: a transition warps only while `world_identity` reports a
// POSITIVE `WorldKind::Gameplay`. `Unknown` -- published for the whole boot window and
// for ~1 s across every travel -- never means "not in gameplay", so nothing fires on it.
// The memo is kept warm at 10 Hz by input_owner's refresh floor (input_owner.cpp:294-312),
// ungated and alive at the menu with no session, so this read is never stale.
//
// A gate on the PHENOMENON was designed first -- "warp only while the game is actually
// driving the pointer". The phenomenon is real (cursor_probe measured ~120 SetCursorPos
// calls/s in gameplay, 2026-08-26) but it cannot gate THIS edge: capture flips at the
// first present, before the game's recentre loop starts, so every measured transition saw
// everSeen=0 and a freshness gate would have skipped the first transition of every
// session. The observation survives as EVIDENCE (below), logged beside every decision.
//
// Called from the SetCursorPos detour (any thread), before the swallow -- a suppressed
// write is still a write. Cheap: one relaxed atomic store. Feeds the diagnostic printed
// with each transition, nothing else.
void NoteGameCursorWrite();

// Render thread, once per frame, BEFORE the frame's drawing reads the pointer.
// `captureActive` is the overlay's own "an interactive surface owns input" predicate;
// `origSetCursorPos` is the un-detoured entry (writes must bypass our own suppression,
// exactly as MTA's `CallSetCursorPos` does). `hwnd` supplies the client rect for the
// exit recentre. Does nothing at all while the state is unchanged, and NOTHING while the
// game is not driving the pointer (see above).
void FrameTransition(HWND hwnd, bool captureActive, SetCursorPosFn origSetCursorPos);

}  // namespace ui::overlay_cursor
