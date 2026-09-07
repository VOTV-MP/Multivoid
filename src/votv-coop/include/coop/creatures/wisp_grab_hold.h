// coop/creatures/wisp_grab_hold.h -- killerwisp grab-window body placement, on every peer.
//
// The native kill sequence is hard-bound to player 0 -- socket attach, fatality, lift -- so a
// puppet victim got a flat death while the wisp mimed the tear alone. This module puts the BODIES
// where the native choreography puts them: the victim peer replays the native Capture template
// against its own player and its local wisp mirror, every other peer holds the victim's PUPPET at
// the wisp's 'playerGrab' socket.
//
// That hold is a per-tick SetActorLocation follow, not an engine attach, because the puppet pose
// apply would fight an attach every frame; it overwrites after it instead, so Tick() must run after
// the puppet Tick loop in puppet_drive::DriveTick -- the call site IS the ordering. Nothing sends a
// release: the hold ends on its own liveness guards, and the victim window on the kill deadline it
// rode in with. wisp_attack_sync owns WHO is attacked and the host lift, wisp_tear_mirror the
// montage and the death; this owns only placement. Game-thread only.

#pragma once

#include <cstdint>

namespace coop::wisp_grab_hold {

// VICTIM peer: our own player was grabbed (WispGrab accepted). Attach the local player
// to the local mirror of wisp `wispEid` (retry each Tick until it resolves, bounded by
// `killDelayMs` -- the mirror can race the reliable message by a frame). Game thread.
void EngageSelf(uint32_t wispEid, uint32_t killDelayMs);

// HOST/THIRD peer: hold the puppet of `victimSlot` at wisp `wispEid`'s 'playerGrab'
// socket until the wisp or the puppet goes away. Idempotent per (slot). Game thread.
void EngagePuppet(uint32_t wispEid, uint8_t victimSlot);

// VICTIM peer: undo the self grab (detach + restore movement/camera flags). Called by
// wisp_tear_mirror right before the ragdoll death fires, and by OnDisconnect (a
// mid-grab session teardown must not strand the player in MOVE_None). Safe no-op when
// not engaged. Game thread.
void ReleaseSelf();

// Per-tick pump: victim-side attach retry + every puppet hold (resolve wisp + puppet,
// snap the puppet to the socket, self-release on liveness misses). MUST be called
// AFTER the puppet pose apply loop (see the header note). Cheap no-op when idle.
void Tick();

// Peer `slot` disconnected -- drop its puppet hold (the puppet actor is going away).
void OnPeerLeft(uint8_t slot);

// Full reset (session teardown). Releases a live self-grab first.
void OnDisconnect();

}  // namespace coop::wisp_grab_hold
