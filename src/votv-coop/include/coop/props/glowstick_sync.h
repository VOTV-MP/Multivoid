// coop/props/glowstick_sync.h -- a glowstick stuck to a surface is stuck on every peer.
//
// A held glowstick ticks a short ray from its tip; when it strikes, the game drops the stick from the hand and its
// stickTo hangs it by a physics constraint from a hook at the struck point or from the struck body, and one frame later
// setFrame writes the constraint's frames. A grab frees it (releaseGlowstick). All of that runs on the acting machine
// only, and a constraint is no field a pose or a record moves. The stick is save state, though: the glowstick's own
// getData carries it (coop/props/glowstick_stick_record), so the prop record lane (coop/props/prop_save_data) does. A
// player's own stick, once its setFrame has ended and the struck prop's key is written, is published (a client's as an
// intent the host judges: glowstick_stick_judge); on every peer the landing record is followed by the game's own restore
// (processKeys), once. A grab frees every peer's copy at its hold's begin, a client sends no release, the author's own
// stick coming back is matched by content and cancelled, and a stick whose struck prop is not bound here waits for it
// (glowstick_struck).

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

// Two departures from the references, deliberate. The client PREDICTS its stick and the host repairs it: MTA waits for the
// server's reply before the world changes (CGame.cpp:3041-3043), Source predicts only the entities it owns and restores them
// to the server's state (c_baseplayer.cpp:2324); here the single-player Blueprint runs `stickTo` on the acting machine and
// has no seam to wait, so the host's judge undoes a refused stick with its own record. The author's echo is matched by
// CONTENT, not by a number: MTA's sync time context and Source's command acknowledgement are counters, and this lane's record
// has no field for one (a wire change); an echo equal to a stick in flight is the host's republish of the author's own bytes.

namespace coop::glowstick_sync {

// Caches the session and registers the four watches (stickTo, setFrame, releaseGlowstick, loadData). Idempotent. Game thread.
void Install(coop::net::Session* session);

// Per pump tick: publishes the record of each glowstick a player's own stick ended or the host freed since the last pass;
// makes the stick of each waiting copy whose struck prop has bound.
// Game thread.
void Tick();

// A remote hold of `prop` began on this peer (remote_prop::ResolveAndStartDrive, once per hold). A stuck glowstick
// is freed on this copy by the game's own releaseGlowstick, as the grabber's own game frees its copy; no message
// goes of it from a client, and the host publishes its record of the freed glowstick to every peer. Game thread.
void OnHoldBegin(void* prop);

// HOST: `slot`'s hold of `actor` was refused. A glowstick's record, the host's own, goes to that slot, so a copy
// the grab freed there is put back as the host holds it. Game thread.
void OnHoldRefused(int slot, void* actor);

// HOST: the actor is being destroyed (the destroy seam). The glowsticks that hung from it have their records
// published on the next pass, after the game has acted on its own copies. Game thread.
void NoteDestroyed(void* actor);

// Session teardown: the pending publishes and the landing marks go.
void OnDisconnect();


namespace dev {   // the glowstick drill's events
struct Counts {
    uint32_t published = 0;   // this player's own sticks sent (a client) or published (the host)
    uint32_t landed = 0;      // records that said stuck and landed on a glowstick here
    uint32_t made = 0;        // sticks the game's own restore made on a copy here
    uint32_t heldFree = 0;    // copies freed at a remote hold's begin
    uint32_t echoes = 0;      // landings that were this client's own stick coming back
};
Counts Get();
}  // namespace dev

}  // namespace coop::glowstick_sync
