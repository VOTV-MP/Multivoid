// coop/interactables/woodchipper_gate.h -- on a client, a woodchipper runs nothing of its own: the host's machine does.
//
// A chipper (woodchipper_C) burns its fuel in ReceiveTick and eats a prop that enters its `eat` box, destroying the prop
// and spawning a garbage clump, so a client's copy running either would be a second author of the prop's death and of
// the clump. The host's chipper is the machine; the client's copy shows the fuel and the on/off the host sends
// (coop/interactables/woodchipper_sync). The refusal is at the script-body gate by class and function name, the shape of
// coop/props/food_tick_gate and coop/world/plant_gate: rows on woodchipper_C's ReceiveTick and its eat BeginOverlap.
//
// What a client's player does at the chipper is asked of the host, since only the host's chipper counts:
//   - the pour: the copy's own gascanFuel runs as the game wrote it, so the can in the client's hand drains and sounds;
//     a watch reads the copy's fuel before and after and asks the host to add the difference (WoodchipperIntent, Pour);
//   - the button: a press of the button (action 4 on button_del) is refused and asked as the state the player wanted
//     (WoodchipperIntent, SetActive); the copy changes when the host's state returns.
// Every other press runs natively. A single-player game and the host run all of it. Game thread.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::woodchipper_gate {

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
    uint64_t eatHost = 0;   // the begin-overlaps the host's own chipper ran: seen, never changed
    uint64_t poursAsked = 0, pressesAsked = 0, notSent = 0;
};
Counts GetCounts();

}  // namespace coop::woodchipper_gate
