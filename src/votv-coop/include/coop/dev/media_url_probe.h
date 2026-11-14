// coop/dev/media_url_probe.h -- [dev] does the game's media player open and play a web address?
//
// `media_url_probe` is one address or several with `|` between them. Twenty seconds after this
// peer's world is up the probe takes the gamemode's sixteenth media channel (a UMediaPlayer the
// game's TVs share), calls OpenUrl on it with each address in turn and reads the player once a
// second for 25 seconds: whether it is preparing, ready and playing, and the time and the duration
// it reports. Lines tagged [MEDIA-URL-PROBE]; each address ends PLAYED (the time moved) or
// NOT-PLAYED. A channel no screen shows reports a ready player whose time stays at zero.

#pragma once

namespace coop::dev::media_url_probe {

// The probe's pass. A single read when off. Game thread.
void Tick();

}  // namespace coop::dev::media_url_probe
