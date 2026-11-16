// coop/dev/crowbarpry_probe.h -- [dev] a client pries a stuck board off its wall with a crowbar
// (VOTVCOOP_CROWBARPRY_PROBE=1 on both peers), every peer logging the board's prying crowbar and the crowbars near it,
// for the Trello card "crowbar synchronization issue".
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::crowbarpry_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::crowbarpry_probe
