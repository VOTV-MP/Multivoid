// coop/grime_sync.h -- surface grime dirt sync (Agrime_C::process, the walls/ceiling/floor
// dirt). Protocol v42, ReliableKind::GrimeState (31) + GrimeDestroy (32).
//
// Gameplay/network layer (principle 7): owns the wire protocol, the per-tick `process` POLL,
// the receiver min-wins apply, the per-grime position->actor index, the death-watch, the
// deferred-apply retry, and the connect-snapshot. Talks to the engine ONLY through
// ue_wrap::grime + ue_wrap::engine.
//
// Model (SYMMETRIC, MTA monotone min-register -- the SAME shape as window_sync, keyed
// differently): Agrime_C is a STATIC level-placed decal with a saved transform, so both peers
// place each decal at an IDENTICAL world position (same save). That position is the decal's
// cross-peer identity -- grime_sync keys each grime by a QUANTIZED WORLD-POSITION string (NOT a
// host-allocated eid / Element / spawn-interceptor; the decal's own position is its key). Each
// peer POLLS every grime's `process`@0x0250 once per tick and broadcasts on a DECREASE (a wipe),
// keyed by that position string. The receiver resolves the grime by position and applies
// MIN(local, process) (process is monotone-decreasing + inert -- a sponge/rain only lowers it ->
// concurrent wipes converge, no oscillation) + repaints via applyMaterial(), driving the mirror
// decal toward process/maxProcess -> 0 (visually clean). The host RELAYS a client's wipe + connect-
// snapshots each grime's process (adopt=1). The process stream is inherently STREAM-safe: a
// streamed-out decal does not change process, so it broadcasts nothing; only a live wipe decreases it.
//
// THE ONE-SHOT / SUPER-SPONGE WIPE (a max-strength sponge drives process past 0 in a SINGLE hit the
// 20 Hz poll can't see -- the actor self-destructs before the next poll reads the low process) is
// handled by a PROXIMITY-GATED death-watch in PollAndBroadcast: a decal that VANISHES from the index
// NEAR the local camera was WIPED (you can only sponge a decal you stand at) -> broadcast value=0 ->
// the peer's mirror MIN-applies 0 -> invisible (clean); a decal that vanishes FAR was STREAMED OUT (a
// sublevel unloaded -- a connect-teleport unloads hundreds) -> ignored. (A K2_DestroyActor PRE edge
// is NOT usable: a BP-internal clean()->K2_DestroyActor bypasses the ProcessEvent detour, like the
// trash clump's morph-destroy.) A GRADUAL wipe (several hits) is already caught by the normal process
// poll; the death-watch just adds the one-hit case. The host re-sends value=0 for its wiped decals in
// the connect-snapshot so a joiner cleans a decal wiped BEFORE it joined.
//
// DEFERRED: runtime grimeProjectile splatter grime -- its spawn position is non-deterministic across
// peers, so it is not position-identifiable (would need the eid/spawn path). Level-placed grime + its
// cleaning is the stated ask.
//
// RE: research/findings/votv-dirt-window-cleaning-RE-and-coop-sync-design-2026-06-07a.md PART A.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct KeyedScalarPayload;
}  // namespace coop::net

namespace coop::grime_sync {

// Resolve the grime_C class + build the position->actor index. Idempotent; retried every
// net-pump tick until the BP class is loaded. Stores the session pointer. Game thread.
void Install(coop::net::Session* session);

// Receiver entry: a GrimeState packet arrived (payload already memcpy'd + range-checked by
// event_feed). Resolves the grime by position-key and applies MIN(local, value) for a live wipe
// (adopt==0) or VERBATIM for a host adopt==1 connect-snapshot, deferring if the instance has not
// streamed in. Called from event_feed's reliable drain loop.
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

}  // namespace coop::grime_sync
