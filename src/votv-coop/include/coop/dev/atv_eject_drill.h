// coop/dev/atv_eject_drill.h -- the forced-eject acceptance arm for the v147 ATV condition
// lane. Env-gated (VOTVCOOP_ATV_EJECT_TEST = "host" | "client"), fires ONCE per process
// through the game's own damageWheel path. See the .cpp header for the two arms' contracts.
// RULE-2-exempt diagnostic.

#pragma once

#include <cstdint>

namespace coop::atv_eject_drill {

// Called per owned/authored ATV from atv_sync's Tick. Cheap when the env is unset (one
// cached getenv). Game thread.
void MaybeFire(void* actor, const wchar_t* key, uint64_t nowMs, bool isHost, bool isAuthority,
               bool ownsTick);

}  // namespace coop::atv_eject_drill
