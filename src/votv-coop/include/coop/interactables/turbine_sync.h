// coop/interactables/turbine_sync.h -- giant wind-turbine facing and spin mirror
// (ReliableKind::TurbineState). HOST-authoritative at about 1 Hz per turbine.
//
// WHY. The turbine blueprint is a per-tick servo chasing the directionalWind direction, which
// weather_sync already mirrors, so agreeing on the wind is not enough to agree on where a turbine
// points. Its integrator `rot` is not saved -- only alpha_blades and headRotation round-trip -- so
// it restarts at zero every world load while the host's keeps its history; the chase is capped at
// 1 deg/s and reaches the nacelle through a spring, so a gap closes over minutes; and each
// instance draws its blade-rate multiplier from a BeginPlay random in [0.9, 1.0].
//
// So this mirrors the six driver floats at a low rate rather than streaming a pose: the client
// writes them raw and the turbine's OWN tick keeps running, its head spring serving as the
// interpolator. Nothing is suppressed, because nothing random-walks behind our back -- the
// blueprint's one RNG timer, setRandRot, is never armed. Identity is a quantized world position,
// the grime lane's shape: turbines are static and one map-baked PAIR shares a save Key.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct TurbineStatePayload;
}  // namespace coop::net

namespace coop::turbine_sync {

// Store the session + resolve/index lazily. Net-pump install path. Game thread.
void Install(coop::net::Session* session);

// Per net-pump tick: the throttled deferred-apply retry -- the index itself is
// refreshed on the scan hub's own cadence, not here -- and on the HOST a further
// poll at about 1 Hz that broadcasts changed turbines. Game thread.
void Tick();

// Receiver entry (event_feed dispatch): apply a turbine state (client only;
// defers until the turbine streams in). Game thread.
void OnReliable(const coop::net::TurbineStatePayload& payload);

// HOST: snapshot every indexed turbine to a freshly world-ready client.
void QueueConnectBroadcastForSlot(int peerSlot);

// Session teardown: clear the index + last-known + pending state.
void OnDisconnect();

}  // namespace coop::turbine_sync
