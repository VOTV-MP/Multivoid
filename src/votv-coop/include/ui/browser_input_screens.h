// ui/browser_input_screens.h -- VARIANT A of the browser's input fork: the address and the
// nickname are typed in their OWN small native windows, never in the browser's layout.
//
// WHY THERE ARE TWO VARIANTS AT ALL. The user cut the browser's inline address and name
// boxes on sight ("ебаный текст в ебаное окно ввода ip не помещается ... не нужен прям в
// нем ввод"), and then, asked to settle it, answered with a METHOD rather than a verdict:
// "попробуем разные дизайны и что лучше будет то и оставим" -- build both and keep the
// better one. Their veto was CONDITIONAL, against inline input *if it is crutchy and
// pointless by design*, so it is not a question argument can close. This module is the
// answer that says the input belongs somewhere else; `ui/server_browser_inline_input` is
// the one that says it belongs here, done properly. The config row
// `ui.browser_inline_input` chooses, and RULE 2 deletes the loser the moment the user
// picks.
//
// THE SHAPE IS THE GAME'S OWN SUB-WINDOW, not an overlay. VOTV's Language window is a
// SIBLING SWITCHER SCREEN -- the settings screen is not behind it, it has replaced it --
// and that measurement (SERVER_BROWSER_ARC section 2, capture `_3`) is what killed the
// overlay-dialog design in round 2 of the pass. So each of these is a real 12th/13th child
// of `ui_menu_C::switcher_widgets`, built once per menu instance, exactly like the browser
// and the hosting window.
//
// WHAT IT CLOSES. Three of the five parity divergences the browser declares against the
// ImGui fallback: direct-IP connect (D1), the address being remembered (D2) and setting
// your name (D3). `session_manager::ConnectDirect` has worked the whole time -- it was
// homeless, not missing -- and a LAN-only host is reachable by NO other route, so this is
// a functional hole rather than a convenience.
//
// Game thread only, like every other native-screen module.

#pragma once

#include <string>

namespace ui::browser_input_screens {

// WHICH of the two screens. They share every line of their construction and differ only in
// their title, their label, the row they read and write, and what their confirm button
// does -- so they are one screen with a `Kind`, not two modules.
enum class Kind { DirectConnect, ChangeName, LobbyPassword };

// THE PASSWORD PROMPT IS OPENED WITH THE ROW IT IS FOR, because by the time the player
// finishes typing the selection can have moved -- the list re-fetches every 5 s and a
// refresh re-sorts it. The lobby is captured at the moment CONNECT was pressed and the
// join uses THAT, not whatever is highlighted when OK is clicked.
//
// The user asked for this window in these words (2026-08-31): "если кто хочет к серверу
// из списка с замком подключиться и версия правильная то когда он нажмет connect в правой
// PANE, вылезет отдельное окно ввода пароля".
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
