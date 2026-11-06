// coop/interactables/woodchipper_sync.h -- a woodchipper's fuel and its on/off are the host's, and every peer shows them.
//
// A woodchipper (woodchipper_C, a keyed prop_C) keeps `fuel` (0..100) and `active`. A gas can fills it through its
// gascanFuel, the button flips `active` through its actionOptionIndex, and its own tick burns fuel while it spins and
// switches it off at 0. The host's chipper is the machine: a client's copy runs no tick and no intake
// (coop/interactables/woodchipper_gate), and what its player does at the machine is asked of the host.

// HOST: every edge is announced by the game's own checkActive (every pour, every press with fuel in the tank, the burn's
// switch-off) and by the button's body (a press at fuel 0 skips checkActive), so a script-gate POST on each puts the
// chipper in the state table and marks it changed; one WoodchipperState per changed chipper leaves per tick. The burn
// is the one thing nothing announces, so while a chipper's fuel moves it is sent again once a second.

#pragma once

// A client's pour (the fuel its copy gained) or press (the state it wanted) is a WoodchipperIntent, queued per sender at
// a bounded rate and judged -- world, chipper by key and class, reach, a dead player, the gas can in its hand and the
// room in the tank, the state wanted -- and done on the host's copy: the fuel added and the chipper's own checkActive, or
// the game's own actionOptionIndex with the sender's puppet as its player, inside a requester_feedback scope. A refused
// intent is answered with the state, to the sender alone, whenever the host can name the chipper (a key that names none
// has no state to send).

// CLIENT: a state is written onto the copy (fuel and active), and when the on/off changed the game's own checkActive runs
// on it, so the copy's motor, particles and sounds start as in single player; one for a chipper not resolved yet is
// held, the newest per key, and the held keys are retried when the prop index moves. JOIN: the record lane carries a chipper's fuel with its
// spawn; at the joiner's ready edge the host sends which chippers are on (`active` alone), after the record. Game thread.

namespace coop::net {
class Session;
struct WoodchipperPayload;
}  // namespace coop::net

namespace coop::woodchipper_sync {

// Store the session, register the host's two POST watches and requester_feedback's seams. Idempotent; called at each
// session start.
void Install(coop::net::Session* session);

// The host's queues and sends, the client's held states. Game thread, once per pump tick.
void Tick();

// A client's receiver: the host's chipper.
void OnState(const coop::net::WoodchipperPayload& p);

// The host's receiver: a client's pour or press, its format already checked by the dispatcher.
void OnIntent(int senderSlot, const coop::net::WoodchipperPayload& p);

// The chippers that are on, to a joiner, at its ready edge.
void QueueConnectBroadcastForSlot(int slot);

// A chipper left the world (the prop destroy seam): its table row goes. Called for every actor destroy, so it costs a
// compare when no chipper is tracked.
void NoteDestroyed(void* actor);

// The session ended: everything is forgotten.
void OnDisconnect();

// What the drill reads of the host's lane. Game thread.
struct DrillView {
    unsigned long long pours = 0, presses = 0, clamped = 0, answers = 0;
    unsigned long long refusedReach = 0, refusedCan = 0, refusedFull = 0, refusedAlready = 0, refusedOther = 0;
    unsigned long long statesSent = 0, replayed = 0;   // host
    unsigned long long applied = 0, held = 0;          // client
};
DrillView ReadForDrill();

}  // namespace coop::woodchipper_sync
