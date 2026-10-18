// coop/interactables/eraser_press_intent.h -- the drive eraser's delete button, as a client's intent the host presses.
// Overview: docs/signals.md. Gameplay/network layer (principle 7); the engine is reached through ue_wrap.
//
// The eraser (signalDriveEraser_C) is level-placed, one per world, with no key and no element id. Its delete button,
// action 4, starts a 3 s Delay while its slot holds a drive, and the resume wipes the drive in THAT peer's slot, so a
// client's press wiped only its own copy. A drive's row is the host's (drive_payload_sync), so this lane refuses the
// client's own press at the script-body gate and sends it, naming the drive it saw seated; the host runs it with the
// drone console's reach test, mid-join wait and rate limit (drone_call_intent) once its own eraser's slot holds that
// drive, waiting only while the slot line naming it waits for the drive to bind, and the wipe reaches every peer as
// the host's row. Every press the host's eraser runs, and its resume, are shown by each client's own eraser (its
// sounds and its wiper), the presser's included; a press the host refuses is shown to its presser alone. Nothing to
// replay on a late join: the wipe's row is the drive lane's, and a press's show lasts its 3 s.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EraserPressIntentPayload;
}  // namespace coop::net

namespace coop::eraser_press_intent {

// Watch the eraser's press and its resume, and cache the session. Idempotent.
void Install(coop::net::Session* session);

// Settle the watches; HOST: run one queued press a tick a sender, or wait for its body or its drive's insert.
void Tick(coop::net::Session& session);

// HOST: a client's press. CLIENT: the host's press or resume, to show (event_dispatch_intent).
void OnEraserPressIntent(coop::net::Session& session, const coop::net::EraserPressIntentPayload& payload,
                         uint8_t senderSlot);

// Session end: the counters go, said. A sender's queue, its bucket and its said-once latch live in its per-slot
// state and go with its occupant.
void OnDisconnect();

// For a drill: presses this client sent, presses the host ran, and host events this client showed.
uint64_t SentCount();
uint64_t PressedCount();
uint64_t ShownCount();

}  // namespace coop::eraser_press_intent
