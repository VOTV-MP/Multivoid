// coop/local_streams.h -- the OUTBOUND local-state streams: read the local
// player's pose / held-prop transform / ragdoll pelvis physics each pump tick
// and publish them to the session's send side. Extracted from net_pump.cpp
// 2026-06-12 (modular soft cap): net_pump stays the orchestrator; this module
// owns the per-stream edge state (held-prop edge, ragdoll recover edge) and
// the wire-boundary normalization rules documented inline.
//
// MTA shape: the local player's sync slots live with the sync writers
// (CNetAPI::WritePlayerPuppet et al), not in the pulse loop.

#pragma once

namespace coop::net { class Session; }

namespace coop::local_streams {

// Reset the per-session edge detectors (held-prop edge, ragdoll edge, emit
// counters). Called from net_pump::OnSessionStart BEFORE session.Start so a
// session stop/restart doesn't carry stale "was holding prop" / "was
// ragdolling" state into the new session (a stale held-prop edge would fire
// SendPropRelease for the OLD session's key on the NEW session -- a real bug
// found by the audit when this state lived as static-locals).
void OnSessionStart();

// v122 (A' rebind fanout): the held-prop eid is cached at the held EDGE, so an
// identity rebind landing MID-carry (the handback dissolve / any fresh mirror
// bind on the held actor) must refresh the cache here or the stream keeps a
// stale/invalid eid for the rest of the hold. No-op unless `actor` is the
// currently-held prop. Game thread (wire applies + the stream tick are GT).
void NotifyPropEidRebound(void* actor);

// The local player's currently-held actor (grab/hold slot), IsLive-guarded; null
// when nothing is held. v106: trash_channel::TickCarry's rest-exclusion (a still
// player holding a clump must not read as "clump at rest un-held"). Game thread.
void* LastHeldActor();

// Per-tick outbound publish. `local` is the live local mainPlayer_C,
// `controller` its cached controller (may be null -- pose pitch falls back to
// actor rotation). Publishes the pose snapshot, streams the held prop's world
// transform (PropSpawn broadcast on the new-held edge, PropRelease + velocity
// on the release edge), and streams the local ragdoll pelvis physics while
// ragdolling (stop edge on recover). Game thread only.
void Tick(coop::net::Session& session, void* local, void* controller);

}  // namespace coop::local_streams
