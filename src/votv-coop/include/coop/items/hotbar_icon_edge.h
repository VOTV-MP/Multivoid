// coop/items/hotbar_icon_edge.h -- redraw the hotbar once per world, when the icons it
// needs arrive after the game has already drawn it without them.
//
// The game publishes its prop-icon tables seconds after a world load and draws the hotbar's slots
// before that, so every icon resolves to nothing and the slots keep it; at the moment it publishes,
// the renderer tells the hotbar nothing. This is the
// notification it never sends. docs/ui.md has the mechanism, the measurement, and why the
// trigger asks whether the bar is wrong rather than watching the tables fill.
//
// The reading and the verb are the wrapper's (ue_wrap/hotbar/icons.h); the policy -- which world
// is owed a redraw, and when it stops asking -- is here. No network and no coop state: a host, a
// client and a world with no session at all lose the same race for the same reason.

#pragma once

namespace coop::hotbar_icon_edge {

// Poll the bar; redraw it once per world if the icons arrived after it was drawn. Called from
// the session tick and from the session-less idle branch, and latched per world either way, so
// reaching it twice in a tick costs a comparison. Game thread.
void Tick();

}  // namespace coop::hotbar_icon_edge
