// coop/dev/prop_syncer_drill.h -- [dev] the prop syncer drill: a client syncs a prop its own interaction moves
// (ini prop_syncer_drill=kick|forge, env VOTVCOOP_PROP_SYNCER_DRILL; ALL THREE peers, the same value). The client in
// slot 1 acts, the client in slot 2 observes (and forges), another slot idles. The peers sequence on lobby-chat lines.
//   kick  -- both clients say they are ready (their own streams run), then the host says go; the client walks to a keyed
//            prop with the director, claims it, kicks it with the game's own physics and holds the claim for a second;
//            when its copy rests it says where. The host and the observer each judge the stream: at least three rows
//            of the granted generation applied to their copy before the End, the last of them naming a place half of
//            the client's distance from the copy's start, and the copy at rest within 5 cm / 3 deg of the End's pose.
//   forge -- the kick, and the observer publishes rows for the kicked prop at its own place, failing if it forged none;
//            the host must count them as from a non-syncer, admit none, and still land on the client's pose.
// A peer ends on a DONE PASS line or a FAIL: line (the fail marker), both tagged PROP-SYNC. The reds are the
// developer knob prop_syncer_red (prop_syncer_host.cpp and prop_drive_host.cpp read it): refuse (arm kick), no_relay
// (arm kick), not_syncer (arm forge). Every bound is a failure bound, never a wait.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dev::prop_syncer_drill {

// True when the drill row is on (latched). The production modules the drill judges write their views (the stream's,
// the host's admission counts, the client's last grant) only while it is.
bool IsEnabled();

// Advances this peer's phases; a latched read when off. Game thread.
void Tick(coop::net::Session* s);

// CLIENT: message `seq` of `total` of the host's DevLoadFiller arrived. Dropped at once when the drill row is off;
// otherwise counted, with the receive time of the first and of the last logged. With prop_syncer_load_kb > 0 (the
// host and the kicking client) the host queues that filler as it answers the claim (BeforeGrant), and the client's
// DONE line adds grant_ms, load_msgs_at_grant, load_total and load_ms. Game thread.
void OnFiller(uint32_t seq, uint32_t total);

// HOST: the claim of `slot` is about to be answered Granted. With the drill row on and prop_syncer_load_kb > 0, queues
// the filler to `slot`, once per session; otherwise a no-op. A refused send is the host arm's FAIL. Game thread.
void BeforeGrant(coop::net::Session& s, uint8_t slot);

// The session ended: the drill starts over, and its walk with it.
void OnDisconnect();

}  // namespace coop::dev::prop_syncer_drill
