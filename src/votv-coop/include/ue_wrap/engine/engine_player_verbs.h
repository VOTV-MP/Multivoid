// ue_wrap/engine/engine_player_verbs.h -- the two verbs that hurt a player pawn, called as the game's own
// Blueprints call them: `addDamage` (the int_objects interface's hit, which the pawn answers with its damage, and
// with a punch when the hit is hard and has a source) and `punch` (a knock-down: ragdoll, then the damage).
// Engine-wrapper layer (principle 7): each call marshals one UFunction call, with no gameplay, network or coop
// state. Game thread. Implementation: src/ue_wrap/engine/engine_player_verbs.cpp.

#pragma once

#include "ue_wrap/core/types.h"

namespace ue_wrap::engine {

// mainPlayer_C::addDamage(actor, damage, hit, impact, skipSetting) on `player`. `actor` is the hit's source (null
// is a hit with no source); `hit` is passed as a default hit result and `skipSetting` as false, neither of which
// the body reads. False when the player is not live, the function does not resolve, or the call faulted.
bool InvokeAddDamage(void* player, void* actor, float damage, const FVector& impact);

// mainPlayer_C::punch(fullBody, boneImp, bn1, damage, damageLocation) on `player`. `boneName` names the bone the
// punch lands on (null is the name None). False as for InvokeAddDamage.
bool InvokePunch(void* player, const FVector& fullBody, const FVector& boneImp, const wchar_t* boneName,
                 float damage, const FVector& damageLocation);

}  // namespace ue_wrap::engine
