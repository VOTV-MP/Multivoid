// coop/session/net_pump.h -- main per-tick net pump ORCHESTRATOR.
//
// Owns the per-slot connect and disconnect edge logic, the death policy and flee-to-menu, and the
// world-ready announce axis. The dead-prop reaper and re-seed engine live in
// coop/props/registry_reaper and the puppet array and pose/ragdoll drive in
// coop/player/puppet_drive; net_pump calls them at the right tick moments and still owns the tick
// ORDER. The sync-module fan-out lists live in coop/session/subsystems, the wiring registry, and
// the outbound local pose, held-prop and ragdoll streams in coop/player/local_streams. Driven from
// the harness timeline tick at the game-thread post rate, which keeps harness.cpp to its boot and
// scenario glue.
//
// The scenario branches in harness.cpp that drive a single puppet for non-net visual tests (drive,
// show, skin) reach the slot-1 puppet through Puppet(1) -- on the HOST that IS the canonical "the
// remote" puppet.

#pragma once

namespace coop::net { class Session; }

namespace coop::net_pump {

// Per-tick main pump. Game thread only. Reads local pose, drives per-slot
// puppet spawn + pose interp, fires per-slot connect/disconnect edges,
// drains every subsystem's TickConnect / DrainChunk / Tick + the
// event_feed.
void Tick(coop::net::Session& session);

// Called from the harness Start site (play) BEFORE
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

// True while this session is already travelling to the main menu (the one-shot
// flee latch). registry_reaper's gameplay->menu guard reads it so its branch
// predicate stays literal across the extraction. Game thread.
bool IsFleeing();

// The announce axis' ONE owner: registry_reaper requests a world-ready re-announce after a
// world-change re-seed, and this owner compares against the last-announced world, arms the quiesce
// probe and sets the re-announce flag -- or logs the same-world suppression. No-op on the host.
// Game thread.
void MaybeRequestReAnnounce(coop::net::Session& session, void* reapWorld);

// The gameplay->menu RAM-balloon guard's ACTION half (detection lives in
// registry_reaper's world scan): full coop-state teardown + the no-travel flee
// (VOTV's own menu transition is already in flight). Idempotent via the flee
// latch. Game thread.
void FleeAfterNativeMenuTravel(coop::net::Session& session);

// True once THIS client has announced ClientWorldReady for the current connection (false on the
// host, which never announces; latched false again at disconnect). The meadow lane's client send
// gate reads it: a client line reaching the host before the ready flip would ride the join seed
// back as a duplicate, so peer-symmetric lanes stay mute until the announce.
bool HasAnnouncedWorldReady();

}  // namespace coop::net_pump
