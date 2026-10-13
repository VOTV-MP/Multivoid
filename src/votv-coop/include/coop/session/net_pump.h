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

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::net_pump {

// Per-tick main pump. Game thread only. Reads local pose, drives per-slot
// puppet spawn + pose interp, fires per-slot connect/disconnect edges,
// drains every subsystem's TickConnect / DrainChunk / Tick + the
// event_feed.
void Tick(coop::net::Session& session);

// The session ticks this process has run, about sixty a second in play. A window a callback opens
// and a tick judges is counted in these rather than in wall time: posted work runs only at an
// outermost dispatch, so one script body -- a world load's tail, a save -- holds every tick for its
// whole length and then costs one, while a wall-clock window stamped in the callback can lapse
// before its judge first looks. Game thread.
uint64_t TickSerial();

// Called from the harness Start site (play) BEFORE
// session.Start. Resets edge-detector flags so a session stop/restart
// doesn't carry stale "was connected" / "was holding prop" state into
// the new session. Mirrors event_feed::OnSessionStart's contract.
void OnSessionStart();

// The public entry for a death the revive watchdog gave up on (the user must always know the session
// ended): it ends the session -- the stop, then the full coop teardown -- and travels to the menu.
// Idempotent (one travel per session, through the flee latch); reuses the validated
// transparent-bypass + transition("/Game/menu") path. Game thread only.
void FleeToMainMenuOnDeath(coop::net::Session& session, const char* why);

// The session's whole coop-state teardown, run once per end on the game thread whatever stopped the
// session: every slot's puppet and DisconnectSlot, DisconnectAll (which ends the script-gate hold),
// the session-scoped overlays, the peer-leave edges, the roster ledger and the final element flush.
// A no-op unless the session is stopped AND this stop has not been torn down yet (Session::StopCount
// against the last torn-down count), so a call after a new session started finds it running and does
// nothing, and a session that never ran is never torn down. Never stops anything itself; the stop is
// the caller's, on the thread the net-thread join belongs to. Game thread only.
void TearDownSession(coop::net::Session& session, const char* why);

// True when the session is stopped and its last stop has not yet been torn down. A start RunAndWaits
// TearDownSession when this is true before it begins, as MTA's Start unloads a mod still loaded
// (CModManager.cpp:134-135), so no session starts over an owed end. Reads the count with acquire.
bool EndOwed(const coop::net::Session& session);

// True while this session is already travelling to the main menu (the one-shot
// flee latch). registry_reaper's gameplay->menu guard reads it so its branch
// predicate stays literal across the extraction. Game thread.
bool IsFleeing();

// The announce axis' ONE owner: registry_reaper requests a world-ready re-announce after a
// world-change re-seed, and this owner compares against the last-announced world, arms the quiesce
// probe and sets the re-announce flag -- or logs the same-world suppression. No-op on the host.
// Game thread.
void MaybeRequestReAnnounce(coop::net::Session& session, void* reapWorld);

// The session's end for a quit to the menu whose travel the game itself runs: the session ends (the
// stop, then TearDownSession), then the no-travel flee (VOTV's own menu transition is in flight or
// about to be). Reached from the quit decision (run_end_travel posts it as its own task, before the
// travel runs) and from registry_reaper's world scan, which is the fallback for a menu arrival that
// did not pass the decision. `why` names the route in the log. Idempotent via the flee latch. Game
// thread.
void FleeAfterNativeMenuTravel(coop::net::Session& session, const char* why);

// True once THIS client has announced ClientWorldReady for the current connection (false on the
// host, which never announces; latched false again at disconnect). The meadow lane's client send
// gate reads it: a client line reaching the host before the ready flip would ride the join seed
// back as a duplicate, so peer-symmetric lanes stay mute until the announce.
bool HasAnnouncedWorldReady();

// HasAnnouncedWorldReady and not waiting on a world change's re-announce (the two terms the pose
// gate reads, without the load tail); false on the host. A client applies a host's order only then.
bool IsWorldSettled();

// Whether `obj` belongs to the world THIS client announced world-ready in: false before the announce,
// while a world change waits for its re-announce, and for an object of any other world, so a later
// world's own load is told apart at the object itself, whatever the tick order (a level load runs
// its actors' first calls before the next tick). False on the host, which never announces. A world
// the reader cannot name, the object's or the announce's, counts as the announced one. The lanes that
// refuse a client's own calls of the verbs they carry ask it: a client's load of a world is the
// host's save and runs natively. Game thread.
bool IsInAnnouncedWorld(void* obj);

}  // namespace coop::net_pump
