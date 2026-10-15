// coop/interactables/desk_ping_sync.h -- the ping's verdict on the main desk is the host's. A client's own ping runs
// as its clock; its stage and three accumulators stream as visual-only snapshots, while observers keep coord_isPing
// clear so their local Blueprint does not start a second machine. Start, loop and stage sounds reach everyone through
// the desk sound lane. At its stage 3 its gate refuses the renderer's gatherSignal, with
// the out parameters written false, and so the failure line and sound that follow in that process_coords body; the
// verdict intent carries its view and aim to the host. The host writes them and primes its own machine to stage 2
// complete from the pre of its next process_coords, so that very body rolls the verdict against the sky of that moment
// and runs the native consequences, whose lines, sound, catch and dishes cross on their own lanes. The host keeps back
// that body's stage-change cue, and hands the pinger the catch's authorship and the find on its profile. The cheat
// menu's isntaCatchSignal, the same verdict outside the machine, crosses as its own op, and the host runs its own
// where its game lets a player cheat. Game thread, every function.

#pragma once

#include <cstdint>

namespace coop::net { class Session; struct DeskPingVerdictPayload; }

namespace coop::desk_ping_sync {

void Install(coop::net::Session* session);

// Per pump tick: registers the watches until they are live or refused.
void Tick();

// DeskPingVerdict from `senderSlot`: a HOST takes a client's verdict, a CLIENT a refusal or its find.
void OnMessage(coop::net::Session& session, const coop::net::DeskPingVerdictPayload& p, int senderSlot);

// HOST: a peer left: its budget resets, and a verdict of its that waits for the desk is dropped. One the machine has
// rolled finishes as the host machine's run (desk_input_sync::OnPeerLeft).
void OnPeerLeft(uint8_t slot);

void OnDisconnect();

// What this peer did, for the drill.
struct Counts {
    uint32_t refused;     // CLIENT: verdicts its gate refused and sent to the host
    uint32_t unsent;      // CLIENT: verdicts its gate refused and could not send, their aim or outputs unreadable
    uint32_t outputsHeld; // CLIENT: failure lines and sounds held back after a refused verdict
    uint32_t answered;    // CLIENT: refusals from the host shown as a failed ping
    uint32_t finds;       // CLIENT: finds the host handed this profile
    uint32_t primed;      // HOST: client pings primed into its desk
    uint32_t instas;      // HOST: client insta-catches dispatched on its desk
    uint32_t rolled;      // HOST: client verdicts its desk rolled
    uint32_t caught;      // HOST: client verdicts that caught
    uint32_t busy;        // HOST: intents refused while its desk was pinging or held another's verdict
    uint32_t overBudget;  // HOST: intents dropped unanswered past their sender's budget
};
Counts CountsNow();

}  // namespace coop::desk_ping_sync
