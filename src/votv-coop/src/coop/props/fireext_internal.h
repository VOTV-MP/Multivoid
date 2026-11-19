// coop/props/fireext_internal.h -- what fireext_sync.cpp and fireext_runaway_ask.cpp share. Private to coop/props.

#pragma once

#include "coop/net/intent_bucket.h"

#include <cstddef>
#include <cstdint>

namespace coop::net {
class Session;
struct FireExtPayload;
}  // namespace coop::net

namespace coop::fireext_sync::detail {

// What the host decided on a runaway ask, in the order the tally keeps them.
enum class Reason : uint8_t {
    Applied, Budget, NotReady, NoRow, WrongType, NoBody, OutOfReach, NoTarget, NotExtinguisher, HeldByOther, AlreadyActive,
    VerbFailed,  // the game's verb did not run on the host's copy
    kCount
};
constexpr size_t kReasons = static_cast<size_t>(Reason::kCount);
const char* ReasonName(Reason r);

// The host's account of one sender slot: the budgets of its two intents and the tallies printed when it leaves. One struct
// in one PerSlotState, so every part is reset with the slot's occupant.
struct SlotLedger {
    coop::net::IntentBucket spray;  // an "on" of op 2
    coop::net::IntentBucket ask;    // an op 3
    uint32_t onsTaken = 0;
    uint32_t onsDropped = 0;
    uint32_t asks[kReasons] = {};
};
SlotLedger& Ledger(int slot);

coop::net::Session* SessionPtr();
bool IsHost();
bool IsClient();

// HOST: a client's runaway ask (op 3) judged and answered with the game's verb on the host's copy.
void OnRunawayAsk(int senderSlot, const coop::net::FireExtPayload& p);

// CLIENT: the host's thrust of `eid` is shown here, so a later damage asks afresh; and every ask forgotten.
void ForgetAsk(uint32_t eid);
void ClearAsks();

// The last answers kept for the drill, cleared at disconnect.
void ClearAnswers();

}  // namespace coop::fireext_sync::detail
