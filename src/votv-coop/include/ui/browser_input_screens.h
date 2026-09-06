// ui/browser_input_screens.h -- VARIANT A of the browser's input fork: the address and the
// nickname are typed in their OWN small native windows, never in the browser's layout.
//
// WHY THERE ARE TWO VARIANTS AT ALL. The objection to inline address and name boxes was
// CONDITIONAL -- against inline input *if it is crutchy and pointless by design* -- which is
// not a question argument can close, so both designs are built and measured against each other.
// This module says the input belongs somewhere else; `ui/server_browser_inline_input` says it
// belongs here, done properly. The config row `ui.browser_inline_input` chooses, and RULE 2
// deletes the loser once one is picked.
//
// THE SHAPE IS THE GAME'S OWN SUB-WINDOW, not an overlay. VOTV's Language window is a SIBLING
// SWITCHER SCREEN -- the settings screen is not behind it, it has been replaced -- and that
// measurement is what ruled out an overlay dialog. So each of these is a real 12th/13th child
// of `ui_menu_C::switcher_widgets`, built once per menu instance, exactly like the browser and
// the hosting window.

#pragma once

#include <string>

namespace ui::browser_input_screens {

// WHICH of the two screens. They share every line of their construction and differ only in
// their title, their label, the row they read and write, and what their confirm button
// does -- so they are one screen with a `Kind`, not two modules.
enum class Kind { DirectConnect, ChangeName, LobbyPassword };

// THE PASSWORD PROMPT IS OPENED WITH THE ROW IT IS FOR, because by the time the player finishes
// typing, the browser's selection may have moved. The prompt appears when Connect is pressed on
// a locked row whose version matches, and it carries that row with it.
//
// It also closes three of the five parity divergences the browser declares against the ImGui
// fallback: direct-IP connect, the address being remembered, and setting your name.
// `session_manager::ConnectDirect` has worked the whole time -- it was homeless, not missing --
// and a LAN-only host is reachable by no other route, so this is a functional hole rather than
// a convenience.
void OpenPasswordPrompt(const std::string& lobbyId, const std::string& displayName,
                        int hostProto, const std::string& hostGame);

// Ask for a screen. Safe from any thread: it records the intent and the next main-menu
// tick performs it, the same deferral the browser and the hosting window use (the switcher
// is driven through ProcessEvent, which is game-thread only).
void Open(Kind kind);

// Close whichever is open, deferred the same way. `IsOpen` answers for either.
void Close();
bool IsOpen();

// Driven from the main-menu tick observer, beside the browser's own.
void OnMenuTick(void* menu, void* switcher);

}  // namespace ui::browser_input_screens
