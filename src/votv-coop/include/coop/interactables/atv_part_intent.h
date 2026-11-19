// coop/interactables/atv_part_intent.h -- a client's work on an ATV's wheels and spare
// (AtvPartIntent), run by the host.
//
// Which tyres are on and whether the spare is are presence, taken from the host's own copy only
// (atv_condition_sync), and a client's births do not cross. So the bodies that change them are
// cancelled on a client at the script gate and asked of the host: unscrewPanel (a wheel unbolted),
// putTire (the held wheel fitted), playerUsedOn's spare-stow branch, and actionOptionIndex's spare
// take. A fit or stow destroys the held wheel on the client as the body would and carries its
// scalars. The host judges the sender and the ATV, runs the game's own unscrew or writes the fields
// the body writes, and the part's birth and the presence in AtvState cross as the host's; a refused
// fit or stow is refunded as a prop beside the sender, and a taken spare is born there too.
// The battery is atv_gate's (AtvRequest). docs/vehicles.md carries the lane.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct AtvPartIntentPayload;
}  // namespace coop::net

namespace coop::atv_part_intent {

// Idempotent: caches `session` and registers the four script-gate watches on ATV_C, once.
void Install(coop::net::Session* session);

// Once a tick on the game thread, beside atv_gate's: when every registered watch has settled, says each one that
// settled dead. A no-op after that.
void SettleWatches();

// HOST: a client's AtvPartIntent (router: event_dispatch_atv_part.cpp). Game thread.
void OnIntent(coop::net::Session& session, const coop::net::AtvPartIntentPayload& payload, uint8_t senderSlot);

// Counters for the session-end line and the drill.
struct Stats {
    uint64_t asked;     // CLIENT: bodies cancelled here and asked of the host
    uint64_t ran;       // HOST: asked changes it ran
    uint64_t refused;   // HOST: asked changes it refused
    uint64_t refunded;  // HOST: refused fits and stows it gave back as a prop
};
Stats GetStats();

// Session end: the counters are said and cleared, the senders' budgets filled.
void OnDisconnect();

}  // namespace coop::atv_part_intent
