// ui/server_browser.h -- the MULTIPLAYER server browser (ImGui overlay surface).
//
// One of the surfaces ui/imgui_overlay.cpp composites, alongside the F1 dev menu and the
// tilde scoreboard. Opened from the native MULTIPLAYER button injected into VOTV's main menu
// (coop::multiplayer_menu) and rendered as a modal panel over the menu.
//
// The ROW MODEL is ported from MTA's CServerListItem -- name, players, version, world, locked --
// but the RENDERER is ImGui, not MTA's CEGUI: we already host an ImGui overlay in-process, so the
// table is a BeginTable rather than a new GUI dependency. There is no ping column, deliberately:
// ping is measured post-connect through GNS instead of by a per-server pre-list query, and the list
// shows the lobby's heartbeat age. The live feed is coop::session_manager's master fetch; this
// surface owns the table and the Connect, Host and Direct-IP controls.
//
// Threading: Open, Close, Toggle and IsOpen are atomic (set from the game thread by the menu click
// poll, read by the render thread). Render() and the row list are render-thread only.

#pragma once

namespace ui::server_browser {

// Show / hide / flip the browser. Game-thread safe (atomic open flag).
void Open();
void Close();
void Toggle();
bool IsOpen();

// Draw the browser this frame. Render thread only (called from the ImGui present
// pass in ui/imgui_overlay.cpp when IsOpen()).
void Render();

}  // namespace ui::server_browser
