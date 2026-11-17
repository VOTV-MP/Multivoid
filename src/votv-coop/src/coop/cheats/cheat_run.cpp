// coop/cheats/cheat_run.cpp -- see coop/cheats/cheat_run.h.

#include "coop/cheats/cheat_run.h"

#include "coop/cheats/item_spawn.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/event_trigger.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/world/economy.h"

#include <cctype>
#include <cstring>
#include <string_view>

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

// The props a player may not spawn: the ones that explode. The game has no family to test: each is a direct child of
// prop_C (prop_drive_explosive of prop_drive_C), and no function they share is called to blow them. The list is the
// props of the game's decompiled Blueprints that exist to detonate: each spawns the game's explosion actor as its own
// act (a search for objects/misc/explosion*) and is a bomb, grenade, mine, rocket or the like by name. Left out on
// purpose: a prop that explodes only when misused (a battery, a lemonade, a vending machine), the gas can (fuel), the
// poop and soap grenades (they react to a blast), and the sticky launcher (it fires the bomb). Lower case, as the
// item catalog matches names.
constexpr std::string_view kExplosiveItems[] = {
    "bombbarrel", "bombbottle", "bombbox", "pipebomb", "stickybomb", "drive_explosive", "grenade", "grenade_a",
    "landmine", "rocket", "saltrocket", "explotano", "explodvehicle", "wirecomponent_dynamite", "flaregren",
    "crystal_explode", "comet",
};

bool IsExplosive(const std::string& name) {
    std::string lower = name;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const std::string_view e : kExplosiveItems)
        if (lower == e) return true;
    return false;
}

// The class a prop would spawn as is judged by its name too: prop_<name>_C of a list name. A class named otherwise is
// tested whole.
bool ClassOpenToPlayers(const std::wstring& className) {
    std::string name;
    for (const wchar_t c : className) name.push_back(c > 0 && c < 0x80 ? static_cast<char>(c) : '?');
    constexpr std::string_view kPre = "prop_", kSuf = "_C";
    if (name.size() > kPre.size() + kSuf.size() && name.compare(0, kPre.size(), kPre) == 0 &&
        name.compare(name.size() - kSuf.size(), kSuf.size(), kSuf) == 0)
        name = name.substr(kPre.size(), name.size() - kPre.size() - kSuf.size());
    return !IsExplosive(name);
}

// The red arm: a player's request is judged open without the check (dev row cheat_open_check_off).
bool OpenCheckOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::cheat_open_check_off);
    return off;
}

Outcome RunSpawn(const Request& r, int senderSlot) {
    const int maxCount = senderSlot == 0 ? kHostMaxCount : coop::net::kCheatMaxCount;
    if (r.count < 1 || r.count > maxCount) return Done(CheatReason::BadRequest);
    if (senderSlot != 0 && !OpenCheckOff()) {
        // By its name, and by the class the name spawns as: a list row can name another prop's class.
        std::wstring spawnClass;
        if (!ItemOpenToPlayers(r.name) ||
            (coop::cheats::item_spawn::SpawnClassName(r.name, spawnClass) && !ClassOpenToPlayers(spawnClass)))
            return Done(CheatReason::Refused);
    }
    // Where: in front of the asker, or at a player who is in the session. The asker's own body is its slot.
    const bool inFront = r.target == coop::net::kCheatInFront;
    const int slot = inFront ? senderSlot : r.target;
    int spawned = 0;
    switch (coop::cheats::item_spawn::Spawn(r.name, r.count, slot, inFront, &spawned)) {
        case coop::cheats::item_spawn::Result::Ok: return Done(CheatReason::Ok, spawned);
        case coop::cheats::item_spawn::Result::NotReady: return Done(CheatReason::NotReady);
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
    // The read, the clamp and the add happen here, in one game-thread step: a burst of removals cannot each read the
    // same balance. A removal stops at zero (the balance is never taken below what the game's own shop could leave it);
    // one that would change nothing is Failed, not a done.
    int32_t now = 0;
    if (!ue_wrap::economy::ReadPoints(&now)) return Done(CheatReason::NotReady);
    if (amount < 0 && now + amount < 0) amount = -now;
    if (amount == 0) return Done(CheatReason::Failed);
    return Done(ue_wrap::economy::AddPoints(amount) ? CheatReason::Ok : CheatReason::Failed);
}

Outcome RunEvent(const Request& r, int senderSlot) {
    const ET::EventInfo* ev = FindEvent(r.name);
    if (!ev) return Done(CheatReason::UnknownEvent);
    if (senderSlot != 0 && !OpenCheckOff() && !OpenToPlayers(*ev)) return Done(CheatReason::Refused);
    return Done(ET::Trigger(*ev) ? CheatReason::Ok : CheatReason::Failed);
}

}  // namespace

Outcome Run(const Request& r, int senderSlot) {
    UE_ASSERT_GAME_THREAD("cheat_run::Run");
    switch (r.op) {
        case CheatOp::SpawnItem: return RunSpawn(r, senderSlot);
        case CheatOp::Points:    return RunPoints(r, senderSlot);
        case CheatOp::Event:     return RunEvent(r, senderSlot);
    }
    return Done(CheatReason::BadRequest);
}

bool OpenToPlayers(const ET::EventInfo& ev) {
    return ev.risk == ET::Risk::Safe && ev.cat != ET::Category::Story;
}

bool ItemOpenToPlayers(const std::string& name) { return !IsExplosive(name); }

}  // namespace coop::cheats
