// coop/interactables/woodchipper_gate.h -- on a client, a woodchipper runs nothing of its own: the host's machine does.
//
// A chipper (woodchipper_C) burns its fuel in ReceiveTick and eats a prop that enters its `eat` box, destroying the prop
// and spawning a garbage clump, so a client's copy running either would be a second author of the prop's death and of
// the clump. The host's chipper is the machine; the client's copy shows the fuel and the on/off the host sends
// (coop/interactables/woodchipper_sync). The refusal is at the script-body gate by class and function name, the shape of
// coop/props/food_tick_gate and coop/world/plant_gate: rows on woodchipper_C's ReceiveTick and its eat BeginOverlap, and on
// its pour (gascanFuel), its press (actionOptionIndex) and its ubergraph (the host's intake resume, below).
//
// What a client's player does at the chipper is asked of the host, since only the host's chipper counts:
//   - the pour: the copy's own gascanFuel runs as the game wrote it, so the can in the client's hand drains and sounds;
//     a watch reads the copy's fuel before and after and asks the host to add the difference (WoodchipperIntent, Pour);
//   - the button: a press of the button (action 4 on button_del) is refused and asked as the state the player wanted
//     (WoodchipperIntent, SetActive); the copy changes when the host's state returns.
// Every other press runs natively, and a host or single-player chipper ticks and eats as written (plus the flip below).

#pragma once

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::woodchipper_gate {

// What the host's chipper eats, it eats as the game wrote it: the begin-overlap on its eat box schedules a Delay(0), and
// the resume asks canChip, which wants a SIMULATING body. A prop a client holds or throws is, on the host, a copy the
// drive stream positions with its simulation off, and the overlap that met it does not refire, so the resume refuses it.
// The host's gate therefore sets that one copy simulating for the length of the resume and sets it back when the chipper
// did not take it: the game's own canChip, its fuel, its busy flag and its material list then judge the copy, and a
// refusal leaves the client's drive exactly as it was.

// Register the watches, once for the process, ahead of a joining client's world load: its chippers tick from that
// world's first frame. A watch that did not register is said in one ERROR line. The refusal reads the session
// spawn_authority holds, so spawn_authority::Install runs first, at the same call site. Each watch is live once the
// game thread resolves its names (script_gate::ResolvePendingNames, driven by the lanes' ticks). Any thread.
void Install(coop::net::Session* session);

// Once the registered watches have all settled (their names resolved, the class asked), a row that settled dead is said
// in one WARN, naming what a client chipper will then run on its own. A settled gate asks nothing. Game thread, from the
// chipper lane's tick (coop/interactables/woodchipper_sync).
void SettleWatches();

// Session end: say what the gate did this session and start the counts again from zero (the watches stay registered).
void OnDisconnect();

// What the gate did this session, for drills: the tick and intake bodies it refused, and the asks it sent. Game thread.
struct Counts {
    uint64_t tickRefused = 0, eatRefused = 0;
    uint64_t eatHost = 0;   // the begin-overlaps the host's own chipper ran (the overlap itself is only seen)
    uint64_t poursAsked = 0, pressesAsked = 0, notSent = 0;
};
Counts GetCounts();

// HOST, for drills ONLY: what the chipper's resume did with a prop that entered its eat box, the newest resume of each
// prop (sixteen are kept), by the prop's key, or by its element id (`eid:<n>`) when it holds none. A resume of a prop no
// client drove is recorded too, with `slot` -1. This is a diagnostics record, the chipper feed drill's verdict input: the
// gate fills it at every resume and nothing in the product reads it, so no behaviour depends on it. Game thread.
struct Intake {
    int  slot = -1;           // the client whose drive moved the host's copy at the resume, -1 when none
    bool simulating = false;  // the copy's mesh simulated when the resume began, before any flip
    bool ready = false;       // the chipper was on, idle and fuelled when the resume began
    bool flipped = false;     // the gate set the copy simulating for this one resume
    bool took = false;        // the chipper's own intake took the prop (its `process` rose and the prop was destroyed)
};
bool IntakeOf(const std::wstring& key, Intake& out);

}  // namespace coop::woodchipper_gate
