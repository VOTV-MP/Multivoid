// coop/dev/kprint_probe.h -- [dev] a 3D print made by one player, its printed object logged on every peer
// (VOTVCOOP_KPRINT_PROBE=host|client on every peer), for VOTV-MP/Multivoid#49.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::kprint_probe {

// The dev lanes' tick. A latched read when the probe is off. Game thread.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::kprint_probe
