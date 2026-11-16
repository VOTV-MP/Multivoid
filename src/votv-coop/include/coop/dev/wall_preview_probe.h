// coop/dev/wall_preview_probe.h -- [dev] a wall builder's aiming preview on both peers (VOTVCOOP_WALL_PREVIEW_PROBE=1): a
// client holds the tool and stretches a wall, and both log the tool's preview, its own and the other's mirror of it.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::wall_preview_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::wall_preview_probe
