// ui/cheat_panes.h -- the F1 menu's cheat panes: the host's tools and the player tools a host may open to its players.
//
// "Host tools" (the host, or a solo game) and "Player tools" (a client) draw the same panes for the controls they
// share. A pane never writes the world: it calls coop::cheats, which runs the cheat at once on the host or sends it to
// the host as a request from a client (coop/cheats/cheat_request). The Items, Economy and Events panes therefore work
// unchanged for a client the host allows; only the menu's gate decides who is offered them (ui/dev_menu). Every name
// and line a pane shows goes through the translation layer. Render thread, inside the overlay's frame.

#pragma once

namespace ui::cheat_panes {

// What the menu read once this frame of the cheat settings, for the panes that show them: the host's master switch, and on a
// client whether its own right holds. Set by the menu before it draws a pane; render thread.
struct CheatFrame {
    bool hostAllows = false;   // net.allow_player_cheats
    bool mayCheat = false;     // a client: the setting is on and the host has not withheld this machine's right
};
void SetCheatFrame(const CheatFrame& f);
const CheatFrame& CheatFrameNow();

// Items: every prop the game can spawn, searchable, with a count and where to put them.
void RenderItems();

// Economy: add or remove points, with presets and a custom amount.
void RenderEconomy();

// Events: every game event, triggered from here.
void RenderEvents();

// Entities: the creatures the host can spawn (host only).
void RenderEntities();

// World: weather, the clock, and helpers for the players (host only).
void RenderWorld();

// Player cheats: the host's switch that lets its players use the cheat tools (host only).
void RenderPlayerCheats();

// Myself: moving and helping one's own body -- teleport to the host, unstuck, and the local tools the host grants.
void RenderMyself();

}  // namespace ui::cheat_panes
