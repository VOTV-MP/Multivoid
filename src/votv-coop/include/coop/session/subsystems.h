// coop/session/subsystems.h -- the sync-module wiring registry.
//
// The one place that knows EVERY coop sync module, fanning the five lifecycle moments out to
// them: the observer install, the per-joiner connect replay, the per-slot disconnect cleanup,
// the aggregate session teardown and the per-tick gameplay chain. The fan-out lists are what
// every new feature edits, and they belong to one owner: kept in the pump, the
// disconnect-edge and death-teardown lists drifted into two near-identical hand-maintained
// copies.
//
// net_pump stays the per-tick ORCHESTRATOR -- connection edges, death policy, the reaper, the
// puppet drive -- and calls in here at the right moments. A new sync feature wires itself in
// HERE (Install and TickGameplay, plus DisconnectAll and optionally ConnectReplayForSlot) and
// never touches net_pump. The MTA shape: CClientManager owns the sub-manager list and fans
// DoPulse, connect and disconnect out to them, keeping the pump thin.

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

// The HOST's per-joiner connect replay (the snapshot bracket and every connect-time state
// broadcast), fired by the joiner's ClientWorldReady in event_feed and NOT by the connect
// edge: a menu-mode joiner is connected long before it has a world to land this on. Game
// thread, on the event_feed drain.
void ConnectReplayForSlot(int slot);

// CLIENT -> HOST (slot 0) connect edge: announce LOCAL flashlight state so
// the host can show it on our puppet; send the save-transfer request if this
// join armed one (menu-mode browser join); open OUR world-ready send gate
// toward the host immediately (the HOST always has a world -- the gate exists
// for host->joiner traffic).
void ClientConnectEdge(coop::net::Session& session);

// Per-slot disconnect cleanup: abort the slot's in-flight streams (the snapshot drain, the
// save transfer), close its world-ready send gate, and drain the subsystems holding per-slot
// state. Shared by the per-slot disconnect edge AND the local-death teardown, which is why it
// is one function: as two, the death path omitted the save-transfer cancel and the gate close,
// so a host dying mid-transfer left the stream armed for the rest of the session. Does NOT
// touch the slot's puppet -- the caller owns those.
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
