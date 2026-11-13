// coop/interactables/atv_host_judge.h -- the host's judge of a client's ATV requests: a refuel, a repair, a module fitted
// or taken off, and the syncer's horn. A client's item act runs on its own copy, where the item is spent; what the act
// does to the ATV is the host's, asked here. Per sender, requests wait in a FIFO of 8 and are served at {4, 4} a second:
// a request whose sender's world is not ready on the host, or whose hand state the host still owes (hand_item's pacing),
// is parked at the head until the host's tick finds it judgeable, never refused for the lag. Judged on the game thread:
// the sender is live and within 800 cm of the ATV or seated in it; a refuel needs a gas can in its hand and room in the
// tank, a repair a toolbox (held or last held, since it spends itself) and health under 100, a fit an ATV module and one
// the host copy does not hold, a take-off one it does. Applied by writing the host copy, as the verbs' own bodies do
// (fuelUp, toolboxFix, the module arrays), published through atv_host_sync, and EVERY request is answered to its sender
// alone: the verdict, the host copy's resulting state and its sequence. A fit refused as a duplicate, after the hand
// judge passed, is refunded as the module's prop at the ATV. The horn is judged on arrival, not queued: only the ATV's
// syncer may press it, at {8, 8} a second, a release never refused; the host sounds it and sends the edge to the others.
// Game thread. Overview: docs/vehicles.md.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct AtvHostPayload;
}  // namespace coop::net

namespace coop::atv_host_judge {

// HOST: a client's AtvRequest, from sender slot `senderSlot`. A horn is judged here; every other op waits in the sender's
// queue for Tick. A request with no readable key is the sender's own bug: counted, not answered.
void OnRequest(int senderSlot, const coop::net::AtvHostPayload& p);

// HOST, per tick: each sender's queue head, at its budget; the refusal tally's window.
void Tick(coop::net::Session& s);

// HOST: the ATV's horn is down by a client's press and its syncer's seat ended, the syncer went silent or the table
// cleared: the host copy's horn is released and the release goes to every client. A no-op when no press is open.
void ReleaseHorn(void* atv);

// [dev] HOST, the ATV drill's fixture: a request for `key` is held in its queue until the host has published a state of its
// own for that ATV after this call, so that state reaches the sender while its open-request gate is shut.
void HoldRequestsForDrill(const std::wstring& key);

// A world change: the horns held down are forgotten, with the copies that sounded them.
void OnWorldChange();

// The session ended.
void OnDisconnect();

}  // namespace coop::atv_host_judge
