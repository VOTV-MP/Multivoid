// coop/interactables/atv_part_intent.h -- a client's work on an ATV's wheels, spare and battery
// (AtvPartIntent), run by the host.
//
// Which tyres are on, whether the spare is and whether a battery is fitted are presence, taken from
// the host's own copy only (atv_condition_sync), and a client's births do not cross. So the bodies
// that change them are cancelled on a client at the script gate and asked of the host: unscrewPanel
// (a wheel or the battery unbolted), putTire (the held wheel fitted), playerUsedOn's spare-stow and
// battery-insert branches, and actionOptionIndex's spare take. A fit, stow or insert destroys the
// held part on the client as the body would and carries its scalars. The host judges the sender and
// the ATV, runs the game's own unscrew or writes the fields the body writes, and the part's birth
// and the presence in AtvState cross as the host's; a refused fit, stow or insert is refunded as a
// prop beside the sender, and a taken spare is born there too. docs/vehicles.md carries the lane.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct AtvPartIntentPayload;
}  // namespace coop::net

namespace coop::atv_part_intent {

// Idempotent: caches `session` and registers the four script-gate watches on ATV_C, once.
void Install(coop::net::Session* session);

// HOST: a client's AtvPartIntent (router: event_dispatch_atv_part.cpp). Game thread.
void OnIntent(coop::net::Session& session, const coop::net::AtvPartIntentPayload& payload, uint8_t senderSlot);

// Counters for the session-end line and the drill.
struct Stats {
    uint64_t asked;     // CLIENT: bodies cancelled here and asked of the host
    uint64_t ran;       // HOST: asked changes it ran
    uint64_t refused;   // HOST: asked changes it refused
    uint64_t refunded;  // HOST: refused fits, stows and inserts it gave back as a prop
};
Stats GetStats();

// Session end: the counters are said and cleared, the senders' budgets filled.
void OnDisconnect();

}  // namespace coop::atv_part_intent
