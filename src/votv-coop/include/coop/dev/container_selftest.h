// coop/dev/container_selftest.h -- an instrument for the container contents lane
// (`[dev] container_selftest=1`, or `VOTVCOOP_CONTAINER_SELFTEST=1` per run; it MUTATES the world).
//
// An idle two-peer run cannot exercise this lane: nobody opens a container, and a save load fills
// `saveSlot.GObjStack` wholesale rather than through the watched verbs, so the lane's central claim
// -- that the `addObject`/`takeObj` watch ENTERS on each peer -- stays invisible to it.
//
// The host dispatches `prop_container_C::extract(0)` at +10 s on the nearest non-empty container
// to its own body: extract's inner `takeObj` is the game's own dispatch the watch must catch. The
// client does what a player does: at +25 s (`VOTVCOOP_CONTAINER_FIRE_MS` overrides) it walks to a
// container the shared pick chose (near, a nav route reaches it, two records or more), says its eid
// on drill chat, opens its screen and presses the first slot, so its take is an act. Both print a
// DIGEST every 5 s. Verdicts: HOST, 15 s after its fire (RED: the trigger changed nothing or the
// watch never fired); HOST-SEES-CLIENT (one grant, no Gone, the announced target's count fell by
// one); CLIENT (a Granted reply and a done commit). The host then prints DONE.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::container_selftest {

// Cache the session pointer. Call once at boot (subsystems Install). No-op with the flag off.
void Install(coop::net::Session* session);

// WHAT IT DOES NOT MEASURE, so nobody reads it as a pass: a take that races another peer's. That
// belongs to `mp.py ctakerace`, whose peers walk to a shared container and whose orchestrator sums
// the item across them.

// Game thread, per tick. Drives the host's dispatch, the client's take and the 5s digest. No-op
// with the flag off or before the session connects.
void Tick();

// Clear the schedule + the resolved containers so a reconnect re-runs the circle.
void OnDisconnect();

}  // namespace coop::dev::container_selftest
