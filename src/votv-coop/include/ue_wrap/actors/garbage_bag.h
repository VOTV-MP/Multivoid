// ue_wrap/actors/garbage_bag.h -- the bagging family: the two tools a player packs a trash pile
// with, and the filled bag the pack produces.
//
// Its own file rather than a corner of ue_wrap/actors/prop: bagging is one feature, and prop.cpp
// is at the size where the next feature to land in it would be looking for a home anyway.
//
// A folded bag (prop_garbBagFold_C) is spent whole on one pack; a roll (prop_garbBagRoll_C) holds
// a count in `bags` and is spent one at a time; both spawn prop_garbageBag_C at the target's
// transform with the target's chipType. The gameplay lane that carries a client's pack across is
// coop/props/pack_trash_intent.

#pragma once

#include <cstdint>

namespace ue_wrap::garbage_bag {

// The two tools' classes, for a consumer that gates a hot callback on them: produce the class in
// an installer, on a throttle, and COMPARE in the callback. Null until the class is loaded, and a
// FindClass miss is not memoised, so a caller that asks per press pays a full object walk per press
// in every world that holds no bag. Game thread.
void* FoldClass();
void* RollClass();

// The same test, for a call site that runs once per press and not per dispatch of a shared verb.
// Each answers for its class or a subclass. Game thread.
bool IsFold(void* obj);
bool IsRoll(void* obj);

// The filled bag's class, for a spawn, loaded first when this process has not (actors/item_spawn); null when it does not
// load. Game thread.
void* FilledClass();

// The roll's remaining count, the number its own hint prints. A peer that spends a bag writes this
// field, and the roll's own body reads the same one. False for anything but a roll. Game thread.
bool ReadRollCount(void* roll, int32_t& bags);
bool WriteRollCount(void* roll, int32_t bags);

}  // namespace ue_wrap::garbage_bag
