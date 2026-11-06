// coop/interactables/atv_host_sync.h -- what of an ATV is the host's beyond its rig, which stays with its driver
// (atv_sync.h): its fuel and health, its modules, its lights, brake and turbo, and its horn. A client's own act runs on
// its copy and is sent to the host, which does it on its own copy and sends every ATV's state back:
//   - a refuel or repair is the rise its copy shows, a gain the host adds within reach; a client driver only lowers them;
//   - a module fitted or taken off is written on the host's Modules, a refused fit refunded with its prop at the ATV;
//   - the seated driver's lights, brake and turbo, and its horn, which the host and every other peer play.
// The state goes when any of it changes, every 2 s and to a joiner; one sent before the host heard of a client's own act
// waits 1.5 s on that client. Game thread. Overview: docs/vehicles.md.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct AtvHostPayload;
}  // namespace coop::net

namespace coop::atv_host_sync {

// Store the session. Idempotent; called every load tick.
void Install(coop::net::Session* session);

// The poll and the inboxes, on either role. Game thread.
void Tick();

// A client's receiver: the host's state of an ATV, or its horn.
void OnState(const coop::net::AtvHostPayload& p);

// The host's receiver: a client's gain, module change, controls or horn.
void OnRequest(int senderSlot, const coop::net::AtvHostPayload& p);

// Every ATV again for a joiner.
void QueueConnectBroadcastForSlot(int slot);

// The session ended: everything is forgotten.
void OnDisconnect();

// From the condition lane, before a block writes `actor`'s fuel and health. HOST taking a client driver's block: the
// values become the lower of the host's and the driver's. CLIENT taking the host's block: a rise its own copy shows past
// the host's last values is sent as a gain first, and while a gain of this client's is unanswered its own values stand.
void FilterVitals(void* actor, const std::wstring& key, bool hostTakingClientBlock, float& fuel, float& health);

// [dev] The horns this peer played for another peer's press.
uint32_t HornsPlayed();

}  // namespace coop::atv_host_sync
