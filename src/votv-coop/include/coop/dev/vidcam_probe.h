// coop/dev/vidcam_probe.h -- [dev] a camcorder recording driven through the game's own calls, its recorder's state
// logged (VOTVCOOP_VIDCAM_PROBE=host|client names the peer that records): the peer takes a camcorder into its hand,
// starts it through the camcorder's own LMB and stops it through the camera manager's stopRecord.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::vidcam_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::vidcam_probe
