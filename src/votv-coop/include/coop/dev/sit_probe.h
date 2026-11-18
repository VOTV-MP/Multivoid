// coop/dev/sit_probe.h -- [dev] a client sits on a chair and the host looks at its body
// (VOTVCOOP_SIT_PROBE=1 on both peers), for the fork issue "a client sitting in a chair shows standing".
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::sit_probe {

// The dev lanes' tick. Game thread.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::sit_probe
