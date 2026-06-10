// coop/sky_sync.h -- host-authoritative NIGHT-SKY sync (star-dome orientation + moon phase).
// Mirrors coop/time_sync.h: a tiny host->client push (throttle + connect edge), the client
// direct-writes, the BP's own per-tick code keeps rendering. Fixes the per-peer-random star
// orientation + save-derived moon phase that TimeSync(29) does NOT cover (TimeSync syncs the
// clock-derived sun/moon orbit + brightness, not the random dome yaw / save moonPhase).
//
// RE: research/findings/votv-sky-stars-celestial-sync-RE-2026-06-08.md.

#pragma once

namespace coop::net {
class Session;
struct SkyStatePayload;
}  // namespace coop::net

namespace coop::sky_sync {

// Store the session + resolve newsky_C (retried via Tick until it loads). Game thread.
void Install(coop::net::Session* session);

// CLIENT receiver: apply the host's sky orientation + moon phase (host early-returns -- it is
// authoritative). Called from event_feed's reliable drain.
void OnReliable(const coop::net::SkyStatePayload& payload);

// HOST-only: send one sky snapshot to a freshly connected client so it converges immediately
// (the same connect edge that pushes the clock). Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// HOST-only per-tick throttled push of the current sky state. Game thread.
void Tick();

// Session teardown: reset the throttle clock.
void OnDisconnect();

}  // namespace coop::sky_sync
