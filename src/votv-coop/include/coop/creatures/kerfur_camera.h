// coop/creatures/kerfur_camera.h -- a client's Omega camera stays on its laptop's camera list.
//
// An Omega carries a camera (its `camera`, a prop_camera_good_C child actor) that its BeginPlay switches on, and every
// camera puts itself on the gamemode's `cams`, the laptop's list, through its saveInit (at its saved index, or added) and
// takes itself off as it ends. On a client the save's own Omega is replaced by the host's, and the two cameras' adds
// and removals shift `cams` under each other: the mirror's camera ended off the list, and the laptop showed it
// unavailable (Ghezra/Multivoid#38). CLIENT: every two seconds, an Omega camera missing from `cams` is put back through
// its own saveInit, its saved index cleared so it is added. Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::kerfur_camera {

// Per pump tick; a client's check runs every two seconds. Game thread.
void Tick(coop::net::Session& session);

}  // namespace coop::kerfur_camera
