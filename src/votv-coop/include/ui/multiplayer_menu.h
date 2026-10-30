// ui/multiplayer_menu.h -- the MULTIPLAYER entry point in VOTV's main menu.
//
// Injects a native "MULTIPLAYER" UButton into ui_menu_C, positioned just ABOVE
// button_start (NEW GAME). A click asks ui::server_browser_surface to open; WHICH
// browser that is -- the native one, unless an ini row picks the ImGui fallback --
// is the surface's decision, not this module's, because the session runtime's
// recovery paths ask the same question. Gameplay layer (principle 7): it owns the
// FEATURE and reaches the engine only through ue_wrap (InjectCanvasButton,
// WidgetIsHovered, and reflection for ui_menu_C's fields).

// Click detection is a POLL, not a delegate bind: a POST observer on ui_menu_C's
// own per-frame Tick watches a global VK_LBUTTON edge and asks whether the button
// is hovered on the RELEASE edge. That Tick is the one reliable game-thread tick
// while the menu is up, since net_pump does not run pre-gameplay; it mirrors
// save_button_disable, the isPause main-versus-pause discriminator included.
//
#pragma once

// A bind IS reachable -- OnClicked is a plain delegate array, and a
// delegate-dispatched event is ProcessEvent-visible -- and is still not the answer,
// because it needs a no-param UFunction to point at. RegisterInterceptor keys on
// the UFUNCTION, so borrowing one the game already provides would route every other
// caller in the game through our callback, and minting our own means building a
// UFunction from a DLL that owns no UClass.

namespace coop::multiplayer_menu {

// Resolve ui_menu_C + register the Tick observer (idempotent). Safe to call at
// boot: if the menu BP class is not loaded yet, a bounded retry re-attempts until
// it resolves. Gated off by [coop] multiplayer_menu=0 in multivoid.ini.
void Init();

// TEST hook: inject the MULTIPLAYER button onto the live ui_menu_C right now,
// deterministically (used by coop::dev::menu_proceed to avoid the observer-timing
// race in the brief screenshot window). Game thread only.
void ForceInjectNow();

// Whether our MULTIPLAYER button is present on `menu`, the ui_menu_C instance (compared, never
// dereferenced). What a test waits on after ForceInjectNow. Game thread only.
bool IsInjectedInto(void* menu);

// True while VOTV's native pause/ESC menu (ui_menu_C with isPause) is currently up.
// Backed by a freshness-stamped atomic the game-thread Tick observer updates, so this
// is RENDER-THREAD / WndProc safe. The ImGui overlay reads it to NOT draw the passive
// coop HUD (chat feed / nameplates) or open the chat input over the modal pause menu.
// Auto-clears ~250 ms after the pause menu stops ticking (closed or back in gameplay).
bool IsPauseMenuOpen();

// The ui_menu_C that mainGamemode_C::pause_mainMenu holds, live: the game's own reference to its
// menu widget. In a game level it is the pause menu; in the "menu" level the same field holds the
// main menu. A field read off the cached gamemode, never a walk of the object array. Null while
// the gamemode is absent (boot, mid-travel), before the gamemode has made the widget, or when the
// widget is not live; a caller treats null as "not now" and asks again. Game thread.
void* GamemodeMenu();

// Whether `menu`, a ui_menu_C handed to the caller (a tick's own self, never searched for), reads
// isPause, by the bool's byte offset and bit mask. False for null. Game thread.
bool IsPauseInstance(void* menu);

// The in-game pause menu: GamemodeMenu() when that widget reads isPause, so null in the "menu"
// level, where the field holds the main menu. Same null cases otherwise. Game thread.
void* PauseMenu();

// The resolved ui_menu_C::Tick UFunction* (the menu's per-frame tick), or null if
// not resolved yet. net_pump uses it as the death-flee transparent-bypass RELEASE
// condition: the first time this dispatches, the menu world is up so the detour
// can resume and inject MULTIPLAYER on that frame. Game thread.
void* MenuTickFn();

}  // namespace coop::multiplayer_menu
