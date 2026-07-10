// coop/multiplayer_menu.h -- the MULTIPLAYER entry point in VOTV's main menu.
//
// Injects a native "MULTIPLAYER" UButton into VOTV's ui_menu_C, positioned just
// ABOVE button_start (NEW GAME), and opens the ImGui server browser
// (ui::server_browser) when it is clicked. Coop/gameplay layer (principle 7): it
// owns the FEATURE (which menu, where, what the click does) and talks to the
// engine only through ue_wrap (engine::InjectCanvasButton / WidgetIsHovered +
// reflection for the ui_menu_C class + field offsets).
//
// Click detection is a POLL, not a delegate bind: a POST observer on
// ui_menu_C::Tick (the menu's own per-frame game-thread tick) checks
// UButton::IsHovered() + a global VK_LBUTTON edge. A reflection-only DLL cannot
// bind the UButton::OnClicked FMulticastScriptDelegate (no UObject+UFunction to
// point it at), and the Tick observer is the one reliable game-thread tick that
// runs while the menu is up (net_pump does not run pre-gameplay). Mirrors the
// proven coop::save_button_disable pattern (same Tick observer, FindPropertyOffset
// field reads, isPause main-vs-pause discriminator).

#pragma once

namespace coop::multiplayer_menu {

// Resolve ui_menu_C + register the Tick observer (idempotent). Safe to call at
// boot: if the menu BP class is not loaded yet, a bounded retry re-attempts until
// it resolves. Gated off by [coop] multiplayer_menu=0 in votv-coop.ini.
void Init();

// TEST hook: inject the MULTIPLAYER button onto the live ui_menu_C right now,
// deterministically (used by coop::dev::menu_proceed to avoid the observer-timing
// race in the brief screenshot window). Game thread only.
void ForceInjectNow();

// True while VOTV's native pause/ESC menu (ui_menu_C with isPause) is currently up.
// Backed by a freshness-stamped atomic the game-thread Tick observer updates, so this
// is RENDER-THREAD / WndProc safe. The ImGui overlay reads it to NOT draw the passive
// coop HUD (chat feed / nameplates) or open the chat input over the modal pause menu.
// Auto-clears ~250 ms after the pause menu stops ticking (closed or back in gameplay).
bool IsPauseMenuOpen();

// The resolved ui_menu_C::Tick UFunction* (the menu's per-frame tick), or null if
// not resolved yet. net_pump uses it as the death-flee transparent-bypass RELEASE
// condition: the first time this dispatches, the menu world is up so the detour
// can resume and inject MULTIPLAYER on that frame. Game thread.
void* MenuTickFn();

}  // namespace coop::multiplayer_menu
