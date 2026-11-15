// coop/dev/sleepall_probe.h -- [dev] the host sleeps, then a client, then the client wakes and the host
// (VOTVCOOP_SLEEPALL_PROBE=1 on both peers; VOTVCOOP_SLEEPALL_RULE=1 on the host turns world.sleep_everyone on), the
// world's dilation logged on the host each second, for the host rule "time speeds up only when everyone sleeps".
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::sleepall_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::sleepall_probe
