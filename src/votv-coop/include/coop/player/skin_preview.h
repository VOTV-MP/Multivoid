// coop/player/skin_preview.h -- LIVE in-world skin preview for the F1 skins panel.
//
// While the user is looking at the skins section of the F1 menu, a display-only
// mannequin (an inert mainPlayer_C puppet) is spawned in front of the player's
// camera and the skin under the cursor is applied to it LIVE -- a real 3D preview
// that needs no preview-image assets. When the section is closed / the menu gone,
// the mannequin despawns after a short grace window.
//
// Thread split (the panel renders on the render thread, the engine is game-thread):
//   Activate()  -- render thread, every frame the skins section is open, with the
//                  hovered (or current) skin name. Updates the keep-alive stamp
//                  and the pending-skin request.
//   Tick()      -- game thread (subsystems::TickGameplay): spawn/despawn by the
//                  keep-alive stamp, apply the pending skin if it changed, and
//                  reposition the mannequin in front of the camera each tick so it
//                  follows the player.
//   OnDisconnect() -- session teardown: despawn + clear the pending request.

#pragma once

#include <string>

namespace coop::skin_preview {

// Render-thread keep-alive + skin request. Called every frame the skins section is
// open (idempotent). `name` is the hovered skin, or the current one when nothing
// is hovered.
void Activate(const std::string& name);

// Game thread. Drives the mannequin lifecycle + pose.
void Tick();

// Session teardown: despawn the mannequin + drop the pending request.
void OnDisconnect();

}  // namespace coop::skin_preview
