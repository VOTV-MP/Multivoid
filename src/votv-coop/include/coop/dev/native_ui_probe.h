// coop/dev/native_ui_probe.h -- P1 of the native server browser (docs/MULTIPLAYER_UI.md
// section 8). The MEASUREMENT that comes before the ~700 LOC of P2.
//
// Every question this answers is one the converged 11-round /qf could not settle by
// reading code, and each is load-bearing on a decision the design has already made:
//
//   RUNG 0  lives in coop/dev/worldless_frames.h -- the same arming row, a different
//           thread and call site, and nothing shared. It counts frames presented per
//           WorldKind, which is the whole of question O4: a UMG surface needs a world
//           to draw into and an ImGui Present hook does not.
//
//   O1      Does each UMG UFunction the browser needs RESOLVE -- on its OWNING class?
//           R::FindFunction has no super-walk (reflection.cpp:493), so `AddChild` lives
//           on UPanelWidget and resolving `ScrollBox` buys nothing by itself. A miss
//           must be known now, not at draw time.
//
//   O5      Is a donor brush's FSlateResourceHandle (the 16 UNREFLECTED bytes at
//           FSlateBrush+0x70) POPULATED at inject time? InjectCanvasButton memcpys
//           0x278 bytes of FButtonStyle, which spans four brushes; if the handle is
//           live we are aliasing a refcounted pointer with no AddRef. This gates P0 and
//           nothing else -- a null handle means there is no bug and the one
//           hands-on-verified inject is not touched at all.
//
//   O7      Donor RESIDENCY: which of section 8's style donors actually exist, and are
//           non-null, at MAIN-MENU time. Fail-closed styling is only diagnosable if we
//           know which donor is missing.
//
//   O8      The delegate layout -- read-only. UButton::OnClicked is a multicast
//           delegate at +0x3C8 (UMG.hpp:294); reading the GAME's own bound button
//           proves the offset and proves the game binds there, without writing
//           anything. (v1 polls; this is the evidence the v2 retire-the-poll call
//           needs.)
//
//   RUNG 1  Does a hand-wired, never-Initialize()d UUserWidget RENDER inside a
//           UWidgetSwitcher? AddToViewport is the only path ever proven (pos_hud). If
//           it renders nowhere, the 12th-child placement dies and the browser falls
//           back to AddToViewport like every other surface we ship. THIS RUNG WRITES --
//           see the note on its own gate below.
//
// WHY MENU-TIME AND NOT BOOT. Corrected in /qf round 7: every donor is a live-menu
// widget and the inject fires from the ui_menu Tick observer. At boot there is no
// ui_menu, so a boot-time run reads null for all of them -- and that null is
// indistinguishable from "no handle, no bug, donors absent". The probe therefore rides
// the same anchor the inject does, and stamps which menu instance it sampled.
//
// TWO GATES, ON PURPOSE. `[dev] native_ui_probe=1` arms the reads + rung 0 and touches
// nothing. `[dev] native_ui_probe_write=1` additionally arms RUNG 1, which AddChilds
// into the live shipped `switcher_widgets` and moves ActiveWidgetIndex -- inside the one
// native inject this mod has hands-on verified. It reads the prior index back, restores
// it (only if the index is still ours -- the game may have navigated), removes the
// throwaway, and is bounded by a deadline rather than by anything the user must do:
// per section 8's ESC finding, at our index ESC is a no-op and a throwaway has no
// button_back, so a probe that could outlive its own deadline could strand the player in
// a menu with no way out.

#pragma once

namespace coop::dev::native_ui_probe {

// Arm off the config rows and register the ui_menu_C::Tick post-observer (bounded
// retry -- the menu BP may not be loaded when this runs at boot). Idempotent; a no-op
// when disarmed. Call from the harness dev-init block.
void Init();
}  // namespace coop::dev::native_ui_probe
