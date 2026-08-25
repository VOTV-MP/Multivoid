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
// UButton::IsHovered() + a global VK_LBUTTON edge. The Tick observer is the one
// reliable game-thread tick that runs while the menu is up (net_pump does not run
// pre-gameplay). Mirrors the proven coop::save_button_disable pattern (same Tick
// observer, FindPropertyOffset field reads, isPause main-vs-pause discriminator).
//
// WHY NOT A DELEGATE BIND -- and read this before repeating the old claim. This
// comment used to assert that "a reflection-only DLL CANNOT bind the OnClicked
// FMulticastScriptDelegate (no UObject+UFunction to point it at)". That described
// what had been BUILT, not the substrate, and it is now measured to be reachable:
// OnClicked is a plain TArray<FScriptDelegate> at UButton+0x3C8 (CXXHeaderDump
// UMG.hpp:284), a delegate-dispatched event IS ProcessEvent-visible
// (COOP_DISPATCH_VISIBILITY.md:81 -- the game's own inventory buttons), and every
// primitive the bind needs is already public here (reflection InternalIndexOf /
// SlotSerial / EngineAlloc, fname_utils StringToFName, game_thread
// RegisterInterceptor with its cancel-on-true). The bind has still NEVER RUN --
// design + the one [RD] link left to measure: docs/MULTIPLAYER_UI.md 6e.
//
// DECIDED 2026-08-25 by the browser design's converged /qf: the bind is DROPPED
// FROM v1 and THE POLL IS THE ANSWER, not a placeholder. Two reasons, both
// measured. (1) Nothing makes the bind NECESSARY: this poll already reads a real
// UButton with no new primitives, and OPUS section 7 endorses poll-and-diff where
// dispatch is in doubt. (2) The shape 6e describes needs "a no-param UFunction the
// engine already provides", and WHICH one was never answered -- RegisterInterceptor
// keys on the UFUNCTION (game_thread.h:89-91), so borrowing an existing one routes
// every other caller in the game through our callback, while minting our own means
// building a UFunction from a DLL that owns no UClass. Dropping it removed the
// design's last unmeasured leap. The browser's probe still OBSERVES the bind on a
// throwaway button (a bind is a WRITE, not a read) so the v2 call has evidence.
// NOTE the arc this serves retires the IMGUI SUBSTRATE, not polling -- and
// IsHovered() is resolved on UWidget (engine_widget.cpp:214-217), so it answers for
// ANY UWidget*, not only a UButton. See docs/MULTIPLAYER_UI.md section 8.

#pragma once

namespace coop::multiplayer_menu {

// Resolve ui_menu_C + register the Tick observer (idempotent). Safe to call at
// boot: if the menu BP class is not loaded yet, a bounded retry re-attempts until
// it resolves. Gated off by [coop] multiplayer_menu=0 in multivoid.ini.
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
