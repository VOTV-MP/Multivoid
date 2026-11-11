// coop/cheats/cheat_run.cpp -- see coop/cheats/cheat_run.h.

#include "coop/cheats/cheat_run.h"

#include "coop/cheats/item_spawn.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/add_points.h"
#include "coop/dev/event_trigger.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/world/economy.h"

#include <cstring>

namespace coop::cheats {
namespace {

using coop::net::CheatOp;
using coop::net::CheatReason;
namespace ET = coop::dev::event_trigger;

// The host's own act may spawn more at once than a player's request, still bounded: a prop each is a physics body.
constexpr int kHostMaxCount = 50;

const ET::EventInfo* FindEvent(const std::string& name) {
    for (const ET::EventInfo& ev : ET::Events())
        if (name == ev.name) return &ev;
    return nullptr;
}

Outcome Done(CheatReason r, int done = 0) { return Outcome{r, done}; }

Outcome RunSpawn(const Request& r, int senderSlot) {
    const int maxCount = senderSlot == 0 ? kHostMaxCount : coop::net::kCheatMaxCount;
    if (r.count < 1 || r.count > maxCount) return Done(CheatReason::BadRequest);
    // Where: in front of the asker, or at a player who is in the session. The asker's own body is its slot.
    const bool inFront = r.target == coop::net::kCheatInFront;
    const int slot = inFront ? senderSlot : r.target;
    int spawned = 0;
    switch (coop::cheats::item_spawn::Spawn(r.name, r.count, slot, inFront, &spawned)) {
        case coop::cheats::item_spawn::Result::Ok: return Done(CheatReason::Ok, spawned);
        case coop::cheats::item_spawn::Result::UnknownItem: return Done(CheatReason::UnknownItem);
        case coop::cheats::item_spawn::Result::NoPlace: return Done(inFront ? CheatReason::NotReady : CheatReason::NoTarget);
        case coop::cheats::item_spawn::Result::Failed: return Done(CheatReason::Failed);
    }
    return Done(CheatReason::Failed);
}

Outcome RunPoints(const Request& r, int senderSlot) {
    if (r.amount == 0) return Done(CheatReason::BadRequest);
    if (senderSlot != 0 && (r.amount > coop::net::kCheatMaxPoints || r.amount < -coop::net::kCheatMaxPoints))
        return Done(CheatReason::BadRequest);
    int amount = r.amount;
    // A removal stops at zero: the balance is never taken below what the game's own shop could leave it.
    int32_t now = 0;
    if (!ue_wrap::economy::ReadPoints(&now)) return Done(CheatReason::NotReady);
    if (amount < 0 && now + amount < 0) amount = -now;
    if (amount != 0) coop::dev::add_points::GivePoints(amount);
    return Done(CheatReason::Ok);
}

Outcome RunEvent(const Request& r, int senderSlot) {
    const ET::EventInfo* ev = FindEvent(r.name);
    if (!ev) return Done(CheatReason::UnknownEvent);
    if (senderSlot != 0 && ev->risk == ET::Risk::Dangerous) return Done(CheatReason::Refused);
    return Done(ET::Trigger(*ev) ? CheatReason::Ok : CheatReason::Failed);
}

}  // namespace

Outcome Run(const Request& r, int senderSlot) {
    UE_ASSERT_GAME_THREAD("cheat_run::Run");
    if (senderSlot != 0 && !coop::config::ResolveFlag(coop::config_registry::rows::net_allow_player_cheats))
        return Done(CheatReason::Off);
    switch (r.op) {
        case CheatOp::SpawnItem: return RunSpawn(r, senderSlot);
        case CheatOp::Points:    return RunPoints(r, senderSlot);
        case CheatOp::Event:     return RunEvent(r, senderSlot);
    }
    return Done(CheatReason::BadRequest);
}

bool EventOpenToPlayers(const char* eventName) {
    if (!eventName) return false;
    const ET::EventInfo* ev = FindEvent(eventName);
    return ev && ev->risk != ET::Risk::Dangerous;
}

}  // namespace coop::cheats
