// coop/player/hit_judge.h -- the host's judge of a swing a peer says it made, for any lane that turns such a claim into
// damage (a client's hit on a creature; a player's hit on a player is the next caller).
//
// A held tool's swing is one line trace from the player's arm as long as the held item's list_weapons row says, and
// deals the row's damage (ue_wrap/world/weapon_catalog). The judge asks whether the slot's puppet holds an item that
// swings and has reach data and is alive (its streamed health above 0); whether the struck point lies within that
// reach of the eye the host holds for the slot (plus the pose's latency) and in front of the swinger's aim; whether
// nothing static stands between the eye and the
// point (a block within the slack of the point is not cover); and whether the point lies on the target. It cuts the
// claimed damage to what the held item can deal and has no notion of what the target is.
// Source takes the swing's origin from the server's own eye, never the client's
// (reference/source-sdk-2013/src/game/shared/baseplayer_shared.cpp:817); MTA bounds a claimed origin to its copy of
// the player (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CBulletsyncPacket.cpp:167-191). This takes the
// origin from the host's eye (Source) and bounds the claimed struck point against it (the MTA shape). Game thread.

#pragma once

#include "ue_wrap/core/types.h"

namespace coop::hit_judge {

// Not judged. A creature is not lag compensated: Source rewinds PLAYER targets only (the rewind is
// reference/source-sdk-2013/src/game/server/player_lagcompensation.cpp:345-437, its loop over players at :418-436,
// sized by the measured latency at :382-399), and the flat slacks below stand in for it, as MTA's flat origin bound
// does; neither scales with the measured round trip. The tool is not on the wire: it is read from the slot's HandItem
// stream when the intent is served, so an item equipped an instant before a swing can read as the previous one. MTA
// carries the weapon in its bulletsync packet and checks it against the server's copy
// (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CBulletsyncPacket.cpp:194-195); ours would carry the
// tool the same way when friendly fire reopens the message. Friendly fire reuses this judge and revisits both: a
// player target's rewind and a body-sized radius, and the attacker and tool the intent does not carry.
//
// How far past the held item's reach a struck point may lie from the eye, for the pose the host holds trailing the
// player's own, how far a struck point may lie from the target's location (a creature's body is wider than its
// origin), and how far short of the struck point a blocker may stand before the line counts as blocked (the remover's
// own slack, kBlockedSlackCm in coop/props/remover_request.cpp: a point on a surface sits in that surface's own trace).
inline constexpr float kReachSlackCm   = 100.f;
inline constexpr float kTargetRadiusCm = 300.f;
inline constexpr float kBlockedSlackCm = 25.f;

// The least cosine, in 3-D, between the swinger's aim (the puppet's newest streamed aim, captured with the eye when the
// intent arrived) and the direction from the eye to the struck point: cos 60 degrees. Source's "sort of facing" bound
// of a melee hit is 0.70721 (EyeVectors dotted with the direction to the target entity's origin,
// reference/source-sdk-2013/src/game/shared/hl2mp/weapon_hl2mpbasebasebludgeon.cpp:298, :310-331), there on a server
// that owns the aim and only in the hull-trace fallback after the line trace missed. Ours is always on, since the host
// has no trace of its own to fall back from, takes the struck point, not the entity's origin, and is wider: the aim
// reaches the host after the stream's latency, and the swinger's client has already shown the hit, so a bound as tight
// as the server-authoritative one would refuse swings made on screen. The reach check carries a slack for the same lag.
inline constexpr float kFacingCosine = 0.5f;

// A struck point this close to the eye (cm) has no direction to test and passes the facing bound.
inline constexpr float kFacingMinCm = 1.f;

// The cosine, in 3-D, between the unit vector `aim` and the direction from `eye` to `hitLoc`: what JudgeHit bounds by
// kFacingCosine, exposed so a caller can log the margin. 1 when `hitLoc` lies within kFacingMinCm of `eye`. NaN when
// an input is, so a caller written `!(c >= bound)` refuses it. Any thread.
float FacingCosine(const ue_wrap::FVector& aim, const ue_wrap::FVector& eye, const ue_wrap::FVector& hitLoc);

// Judge a swing `slot` claims to have landed on `target` at `hitLoc`, from the eye `eye` and the unit aim vector `aim`
// the host captured for it together. True on a pass, with `outDamage` the claim cut to the held item's most
// (`min(claimedDamage, maxDamage)`). False with `outReason` a static string naming the check that stopped it:
// "weapon table unreadable", "no swinging item", "no reach data", "puppet unreadable", "dead", "out of reach",
// "not facing", "sight unreadable", "no sight", "target unreadable" or "not on target". A NaN fails every distance and
// facing check.
// Game thread.
bool JudgeHit(int slot, const ue_wrap::FVector& eye, const ue_wrap::FVector& aim, const ue_wrap::FVector& hitLoc,
              void* target, float claimedDamage, float& outDamage, const char*& outReason);

}  // namespace coop::hit_judge
