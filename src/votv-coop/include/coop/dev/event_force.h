// coop/dev/event_force.h -- the event trigger-graph: live gate status + force-NOW.
//
// The user's 2026-07-03 report: menu-fired events "не сразу срабатывают" --
// firing "obelisk" produced nothing until the host walked from outside into the
// base. RE (map census of untitled_1 + trigger class bytecode): the eventer's
// runEvent case does NOT produce the visible effect for 14 events; it calls a
// trigger_TBoxActivator_C.runTrigger, which merely sets a level-placed
// trigger_box_N_C volume isActive=true + enables its collision (setColls). The
// visible effect fires only when the PLAYER WALKS INTO that volume: the box's
// BeginOverlap handler class-filters the actor and calls runAll(-1) ->
// fireTrigger over its stored (objects, ids) connections (obelisk: the box sits
// at the base entrance -> exactly "сработал когда зашёл на базу").
//
// This module makes that graph VISIBLE + drivable:
//   - StatusFor(): per event, whether it is volume-gated, the live IsActive /
//     remaining-shots state of its box (refreshed by a game-thread snapshot).
//   - ForceNow(): arm through the SAME seam as the fire button (HostFire -> the
//     native eventer dispatch + the v95 EventFire broadcast so clients replay
//     the arm per policy), then drive the box's OWN BeginOverlap handler with
//     the local player pawn as OtherActor -- the native class filter, N
//     bookkeeping and collision-disable all run in the game's own bytecode. No
//     game state is faked; it is the exact dispatch a real walk-in performs.
//
// Host-only (dev_gate), dev tooling -- never on the wire beyond the normal
// EventFire arm broadcast.

#pragma once

namespace coop::dev::event_force {

struct BoxStatus {
    bool hasBox = false;       // this event is gated by a level trigger volume
    bool resolved = false;     // its box actor was found live in the world
    bool armed = false;        // IsActive (collision on -> waiting for the walk-in)
    int  shots = -1;           // remaining N fires (box_N); 0 = already consumed
    const char* boxName = "";  // level actor name (tooltip/diagnostics)
};

// Render-thread-safe snapshot for the menu (last game-thread refresh).
BoxStatus StatusFor(const char* eventName);

// Post a game-thread refresh of all mapped box statuses. Internally limited to
// ~1 Hz -- safe to call every rendered frame while the events tab is open.
void RequestRefresh();

// Extra native-gate note for NON-box rows (agrav/bedEvent/signals/...), or
// nullptr. Static text mined from the eventer RE; shown in the row tooltip.
const char* GateNote(const char* eventName);

// Arm + complete the event NOW (see the header comment). False if the event is
// not volume-gated or dev features are disabled (client role).
bool ForceNow(const char* eventName);

}  // namespace coop::dev::event_force
