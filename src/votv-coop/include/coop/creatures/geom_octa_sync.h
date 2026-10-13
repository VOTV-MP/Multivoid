// coop/creatures/geom_octa_sync.h -- Geom Octa pose/animation fields on the NPC pose lane.
#pragma once

#include "coop/net/protocol.h"

namespace coop::net { class Session; }

namespace coop::geom_octa_sync {

void Install(coop::net::Session* session);
void Tick();

// The generic NPC snapshot carries these extra fields only for geomOcta_C. Both functions are
// game-thread only and class-gated; other NPC types are untouched.
void Capture(void* actor, coop::net::EntityPoseSnapshot& pose);
void Apply(void* actor, const coop::net::EntityPoseSnapshot& pose);

}  // namespace coop::geom_octa_sync
