// coop/interactables/tape_caddy_sync.h -- the L7 tape-caddy lane owner (wallunit_tapes
// reel SLOTS + the host accrual CORRECTOR). Wire: ReliableKind::ReelSlot=102
// (presser-authored sentinel edges, relayed) + MsgType::ReelPose=40 (HOST 1 Hz
// corrector, unreliable newest-wins). The `active` toggle rides the existing
// ApplianceState lane (NOT here); the reel PROP's cross-peer birth rides
// PropSpawn.savedScalar + ReelEjectIntent (prop_drop_intent). The client accrual is
// NOT parked -- the WRITTEN park-doctrine deviation (upd() re-enables the tick at
// every native verb + wire apply, so a park is un-holdable; the accrual is RNG-free/
// deterministic/clamped and the corrector owns convergence; sawtooth <= 1 increment).
//
// Design of record: research/findings/computers-devices/votv-tape-caddy-L7-impl-DESIGN-2026-07-17.md.

#pragma once

#include <cstdint>

#include "coop/net/protocol.h"

namespace coop::net { class Session; }

namespace coop::tape_caddy_sync {

// Store the session + log the install latch. Idempotent; called from the
// subsystems ensure path every net-pump tick. Game thread.
void Install(coop::net::Session* session);

// Per-net-pump tick: throttled resolve; the 4 Hz slot sentinel poll (both
// peers); the HOST 1 Hz corrector publish; the CLIENT corrector apply. Game thread.
void Tick();

// Wire handlers (event_dispatch_state routes here; each posts ONE game-thread
// task doing apply + poll-baseline prime -- echo-dead by construction).
void OnReelSlot(const coop::net::ReelSlotPayload& p, uint8_t senderSlot);

// Session teardown (the subsystems OnDisconnect fanout): reset poll baselines,
// timers, the IsRecent stamps, and the ue_wrap singleton cache. Game thread.
void OnDisconnect();

}  // namespace coop::tape_caddy_sync
