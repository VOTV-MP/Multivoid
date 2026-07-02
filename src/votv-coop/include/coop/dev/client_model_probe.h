// coop/dev/client_model_probe.h -- clean side-by-side visual check for the custom client mesh.
//
// THE QUESTION (2026-07-01): the runtime loads + applies our pak's scientist SkeletalMesh to a
// client puppet (verified: SetSkeletalMesh ret=1, field-after == our ptr), yet the coop screenshots
// were AMBIGUOUS -- world kerfurs cluttered the frame; could not tell if the puppet rendered as the
// scientist, as kel, or was INVISIBLE. This probe removes all ambiguity: ~8 s after the world (and
// the player's save-load skin) settles, it spawns TWO display puppets ~3 m in front of the CAMERA,
// side by side, both facing the player --
//   LEFT  = the local kel skin (control: proves a display puppet renders at this spot), and
//   RIGHT = our pak scientist mesh (coop::client_model).
// One look then settles the cook verdict: HL-scientist shape = cook WORKS / kel robot = render data
// is still the template / nothing there = no usable render geometry.
//
// Gated by votv-coop.ini [dev] client_model_probe=1 -- set the flag ONLY on the machine that should
// spawn the pair (the host folder); no role gate in code, so a solo host run works (cleanest).
// One-shot; read-only diagnostic (the pair stays until quit -- it IS the display).
// Game thread. [[feedback-probe-dont-guess-rule]]

#pragma once

namespace coop::dev::client_model_probe {

void Install();
void Tick(bool connected, bool isHost);

}  // namespace coop::dev::client_model_probe
