// coop/player/puppet_drive.h -- per-slot remote-player PUPPET lifecycle + drive.
//
// Extracted from coop/session/net_pump.cpp (2026-07-18; net_pump sat at 1237
// LOC with a 915-line Tick). Owns the g_puppets array (slot convention =
// coop::players::Registry: 0 = host, 1..kMaxPeers-1 = clients) and the whole
// per-tick puppet surface: spawn-on-first-pose (retry backoff, skins, cached
// nick, the join announce at the appearance seam), the pose/ragdoll drives,
// the per-puppet interp Tick, the wisp grab-hold choreography (it MOVES
// puppets -- drive concern), and the 1 Hz pose-diag emit. The bodies are
// verbatim from net_pump's old drive block; net_pump still owns the tick
// ORDER (it calls DriveTick inside its worldUp gate, then remote_prop).
// Game thread throughout (the moved GT asserts enforce it).

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

// The per-tick puppet drive, extracted whole from net_pump:
// per-slot pose spawn/apply, ragdoll pelvis drive, per-puppet interp Tick,
// wisp grab-hold placement, pose-diag emit. Caller (net_pump::Tick) gates on
// worldUp and passes its own g_worldReadyAnnounced load evaluated IN the call
// expression (the same observation point as the old in-loop load -- both
// writers run GT-earlier in the same Tick). Game thread.
void DriveTick(coop::net::Session& session, bool worldReadyAnnounced);

// Drop slot's Player Element from the registry (unconditional -- safe when no
// puppet ever spawned, see the N-3 note at the call site) and destroy the
// puppet actor if one is live. Returns true iff a live puppet was destroyed
// (the disconnect edge logs on true; the session-end teardown calls silently).
// Game thread.
bool DestroySlot(int slot);

}  // namespace coop::puppet_drive
