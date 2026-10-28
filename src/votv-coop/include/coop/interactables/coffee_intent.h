// coop/interactables/coffee_intent.h -- a client's press of the coffee vending machine or the coffee maker
// runs on the host.
//
// Both devices take their brew or their coin at actionOptionIndex(player, 4). On a client the script-body gate
// refuses that body on either device and sends the press here; the host resolves the device by its prop key,
// refuses a sender whose player is not alive, judges its reach and the device's exact class, and runs the game's own
// action on its copy with the sender's puppet as the player, where the device's own gates decide it once (the
// vending machine's processing flag, the maker's cup, coffee and power). The debit crosses by balance_sync, the cup by
// PropSpawn on the Default lane, the maker's state by coffee_state. Every other action of either device runs natively. MTA
// precedent: a client's use of a shared element is a request the server checks and runs
// (reference/mtasa-blue/Server/mods/deathmatch/logic/
// CGame.cpp:3018, Packet_Vehicle_InOut), as the door lane's press.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct CoffeeActionIntentPayload;
}  // namespace coop::net

namespace coop::coffee_intent {

// Registers the gate's two name watches. The per-tick retry pump (coop/interactables/verb_lanes), so a watch
// the gate refused is registered again on a later tick. Game thread.
void Install(coop::net::Session* session);

// Both roles: settles the watches and says once when both are live. Host: runs the queued presses in order,
// each sender at a bounded rate from its own IntentQueue; a sender whose first pose has not arrived has no body
// to measure its reach from, and its presses wait for it. Game thread, once per pump tick.
void Tick(coop::net::Session& session);

// Host: a client's press from the wire, its format already checked by the dispatcher. Queued, and run by Tick;
// a full queue refuses it. Game thread.
void OnAction(coop::net::Session& session, const coop::net::CoffeeActionIntentPayload& payload,
              uint8_t senderSlot);

// The dispatcher refused a press of this kind, for this reason: counted, not logged, so a flood costs no line. A
// payload that is not the struct's size, or a key longer than the wire's.
enum class Drop : uint8_t { Size, Key };
void CountDropped(Drop why);

// The session ended: the counters, with one summary line. A sender's queue, its rate and its said-once latches
// live in its per-slot state and go with its occupant.
void OnDisconnect();

// What the hostile drill reads of the host's lane. Every press past the dispatcher is one claim, and each claim
// ends in exactly one of ran and the refusals; the dispatcher's drops are not claims. A read.
struct DrillView {
    uint64_t claims = 0, ran = 0;
    uint64_t noDevice = 0, wrongClass = 0, outOfReach = 0, notRun = 0, queueFull = 0, senderDead = 0;
    uint64_t dropSize = 0, dropKey = 0;
};
DrillView ReadForDrill();

}  // namespace coop::coffee_intent
