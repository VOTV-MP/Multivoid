// coop/dev/kwheel_probe.h -- [dev] a following Kerfus driven fast and through obstacles, its body, its wheel and the
// props it hits logged on every peer (VOTVCOOP_KWHEEL_PROBE=1 on every peer), for Ghezra/Multivoid issue 20.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::kwheel_probe {

// The dev lanes' tick. A latched read when the probe is off. Game thread.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::kwheel_probe
