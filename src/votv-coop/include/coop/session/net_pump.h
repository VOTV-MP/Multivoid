// coop/net_pump.h -- main per-tick net pump ORCHESTRATOR.
//
// Owns the peer puppet array, the per-slot connect/disconnect edge logic,
// the death policy + flee-to-menu, the dead-prop reaper, and the puppet
// pose/ragdoll drive. The sync-module fan-out lists live in coop/subsystems
// (the wiring registry) and the outbound local pose/held-prop/ragdoll
// streams in coop/local_streams -- net_pump calls both at the right tick
// moments. Driven from the harness timeline tick at the game-thread post
// rate.
//
// State previously lived in harness.cpp as file-scope globals; extracted
// per the audit (`research/findings/votv-coop-audit-post-pr4-7-2026-05-28.md`)
// to keep harness.cpp at its boot/scenario glue role.
//
// Scenario branches in harness.cpp that drove a single puppet for
// non-net visual tests (drive / show / skin) reach the slot-1 puppet via
// Puppet(1) -- it IS the canonical "the remote" puppet on HOST.

#pragma once

namespace coop { class RemotePlayer; }
namespace coop::net { class Session; }

namespace coop::net_pump {

// Per-tick main pump. Game thread only. Reads local pose, drives per-slot
// puppet spawn + pose interp, fires per-slot connect/disconnect edges,
// drains every subsystem's TickConnect / DrainChunk / Tick + the
// event_feed. `displayOffsetX` shifts the rendered puppet sideways for
// the loopback mirror scenario (0 for real coop).
void Tick(coop::net::Session& session, float displayOffsetX);

// Called from the harness Start sites (play + netloopback) BEFORE
// session.Start. Resets edge-detector flags so a session stop/restart
// doesn't carry stale "was connected" / "was holding prop" state into
// the new session. Mirrors event_feed::OnSessionStart's contract.
void OnSessionStart();

// Route a dead session back to the main menu (the user must always know the session
// ended). Used by the harness on the session running->stopped edge -- covers a HOST
// session death, which net_pump's own client-side flees don't. Idempotent (one travel
// per session); reuses the validated transparent-bypass + transition("/Game/menu")
// path. Game thread only.
void FleeToMainMenuOnDeath(coop::net::Session& session, const char* why);

// Accessor for scenario branches that drive a single puppet outside the
// net path (drive / show / skin / autotest visuals / etc). Slot 1 is
// the canonical "the remote" puppet on HOST; slots 1..kMaxPeers-1 hold
// per-peer puppets in coop order. Returns a reference -- the underlying
// array is module-owned.
coop::RemotePlayer& Puppet(int slot);

}  // namespace coop::net_pump
