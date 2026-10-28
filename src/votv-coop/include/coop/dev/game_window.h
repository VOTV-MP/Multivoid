// coop/dev/game_window.h -- [dev] the game's own top-level window, found from the game thread that created it, and a
// key pressed on it as the OS delivers a press: a keyboard message the window's queue turns into the event Slate
// routes to the focused widget. The input seam a drill presses at when a widget reads the key itself (the desk atlas's
// OnKeyDown). Game thread.

#pragma once

#include <windows.h>

#include <string>

namespace coop::dev::game_window {

// The window the mod subclasses once it is held, else the first visible top-level window of the calling (game)
// thread with a usable client area, or null. Deterministic
// where the foreground window is not: an unattended run can have another window focused.
HWND GameWindow();

// A press and release of the virtual key `vk` posted to the game window. False without a window.
bool PostKeyPress(unsigned vk);

// A WM_CHAR for `c` posted to the game window: the character message the window's queue would
// have produced from a typed key. False without a window.
bool PostChar(wchar_t c);

// The virtual key of a key the game displays as `displayName` (Key_GetDisplayName: a letter, a digit, "Space Bar",
// "Enter", "Tab", "Backspace", "Escape"), 0 for one this table does not know.
unsigned VirtualKeyOf(const std::wstring& displayName);

}  // namespace coop::dev::game_window
