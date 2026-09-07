// coop/interactables/grime_sync.h -- surface grime sync (Agrime_C::process, the wall, ceiling and
// floor dirt), on ReliableKind::GrimeState.
// Gameplay and network layer (principle 7): the wire protocol, the poll, the minimum-wins apply,
// the position-to-actor index, the death-watch, the deferred-apply retry and the connect snapshot.
// Its engine reads go through ue_wrap::grime and ue_wrap::engine, plus reflection for the liveness
// and class tests.
//
// A monotone minimum register, the window_sync shape keyed differently. A LEVEL-PLACED decal has a
// saved transform, so both peers put each one at an identical world position, and that position IS
// its cross-peer identity: the key is a quantised world-position string. Runtime splatter is OUT of
// this lane for the same reason -- its spawn position is not deterministic across peers, so nothing
// here can name it. Only a FALL is propagated; a rise means the decal reloaded and the poll resyncs
// silently, so MIN(local, wire) converges concurrent wipes and a streamed-out decal sends nothing.
// A one-hit super-sponge outruns the poll, destroying the actor inside the Blueprint before the
// next read, so a decal vanishing NEAR the camera is a wipe and one far away is a stream-out.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct KeyedScalarPayload;
}  // namespace coop::net

namespace coop::grime_sync {

// Resolve the grime_C class + build the position->actor index. Idempotent; retried every
// net-pump tick until the BP class is loaded. Stores the session pointer. Game thread.
void Install(coop::net::Session* session);

// Receiver entry: a GrimeState packet arrived (event_feed has copied and range-checked it).
// Resolves the grime by position key and applies MIN(local, value) for a live wipe (adopt 0),
// or takes the value as sent for the host's connect snapshot (adopt 1). Defers if the instance
// has not streamed in. Called from event_feed's reliable drain loop.
void OnReliable(const coop::net::KeyedScalarPayload& payload, uint8_t senderPeerSlot);

// HOST-only: snapshot the current `process` of every indexed grime to a freshly connected
// client `peerSlot` with adopt=1 (the joiner adopts the host's world). Called from the net-pump
// connect edge. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Per-tick: poll for live `process` decreases (broadcast wipes) + retry deferred applies
// (throttled). Call every net-pump tick on the game thread.
void Tick();

// Session teardown: clear the per-session poll baseline + pending applies.
void OnDisconnect();

// DEV-DRILL ONLY: the quantised position key this module would index `actor` under. The
// scan-parity drill uses it so its independent walk counts DISTINCT CELLS the way the index
// does. An instance count over-reads, because the grid is not collision-free: a base world's
// thousand-odd decals land on two fewer cells than there are decals.
std::wstring DebugPosKeyForActor(void* actor);

}  // namespace coop::grime_sync
