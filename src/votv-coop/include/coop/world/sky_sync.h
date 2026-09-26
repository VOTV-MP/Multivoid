// coop/world/sky_sync.h -- host-authoritative NIGHT-SKY sync (star-dome orientation, moon phase, the
// sky eye). A tiny host->client push (1 Hz + a joiner's world-ready); the client writes the dome and the
// moon directly, the BP's own per-tick code rendering them, and runs the sky's setEye for the eye. Fixes the
// per-peer-random star orientation + save-derived moon
// phase that the clock (coop/world/time_sync) does NOT cover: the clock converges the sun/moon
// orbit + brightness, not the random dome yaw / save moonPhase. The eye is the noon roll's, which
// runs on every peer: the host's crosses, and a client's own setEye is refused at the gate.

#pragma once

namespace coop::net {
class Session;
struct SkyStatePayload;
}  // namespace coop::net

namespace coop::sky_sync {

// Store the session, resolve newsky_C (retried via Tick until it loads) and, once per process, ask the
// script gate to watch newsky_C.setEye for a client's own. Game thread.
void Install(coop::net::Session* session);

// CLIENT receiver: apply the host's sky orientation, moon phase and eye (host early-returns -- it
// is authoritative). Called from event_feed's reliable drain.
void OnReliable(const coop::net::SkyStatePayload& payload);

// HOST-only: send one sky snapshot to a client whose world just went ready (ConnectReplayForSlot), so it
// converges at once; a joiner meets the host's eye through it, the save carrying none. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// HOST-only per-tick throttled push of the current sky state. Game thread.
void Tick();

// Session teardown: reset the throttle clock, and say how many of its own setEye calls a client
// refused and how many of the host's eye samples did not apply.
void OnDisconnect();

}  // namespace coop::sky_sync
