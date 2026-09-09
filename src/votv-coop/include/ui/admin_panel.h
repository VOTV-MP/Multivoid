// ui/admin_panel.h -- the F1 > Administration > Players content pane (host-only).
//
// Three sections:
//   Online  -- every connected remote peer: full row (nick / ping / link) +
//              Teleport / Kick / Ban actions.
//   Offline -- every player this host has ever seen (coop::seen_players) that is
//              not currently online: nick + last seen + Ban action.
//   Banned  -- every persistent IP ban (coop::ban_list): nick / IP / date /
//              reason + Unban action.
//
// ROLE-gated, not dev-gated: the category is only offered to the HOST of a live
// coop session (dev_menu gates on coop::roster::LocalIsHost()); clients and
// solo players never see it. Render thread; all data comes from the modules'
// any-thread snapshot APIs, refreshed at ~2 Hz while the pane is open.

#pragma once

namespace ui::admin_panel {

// Draw the Players pane content (called from dev_menu's content child).
void Render();

}  // namespace ui::admin_panel
