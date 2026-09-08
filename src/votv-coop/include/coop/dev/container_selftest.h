// coop/dev/container_selftest.h -- an end-to-end instrument for the bidirectional container
// contents lane (`[dev] container_selftest=1`).
//
// An idle two-peer run cannot exercise this lane: nobody opens a container, and a save load
// fills `saveSlot.GObjStack` wholesale rather than through the watched verbs, so the lane's
// central claim -- that the 0x45 `addObject`/`takeObj` callback ENTERS on each peer -- stays
// invisible to it.
//
// It dispatches `prop_container_C::extract(0)` and nothing else. extract's first act is
// `propInventory->takeObj(index, false, ...)`, dispatched blueprint-internally, so the call WE
// make is the outer one and the mutation to be caught is the game's own inner dispatch.
// Dispatching `takeObj` ourselves would prove nothing: it would arrive through ProcessEvent,
// the one path the lane does NOT rely on. The host fires at +10 s on a world container and the
// client at +25 s on a different one; each peer prints a DIGEST line (eid, record count,
// currVol) for both every 5 s, so the NUMBER can be compared across peers.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::container_selftest {

// Cache the session pointer. Call once at boot (subsystems Install). No-op with the flag off.
void Install(coop::net::Session* session);

// Game thread, per tick. Drives the two scheduled dispatches and the 5s digest. No-op with the
// flag off, before the session connects, or once both dispatches have fired.
void Tick();

// Clear the schedule + the resolved containers so a reconnect re-runs the circle.
void OnDisconnect();

}  // namespace coop::dev::container_selftest
