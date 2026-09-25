// coop/interactables/grime_sync.h -- surface grime sync (Agrime_C::process, the wall, ceiling and
// floor dirt), on ReliableKind::GrimeState. Gameplay and network layer (principle 7): the wire, the
// lowering verbs' watches, the per-key floor, the position-to-actor index, the decals' end of play
// and the connect snapshot; engine reads through ue_wrap::grime and ue_wrap::engine, plus reflection
// for the liveness and class tests.
//
// A monotone minimum register, the window_sync shape keyed differently. A decal the save holds has a
// saved transform, so both peers put it at an identical world position, and that position IS its
// cross-peer identity: the key is a quantised world-position string. Splatter spawned during a session
// lands where each peer's own run put it, so its key names nothing on the other peer, and what is sent
// for it is kept there as an unused floor. Only a FALL is propagated, read before and after a lowering
// verb's body on the script gate (clean; the wall fixer's repair of a crack), so MIN(local, wire)
// converges concurrent wipes. A verb that takes the process below zero destroys the decal inside the
// Blueprint; the engine's end of play says destroyed, a wipe sent as zero, or streamed out, whose
// process is kept as the key's floor for the decal's return.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct KeyedScalarPayload;
}  // namespace coop::net

namespace coop::grime_sync {

// Store the session pointer and register with the scan hub, which builds the position index.
// Idempotent. Game thread.
void Install(coop::net::Session* session);

// Receiver entry: a GrimeState packet arrived (event_feed has copied and range-checked it). A live
// fall (adopt 0) lowers the key's floor and is written to a decal reading above it; a value of the
// host's connect snapshot (adopt 1) sets the floor and is written as is to a decal indexed when it
// arrives. A pass lowers a decal it takes to its floor and never raises one. Called from event_feed's
// reliable drain loop.
void OnReliable(const coop::net::KeyedScalarPayload& payload, uint8_t senderPeerSlot);

// HOST-only: snapshot the current `process` of every indexed grime (the lower of it and its floor),
// and the floor of every key the index does not hold live (a decal ended here, or streamed out), to a
// freshly connected client
// `peerSlot` with adopt=1 (the joiner adopts the host's world). Called from the net-pump connect
// edge. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Per-tick: install the two lowering verbs' watches and the end-of-play subscription until they
// take. Call every net-pump tick on the game thread.
void Tick();

// DEV-DRILL ONLY: the quantised position key this module would index `actor` under. The
// scan-parity drill uses it so its independent walk counts DISTINCT CELLS the way the index
// does. An instance count over-reads, because the grid is not collision-free: the rig's save puts
// 1117 decals on 1087 keys.
std::wstring DebugPosKeyForActor(void* actor);

}  // namespace coop::grime_sync
