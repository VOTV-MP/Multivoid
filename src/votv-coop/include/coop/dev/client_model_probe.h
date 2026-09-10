// coop/dev/client_model_probe.h -- clean side-by-side visual check for the custom client mesh.
//
// The runtime loads and applies our pak's scientist SkeletalMesh to a client puppet
// (SetSkeletalMesh returns 1 and the field afterwards is our pointer), but a screenshot of the
// world cannot say whether the puppet RENDERS as the scientist, as kel, or not at all. About
// 8 s after the world and the player's save-load skin settle, this probe spawns TWO display
// puppets ~3 m in front of the CAMERA, side by side, both facing the player --
//   LEFT  = the local kel skin (control: proves a display puppet renders at this spot), and
//   RIGHT = our pak scientist mesh (coop::client_model).
// One look settles it: HL-scientist shape = the cook works; kel robot = the render data is
// still the template; nothing there = no usable render geometry.
//
// Gated by multivoid.ini [dev] client_model_probe=1 -- set it ONLY on the machine that should
// spawn the pair (the host folder); no role gate in code, so a solo host run works. One-shot,
// read-only (the pair stays until quit -- it IS the display). Game thread.

#pragma once

namespace coop::dev::client_model_probe {

void Install();
void Tick(bool connected, bool isHost);

}  // namespace coop::dev::client_model_probe
