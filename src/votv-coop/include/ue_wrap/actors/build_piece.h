// ue_wrap/actors/build_piece.h -- what a player builds that stays: a nail and a custom wall in its three stages.
//
// A nail (nail_C and its five subclasses, all actor_save_C) is spawned where a nail gun's projectile (nailProjectile_C)
// lands or where a hand nail (prop_nail_C) is pressed: nailNail or attach traces along it, keeps the actor it went into
// (actor_a) and the one behind (actor_b, or the world), attaches to actor_a and ties the two with its PhysicsConstraint.
// Its record (getData) carries both ends' keys (names[0]), the pins (vectors[0], rotators[0]) and broken / static / nailed
// (bools[0]); the save's load runs loadData, then processKeys, which resolves the keys and a frame later runs `aea`, the
// attach and the tie. Its ReceiveDestroyed drops a `nailDrop` item.
//
// A wall builder (prop_wallbuilder_C) places a customWall_unfinished_C with a shape and a material. Bricks of the wall's
// material (or a dirt or concrete clump) that touch its `Overlap` add to `bricks` and are destroyed; at `requirement` its
// buildWall makes the finished wall, a customWall_C, or a customWall_wetConcrete_C that its tick dries into one. A finished
// wall's Damage takes `health` down and, below zero, sets wasBroken and destroys it, and its ReceiveDestroyed drops its
// material. Every stage is an actor_save_C whose loadData runs its own Init. Game thread.

#pragma once

#include "ue_wrap/actors/save_record.h"

#include <string>
#include <vector>

namespace ue_wrap::build_piece {

enum class Kind { None, Nail, Unfinished, Wall, WetWall };

// The kind of a class this file names, by its leaf name ("nail_long_C"). None for any other.
Kind KindOfClass(const std::wstring& className);

// The kind of a live actor.
Kind KindOf(void* actor);

// Every live piece of this world, of every class a loaded class lists.
void List(std::vector<void*>& out);

// The piece's Aactor_save_C::Key as it stands, without minting one. "" or "None" when it has none.
std::wstring KeyOf(void* piece);

// The piece's own record (getData).
bool Capture(void* piece, save_record::SaveRecord& out);

// A piece of the record's class at its transform and scale, loading the class if nothing has. Null on failure.
void* Spawn(const save_record::SaveRecord& r);

// Hand `r` to the piece (loadData, which runs a wall's Init). A nail then takes `endA` and `endB`, the actors its record's
// keys name in this world (NailKeys; either may be null), and its `aea` attaches and ties it, as the save's load does a
// frame after processKeys; a broken record's `aea` only attaches it, so a tie the nail still has is broken first.
bool Apply(void* piece, const save_record::SaveRecord& r, void* endA, void* endB,
           save_record::ClassResolver resolve = nullptr);

// A nail's two end keys (names[0]); false when the record does not carry them.
bool NailKeys(const save_record::SaveRecord& r, std::wstring& a, std::wstring& b);

// A nail's live ends as it holds them, or null.
void* NailEndA(void* nail);
void* NailEndB(void* nail);

// A copy that runs nothing of its own: a nail drops no item when it goes and, while `tieHeld`, no force breaks its tie;
// an unfinished wall counts no bricks, a wet wall does not dry. Called on every poll, as an unfinished wall's own upd
// turns its overlap back on.
void Park(void* piece, bool tieHeld = true);

// A finished wall's health and its maximum, both times `k`: the cracks read their ratio, and Damage on this copy never
// takes it below zero.
bool ScaleWallHealth(void* wall, float k);

// A finished or wet wall's health as it stands.
bool WallHealth(void* wall, float& out);

// A hit on the wall through its own Damage, at its location: below zero it breaks, drops its material and goes.
bool HitWall(void* wall, float damage);

// A repair as the wall's own fix makes one, without its sound and hint for the local player: health up by `amount`,
// never past its maximum, and the cracks drawn again.
bool RepairWall(void* wall, float amount);

// Whether a nail's tie has broken (its constraint's broken event sets it).
bool NailBroken(void* nail);

// Whether force can break a nail's tie (its constraint's profile, linear or angular); true when that cannot be read.
bool NailBreakable(void* nail);

// A nail's tie that no force breaks: its constraint's SetLinearBreakable and SetAngularBreakable, both off, the
// thresholds kept. A pin (nail_pin_C) destroys itself when its tie breaks, so on a copy that would read as the player
// taking it away.
bool HoldNail(void* nail);

// A held tie let go: breakable again as its class's nails are, at the thresholds it kept.
bool FreeNail(void* nail);

// The nail's tie broken as force breaks it: the constraint ended, then the nail's own broken event (it sets broken,
// attaches to its first end and runs detached, which destroys a pin). False when it is broken already.
bool SnapNail(void* nail);

// A finished or wet wall's health set, never past its maximum, and its cracks drawn again.
bool SetWallHealth(void* wall, float health);

// Destroy the piece leaving nothing behind: no nail drop, no wall's material.
void DestroyQuiet(void* piece);

}  // namespace ue_wrap::build_piece
