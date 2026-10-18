// coop/interactables/desk_verb_intent.h -- a client's press on the main desk's save family, as an intent the
// host performs.
//
// The family: SAVE and DELETE on the download unit, the deck's drive button (a drive's data imported into the
// list, or the selected row exported onto an empty drive) and its send, the refiner's upload, start and stop.
// Each writes shared state -- the saved-signal list, the caught signal, a drive's data, the refiner's data and
// its decode, the laptop's list -- whose lanes carry one author, so a press run on a client's copy made the
// client a second one. A client's gate on actionOptionIndex refuses a press on one of the seven buttons and sends
// DeskVerb with what the button acts on as the client saw it: the caught signal, the deck's selected row by
// content hash (row order differs per peer), the slot's drive and its row, the refiner's row. The host checks the
// presser's reach, finds each on its own desk and replays the press with the presser's puppet as the player
// (desk_press::PressForAnother), its writes reaching every peer on their own lanes; a miss is answered to the
// presser alone. The press's gloss and sounds are the presser's (desk_verb_effects), as is the decode a start
// begins (comp_sync). Every other button runs where it is pressed; the Kerfus intent is the shape. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct DeskVerbPayload;
}  // namespace coop::net

namespace coop::desk_verb_intent {

// Watch the desk's press and install the effects' seams. Idempotent; retried until each settles. Game thread.
void Install(coop::net::Session* session);

// Settle the seams; HOST: run one queued press per peer per token. Game thread.
void Tick(coop::net::Session& session);

// DeskVerb from `senderSlot`: a HOST queues a client's press, a CLIENT takes its verdict and effects.
void OnMessage(coop::net::Session& session, const coop::net::DeskVerbPayload& p, int senderSlot);

// Session end: the counts go, said. A sender's queue, its bucket and its said-once latch live in its per-slot
// state and go with its occupant.
void OnDisconnect();

}  // namespace coop::desk_verb_intent
