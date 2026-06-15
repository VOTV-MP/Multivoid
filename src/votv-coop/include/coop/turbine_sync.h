// coop/turbine_sync.h -- giant wind-turbine facing/spin mirror (v61,
// ReliableKind::TurbineState). HOST-authoritative at ~1 Hz per turbine.
//
// WHY (user report 2026-06-11: "the windmills' direction doesn't match between
// the client world and the host world"; RE votv-wind-turbines-RE-2026-06-11.md):
// the turbine BP is a per-tick servo chasing the directionalWind direction --
// which v50 ALREADY syncs -- but its integrator `rot` is NOT save-persisted
// (resets to 0 every world load while the host's keeps its history), the chase
// is capped at 1 deg/s through a soft spring (minutes-scale convergence), calm
// periods random-walk per-peer, and a BeginPlay rand(0.9,1.0) skews blade rate.
// So the fix is a low-rate authoritative float mirror, NOT a pose stream: the
// client writes the six driver floats raw and the turbine's OWN tick keeps
// running (its head spring IS the interpolator). No suppression -- nothing
// RNG-driven is alive on the client (setRandRot is dead code).
//
// Identity: quantized world position (the grime PosKey shape) -- turbines are
// static, and one map-baked PAIR shares a save Key (untitled_178), so a
// Key-only index would collapse them.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct TurbineStatePayload;
}  // namespace coop::net

namespace coop::turbine_sync {

// Store the session + resolve/index lazily. Net-pump install path. Game thread.
void Install(coop::net::Session* session);

// Per net-pump tick: throttled index rebuild + deferred-apply retry; HOST
// additionally polls ~1 Hz and broadcasts changed turbines. Game thread.
void Tick();

// Receiver entry (event_feed dispatch): apply a turbine state (client only;
// defers until the turbine streams in). Game thread.
void OnReliable(const coop::net::TurbineStatePayload& payload);

// HOST: snapshot every indexed turbine to a freshly world-ready client.
void QueueConnectBroadcastForSlot(int peerSlot);

// Session teardown: clear the index + last-known + pending state.
void OnDisconnect();

}  // namespace coop::turbine_sync
