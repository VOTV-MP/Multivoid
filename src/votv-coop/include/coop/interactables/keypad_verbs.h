// coop/interactables/keypad_verbs.h -- the password keypads' verbs, watched by name at the
// script-body gate; the lane (coop/interactables/keypad_sync) stands on them.
//   - CLIENT, before the body, on a keypad the lane indexes, outside the lane's own replay: a digit
//     (inputNumber), the numpad's accept and cancel keys (playerAnykey), a held keycard's open and a
//     held pass changer's reset are refused and sent to the host as KeypadIntent, read on this copy
//     as its player's input. Every other call of those verbs, open2, falseEnterEvent, and setActive
//     from anything but a keypad (the world's writers) is refused without a send.
//   - HOST, before the body: the verb goes to every client as KeypadState, to replay; after it, the
//     state its chain settled on: at setActive(false), the pair's with it; at a digit or a reset
//     that started no open. A client's copy is written to it whatever its own replay met.
//   - CLIENT, after its own chain's setActive(false): a state that waited for the chain is written.
//   - HOST: a client's intent runs on the host's copy, checked for reach, for the held item a keycard
//     or pass changer needs, and for rate, from a bounded queue per sender.
// MTA precedent: a client's vehicle entry is a request the server checks and runs
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3018, Packet_Vehicle_InOut).

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct KeypadIntentPayload;
}  // namespace coop::net

namespace coop::keypad_verbs {

// Registers the name watches. The per-tick retry pump (subsystems::Install). Game thread.
void Install(coop::net::Session* session);

// Settles the watches and says once when they are live. Host: runs the queued intents in order, each
// sender at a bounded rate. The head waits while its sender has no body to measure the reach from
// (the first pose not yet applied) and while its keypad's open is in its 0.2 s tail, so no entry is
// lost to either wait (a full queue still refuses). Game thread, once per pump tick.
void Tick(coop::net::Session& session);

// HOST: a client's intent from the wire, its format already checked by the dispatcher. Queued, and
// run by Tick; a full queue refuses it. Game thread.
void OnKeypadIntent(coop::net::Session& session, const coop::net::KeypadIntentPayload& payload,
                    uint8_t senderSlot);

// A peer left: its queued intents and its rate go with it.
void OnPeerLeft(uint8_t slot);

// The session ended: the queues, the rates and the counters, with one summary line.
void OnDisconnect();

}  // namespace coop::keypad_verbs
