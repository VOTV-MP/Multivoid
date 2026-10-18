// coop/player/hit_judge.h -- the host's judge of a swing a peer says it made, for any lane that turns such a claim into
// damage (a client's hit on a creature, a player's hit on a player).
//
// A held tool's swing is one line trace from the player's arm as long as the held item's list_weapons row says, and
// deals the row's damage (ue_wrap/world/weapon_catalog). The judge asks, in order, whether the slot's puppet holds
// an item that swings, whether the struck point lies within that trace's reach of the eye the host holds for the slot
// (plus the pose's latency), whether nothing static stands between the eye and the point (a block within the slack of
// the point is not cover), and whether the point lies on the target; and it cuts the claimed damage to what the held
// item can deal. It has no notion of what the target is. Source takes the shot's origin from the server's own eye and never from the client
// (reference/source-sdk-2013/src/game/shared/baseplayer_shared.cpp:817); MTA bounds a claimed origin to its copy of the
// player (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CBulletsyncPacket.cpp:167-191). The judge does
// both, because the host has the world. Game thread.

#pragma once

#include "ue_wrap/core/types.h"

namespace coop::hit_judge {

// How far past the held item's reach a struck point may lie from the eye, for the pose the host holds trailing the
// player's own, how far a struck point may lie from the target's location (a creature's body is wider than its
// origin), and how far short of the struck point a blocker may stand before the line counts as blocked (the remover's
// own slack, kBlockedSlackCm in coop/props/remover_request.cpp: a point on a surface sits in that surface's own trace).
inline constexpr float kReachSlackCm   = 100.f;
inline constexpr float kTargetRadiusCm = 300.f;
inline constexpr float kBlockedSlackCm = 25.f;

// Judge a swing `slot` claims to have landed on `target` at `hitLoc`, from the eye `eye` the host captured for it.
// True on a pass, with `outDamage` the claim cut to the held item's most (`min(claimedDamage, maxDamage)`). False
// with `outReason` a static string naming the check that stopped it: "no swinging item", "weapon table unreadable",
// "out of reach", "sight unreadable", "no sight", "target unreadable" or "not on target". A NaN fails every distance
// check. Game thread.
bool JudgeHit(int slot, const ue_wrap::FVector& eye, const ue_wrap::FVector& hitLoc, void* target,
              float claimedDamage, float& outDamage, const char*& outReason);

}  // namespace coop::hit_judge
