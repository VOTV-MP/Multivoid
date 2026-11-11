// ui/dev_panes.h -- the F1 menu's developer panes: the controls behind the dev-flagged entries of the
// menu's tree (ui/dev_menu.cpp), which hides them unless the dev switch is on. Kept apart from the menu
// and its player panes, which are translated, so a file is either the player's text or a developer's. What a
// player or a host uses (items, points, events, the world, creatures, the free camera) is in ui/cheat_panes, not
// here: these are the diagnostics. Render thread, inside the overlay's frame, as the menu draws them.

#pragma once

namespace ui::dev_panes {

void RenderStaminaLow();
void RenderObjectOverlay();
void RenderRagdollBones();

}  // namespace ui::dev_panes
