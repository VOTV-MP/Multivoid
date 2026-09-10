// coop/dev/menu_proceed.h -- TEST-ONLY auto-advance past VOTV's begin/OMEGA
// content-warning screen, so autonomous screenshot runs can reach the MAIN MENU
// (where the injected MULTIPLAYER button lives).
//
// VOTV's menu cursor is raw-input driven (the engine recenters the OS cursor every
// frame at the menu -- that's why ui/imgui_overlay hooks SetCursorPos), so
// synthetic OS clicks (mouse_event / SetCursorPos / PostMessage) do NOT activate a
// menu button. Instead this fires the begin-screen "Proceed" button's OnClicked
// handler directly via reflection.
//
// STRICTLY a dev/test tool (RULE 3): gated by env VOTVCOOP_MENU_PROCEED=1, NEVER on
// by default -- the content warning is a real legal/content gate the shipping mod
// must never auto-skip.

#pragma once

namespace coop::dev::menu_proceed {

// If VOTVCOOP_MENU_PROCEED=1, arm a bounded retry that fires the begin-screen
// Proceed handler once ui_menu_C is live. No-op otherwise.
void Init();

}  // namespace coop::dev::menu_proceed
