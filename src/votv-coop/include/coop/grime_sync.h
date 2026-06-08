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
// DEFERRED: (1) the FINAL decal removal when a wipe takes process<0 (native K2_DestroyActor). We do
// NOT infer a destroy from a vanished index entry: grime lives in streamed sublevels and streams
// out/in as the player moves, so a vanished entry is NOT a reliable destroy signal (the first smoke
// proved a connect-teleport streams out hundreds of decals at once -> a death-watch flooded false
// destroys that wrongly removed the peer's still-present decals). For a GRADUAL wipe (multiple
// sponge hits over ~1-2 s) the 20 Hz poll captures the decreasing process and drives the mirror to
// process~=0 (invisible) -- correct. The KNOWN GAP (correctness audit H-1): a single high-strength
// hit that drives process from a high value straight past 0 in one poll interval is never captured
// (the poll's prior value was high, then the actor is gone) -> that decal's MIRROR stays VISIBLY
// DIRTY permanently. The root-cause fix is a K2_DestroyActor PRE edge that reads process<0 just
// before the actor dies and broadcasts value=0 (drive the mirror invisible) -- a separate increment
// (uncertain whether the observer fires for the BP-internal clean()->K2_DestroyActor; needs probing).
// Verify hands-on whether a one-shot wipe is even achievable in-game before prioritizing. (2) runtime
// grimeProjectile splatter grime -- non-deterministic spawn position, not position-identifiable
// (would need the eid path). Level-placed grime is the stated ask.
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
