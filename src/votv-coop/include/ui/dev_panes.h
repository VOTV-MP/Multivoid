// ui/dev_panes.h -- the F1 menu's developer panes: the controls behind the dev-flagged entries of the
// menu's tree (ui/dev_menu.cpp), which hides them unless the dev switch is on. Kept apart from the menu
// and its player panes, which are translated, so a file is either the player's text or a developer's.
// Render thread, inside the overlay's frame, as the menu draws them.

#pragma once

namespace ui::dev_panes {

void RenderSnow();
void RenderTeleportClients();
void RenderFreecam();
void RenderRestoreVitals();
void RenderSetClock();
void RenderPosHud();
void RenderObjectOverlay();
void RenderRagdollBones();
void RenderSpawnNpc();
void RenderGivePoints();
void RenderSpawnMenuUnlock();
void RenderEvents();

}  // namespace ui::dev_panes
