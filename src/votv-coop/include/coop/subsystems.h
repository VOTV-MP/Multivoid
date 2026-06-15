// coop/subsystems.h -- the sync-module wiring registry.
//
// One place that knows EVERY coop sync module and fans the five lifecycle
// moments out to them: observer Install, per-joiner connect replay, per-slot
// disconnect cleanup, aggregate session teardown, and the per-tick gameplay
// chain. Extracted from net_pump.cpp 2026-06-12 (1290 LOC, past the modular
// soft cap): the fan-out lists are what every new feature edits, and the
// disconnect-edge vs death-teardown lists had already drifted into two
// near-identical hand-maintained copies (RULE 2 -- one list, one owner).
//
// net_pump.cpp stays the per-tick ORCHESTRATOR (connection edges, death
// policy, reaper, puppet drive) and calls into this registry at the right
// moments. A new sync feature wires in HERE (Install + TickGameplay +
// DisconnectAll + optionally ConnectReplayForSlot) and never touches
// net_pump again.
//
// MTA shape: CClientManager owns the sub-manager list and fans DoPulse /
// connect / disconnect out to them (reference/mtasa-blue
// Client/mods/deathmatch/logic/CClientManager.cpp); the pump stays thin.

#pragma once

#include <cstddef>

namespace coop::net { class Session; }

namespace coop::subsystems {

// Top-level observer orchestrator: retried each tick. Idempotent --
// each subsystem's own Install() short-circuits once it has succeeded.
// Called from net_pump::Tick AND from the non-net "play" scenario branch
// in harness.cpp (single-instance hands-on play also wants the local
// grab/prop/weather/npc/item observers running for solo-play behavioural
// parity with networked play). Game thread only.
void Install(coop::net::Session& session);

// v56: the HOST's per-joiner connect replay (snapshot bracket + every
// connect-time state broadcast), fired by the joiner's ClientWorldReady
// (event_feed) -- NOT by the connect edge: a menu-mode joiner is connected
// long before it has a world to land this on. Game thread (event_feed drain).
void ConnectReplayForSlot(int slot);

// CLIENT -> HOST (slot 0) connect edge: announce LOCAL flashlight state so
// the host can show it on our puppet; send the save-transfer request if this
// join armed one (menu-mode browser join); open OUR world-ready send gate
// toward the host immediately (the HOST always has a world -- the gate exists
// for host->joiner traffic).
void ClientConnectEdge(coop::net::Session& session);

// Per-slot disconnect cleanup: abort in-flight per-slot streams (snapshot
// drain, save transfer), close the slot's world-ready send gate, and drain
// subsystems with per-slot state. Shared by the per-slot disconnect edge AND
// the local-death teardown (which previously omitted the save-transfer cancel
// + world-ready gate close -- a host dying mid-save-transfer kept the
// in-flight stream armed across the session; unified 2026-06-12).
// Does NOT touch the slot's puppet -- the caller owns g_puppets.
void DisconnectSlot(coop::net::Session& session, int slot);

// Aggregate teardown: every subsystem with session-wide state (the per-slot
// edge above handles per-slot state). Stashed state belongs to the now-dead
// session; replaying it on the next session (possibly a different machine
// after IP change) would carry wrong-peer state. A fresh snapshot enqueues on
// the next connected edge. Shared by the all-peers-gone disconnect edge AND
// the local-death synchronous teardown.
struct DisconnectStats {
    size_t initProcessedDropped = 0;  // prop_lifecycle dedupe entries dropped
    size_t snapPending = 0;           // un-enumerated snapshot candidates cleared
};
DisconnectStats DisconnectAll();

// Per-tick gameplay-world subsystem chain: the connect-broadcast retry drains,
// every module's poll/apply Tick, the NPC pose stream/mirror, the trash
// death-watches, and the dev probes. The caller gates this on worldUp (all of
// it acts on gameplay-world state that cannot exist at the menu) and passes
// `fleeing` so the transition-window gate (g_fleeing || join_progress::Active)
// can suppress the death-watch destroy false-positives during stream-out.
void TickGameplay(coop::net::Session& session, bool isConnected, bool isHost,
                  bool fleeing);

}  // namespace coop::subsystems
