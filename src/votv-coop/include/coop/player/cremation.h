// coop/player/cremation.h -- host-directed trip through the game's cremator.

#pragma once

#include "coop/moderation/moderation.h"

namespace coop::net { class Session; }

namespace coop::cremation {

// Queue a host-only cremator trip for the captured remote player. The client locks its local
// controls, moves into the cremator's firebox, and the host kicks it after the death reaches its
// puppet. A timeout releases the controls if the native cremator sequence did not kill them.
coop::moderation::ModResult SendToCremator(const coop::moderation::PlayerToken& token);

// Host pump hook; call once per game-thread tick.
void Tick(coop::net::Session& session);

// Suppress movement while leaving camera look available on the client. Game thread only.
void ApplyLocalControl(bool disabled);

// Run the cremator Blueprint's close/ignite sequence after the host's teleport has applied.
void StartLocalSequence();

// Hold the named local cremator shut and its start lever active during the trip.
void SetLocalCrematorLock(float locX, float locY, float locZ, bool locked);

// Release this machine's control lock and forget host-side trips during session teardown.
void OnSessionEnd();

}  // namespace coop::cremation
