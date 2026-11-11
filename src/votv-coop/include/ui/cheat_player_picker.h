// ui/cheat_player_picker.h -- the players of the session as a combo, for the cheat panes that act on one of them
// (spawn at, restore vitals, teleport). A player is named by the slot the host numbers the session by (0 = the host),
// the same number a cheat request carries. Render thread; the roster is read as a copy, refreshed twice a second.

#pragma once

#include <cstddef>

namespace ui::cheat_picker {

// Make `slot` name a connected player: left as it is when it does, else the first other player, else this one.
void Choose(int* slot, bool includeSelf);

// Draw a combo named `id` (a literal id, never language-dependent) over the connected players. `slot` is the chosen
// player's slot and is set to a valid one when it names nobody (the first other player, else this one). True when the
// choice changed this frame. `includeSelf` lists the local player too.
bool Combo(const char* id, int* slot, bool includeSelf);

// The nick of the player in `slot` as the roster holds it; false when none is connected there.
bool NickOf(int slot, char* out, size_t outLen);

// How many players are connected, this one included (1 in a solo game).
int Count();

// The proved player id of the player in `slot` (the name `/tphere <id>` takes), empty when not known. Host only.
const char* PlayerIdOf(int slot);

}  // namespace ui::cheat_picker
