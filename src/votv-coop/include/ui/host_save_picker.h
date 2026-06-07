// ui/host_save_picker.h -- the Host-Game save picker (ImGui modal over the menu).
//
// Native functions, ImGui on top: lists existing saves via ue_wrap::save_browser
// (which drives VOTV's own loadSlots) and offers a New Game (save_browser::CreateNamedSave
// = VOTV's CreateSaveGameObject + SaveGameToSlot). On confirm it calls
// coop::session_manager::HostWithSave -> the harness LOADS the chosen world (or creates
// the new save) THEN starts the host session.
//
// Opened from the server browser's "Host Game" button (which passes the lobby host
// params). Interactive surface (cursor + input), like the server browser.

#pragma once

#include <string>

namespace ui::host_save_picker {

// Open the picker, capturing the lobby host params from the server browser, and kick
// off a save scan (RefreshAsync). Render-thread call.
void Open(const std::string& hostName, bool locked, int playersMax);
void Close();
bool IsOpen();

// Draw the modal (call each frame from the overlay while open). Render thread.
void Render();

}  // namespace ui::host_save_picker
