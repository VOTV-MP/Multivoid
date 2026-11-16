// coop/dev/tv_link_drill.h -- [dev] one web video put on a television by the host plays on the
// client's copy of the set.
//
// `tv_link_drill` is a web address, or `typed:` and one, on BOTH peers. Each peer turns the Custom
// Content rule on in its process and gives the televisions of its world power
// (the drill's stand-in for a plugged cord). Once a client stands in its world the host makes sure a
// set exists, spawning one from the game's item list when the save holds none, and asks it for the
// address through the lane's own entry (tv_link_sync::PlayLocal), or with `typed:` by entering it in
// the search box of the set's remote, with `set:` by calling the set's own openLink as its remote's list
// does; with `client:` the client asks its copy and the host watches.
// Each peer then reads its set's
// media player every two seconds for forty: `[TV-LINK-DRILL]` with `PASS` when the play time moved by
// two seconds or more, `FAIL` otherwise.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::dev::tv_link_drill {

// The drill's pass on either peer. A single read when off. Game thread.
void Tick(coop::net::Session* session);

// Back to the first step. Net disconnect.
void OnDisconnect();

}  // namespace coop::dev::tv_link_drill
