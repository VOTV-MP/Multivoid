// coop/interactables/atv_host_judge.h -- the host's judge of a client's ATV requests: a refuel, a repair, a module fitted
// or taken off. A client's item act runs on its own copy, where the item is spent; what the act
// does to the ATV is the host's, asked here. Per sender, requests wait in a FIFO of 8 and are served at {4, 4} a second:
// one whose sender's world is not ready on the host, or whose hand state the host still owes (hand_item's pacing), is
// parked at the head, never refused for the lag, and released by the slot's world-ready edge (OnSlotReady) or its hand
// state applied (hand_item's subscriber); Tick serves no parked slot. Judged on the game thread: the sender is live and
// within 800 cm of the ATV or seated in it; a refuel needs a gas can in its hand and room in the tank (else `full`), a
// repair a toolbox last held (10 uses of one hold) and health under 100 (else `full`), a fit a module of the class its
// hand last held, once per item, and one the host copy does not hold (else `duplicate`), a take-off one it does. Applied
// by writing the host copy as the verbs' own bodies do, published through atv_host_sync, and EVERY request is answered
// to its sender alone: the verdict, the host copy's state (flagged absent when there is none) and its sequence. A fit
// refused as a duplicate is refunded as the module's prop at the ATV. A sync (op kSync) is answered and applies nothing.
// Game thread. Overview: docs/vehicles.md.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct AtvHostPayload;
}  // namespace coop::net

namespace coop::atv_host_judge {

// HOST: a client's AtvRequest, from sender slot `senderSlot`: it waits in the sender's queue for Tick. A request with no
// readable key is the sender's own bug: counted, not answered.
void OnRequest(int senderSlot, const coop::net::AtvHostPayload& p);

// Subscribe to the hand lane's applied states (hand_item::SubscribeHandApplied). Idempotent; called by atv_host_sync::Install.
void Install();

// HOST, per tick: each unparked sender's queue head, at its budget; the refusal tally's window.
void Tick(coop::net::Session& s);

// HOST: `slot` became world-ready (the connect replay's edge): its parked requests are served.
void OnSlotReady(int slot);

// HOST: a state of `key`'s ATV was published (atv_host_sync::PublishIfChanged). Ends the drill fixture's park.
void OnStatePublished(const std::wstring& key);

// [dev] HOST, the ATV drill's fixture: a request for `key` is held in its queue until the host has published a state of its
// own for that ATV after this call, so that state reaches the sender while its open-request gate is shut.
void HoldRequestsForDrill(const std::wstring& key);

// The session ended.
void OnDisconnect();

}  // namespace coop::atv_host_judge
