// coop/player/puppet_drive.h -- per-slot remote-player PUPPET lifecycle and drive.
//
// Owns the g_puppets array (slot convention = coop::players::Registry: 0 is the host,
// 1..kMaxPeers-1 the clients) and the whole per-tick puppet surface: spawn on first pose (retry
// backoff, skins, cached nick, the join announce at the appearance seam), the pose and ragdoll
// drives, the per-puppet interp Tick, the wisp grab-hold choreography -- it MOVES puppets, so it
// is a drive concern -- and the 1 Hz pose-diag emit. net_pump still owns the tick ORDER: it
// calls DriveTick inside its world-up gate, then remote_prop. Game thread throughout, enforced
// by the GT asserts.

#pragma once

namespace coop { class RemotePlayer; }
namespace coop::net { class Session; }

namespace coop::puppet_drive {

// Accessor for scenario branches that drive a single puppet outside the
// net path (drive / show / skin / autotest visuals / etc). Slot 1 is
// the canonical "the remote" puppet on HOST; slots 1..kMaxPeers-1 hold
// per-peer puppets in coop order. Returns a reference -- the underlying
// array is module-owned. Game thread (asserted).
coop::RemotePlayer& Puppet(int slot);

// The per-tick puppet drive: per-slot pose spawn and apply, the ragdoll pelvis drive, the
// per-puppet interp Tick, wisp grab-hold placement, the pose-diag emit. The caller
// (net_pump::Tick) gates on worldUp and passes its own g_worldReadyAnnounced load evaluated IN
// the call expression, so the observation point is the same for both writers, which run earlier
// on the game thread in the same Tick. Game thread.
void DriveTick(coop::net::Session& session, bool worldReadyAnnounced);

// Drop `slot`'s Player Element from the registry -- unconditional because it is right either way:
// a slot that never spawned a puppet may still hold the mirror Element EstablishMirrorForSlot
// installed, which this releases, and the drop early-returns when the slot is empty -- and destroy
// the puppet actor if one is live. Returns true iff a live puppet was destroyed: the disconnect
// edge logs on true, the session-end teardown calls silently. Game thread.
bool DestroySlot(int slot);

}  // namespace coop::puppet_drive
