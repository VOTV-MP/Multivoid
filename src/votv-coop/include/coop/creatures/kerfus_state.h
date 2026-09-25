// coop/creatures/kerfus_state.h -- the Kerfus's live state and its drive, from the host.
//
// A client never runs the Kerfus's brain (kerfus_brain), so what the brain writes has to reach it:
// whether the Kerfus is on, whether it is charging, and its energy, all three on its look-at text
// ("Energy: {energy}" and "Charging..."). The host reads them after each of its Kerfus's ticks and
// sends a KerfusState on an edge of either flag, and for energy alone on a step of half a unit, at
// most once a second. The save record is not the carrier: it lacks `charging`, and energy moves on
// every tick with no upd() to publish at. A client writes the fields and runs the game's own
// upd(false), which sets the looks and sounds and asks the pawn to move (refused there).
//
// While a Kerfus is on, the host holds it under prop_drive_host's drive, so its pose streams to
// every client as a hooked prop's does, and lets go when it goes off: the drive coasts it to rest and
// sends the end. MTA's unoccupied-vehicle syncer, the host the one syncer
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp:171,194).
//
// Late join (principle 8): a joiner's transferred save sets the three fields through loadData, and
// its world-ready gets every Kerfus's state again; the drive re-sends a moving Kerfus's pose at the
// same edge. A state for a Kerfus whose mirror has not bound yet waits for the bind. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct KerfusStatePayload;
}  // namespace coop::net

namespace coop::kerfus_state {

// Register the host's tick watch and cache the session. Idempotent, retried until the gate takes it.
// Game thread.
void Install(coop::net::Session* session);

// CLIENT per tick: apply the states that waited for their Kerfus's mirror. A no-op with none waiting.
void Tick();

// HOST: a peer's world came up -- send it every Kerfus's state. Game thread.
void OnPeerWorldReady(int slot);

// CLIENT: the host's state for one Kerfus. From the event feed, game thread.
void OnState(const coop::net::KerfusStatePayload& payload);

// Session end: the host's per-Kerfus records and the client's waiting states go.
void OnDisconnect();

// CLIENT: the states applied this session (the kerfus drill's proof). Game thread.
unsigned long long AppliedCount();

}  // namespace coop::kerfus_state
