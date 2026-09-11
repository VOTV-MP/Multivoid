// ue_wrap/devices/serverbox.h -- the signal server (AserverBox_C) engine wrapper: the box list the
// gamemode owns, the two verbs that move a disc in and out of it, and the break state the farm
// runs on.
//
// What the wrapper reaches:
//   - mainGamemode_C.servers, whose INDEX is the identity every server lane addresses a box by:
//     the level places the boxes, so both peers build the same ordering
//   - the box's own label, the FString the sign above it renders
//   - the two verbs a player's interaction dispatches: pocessFloppy, which casts the held object
//     and refuses a zip disc before inserting it, and ejectFloppy, which empties the slot and
//     respawns the disc when the out-timeline finishes
//   - the break state: the box's own IsBroken, the notify-free check() it re-skins from, and the
//     three totals the gamemode keeps for the whole farm
//
// Resolution comes in three independent groups -- the verbs, the box list, and the break state --
// each on its own backoff and its own latch. A member one group cannot find must not cost the
// others theirs: a lane that only wants the boxes is not a lane that wants pocessFloppy.
//
// No network logic, no coop state (principle 7). Game thread only.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::serverbox {

// Resolve the box class, its label and the two verbs. Retried on a backoff until the world is up;
// true once every member is in hand. The box LIST resolves separately, so a renamed member cannot
// take the list away from the lane that only wants the boxes. The SLOT the box holds a disc in is
// the same one every such device has, and lives in ue_wrap/devices/floppy_slot.
bool EnsureResolved();

// The gamemode's server list, in its own order. Returns the number appended, 0 before the world
// has a gamemode. The index into `out` is the cross-peer box identity. Resolves what it needs
// itself; EnsureResolved is not a precondition.
size_t ReadServers(std::vector<void*>& out);

// The box's rendered label.
std::wstring ReadName(void* box);

// The insert verb the interaction dispatches with whatever the player holds: it casts to a disc,
// refuses a zip one, and only then reaches the insert. Calling the inner insert instead would skip
// both of those and enter through a door no player has.
bool CallProcessFloppy(void* box, void* discActor);

// The eject verb. It returns before the disc exists: the slot is cleared here and the actor is
// spawned when the box's out-timeline finishes.
bool CallEjectFloppy(void* box);

// ---- break state -----------------------------------------------------------------------------

// The three totals the gamemode keeps for the whole farm: how many boxes are down, and the two
// efficiencies the SAT console's sv.*/tw.* queries read. They live on the gamemode rather than on
// a box because no single box owns them, and they are the farm's half of the same state the
// per-box IsBroken is the other half of.
struct Aggregates {
    int32_t brokenServers      = 0;    // mainGamemode_C.brokenServers
    float   efficiencyCalc     = 0.f;  // serverEfficiency_calc
    float   efficiencyDownload = 0.f;  // serverEfficiency_downl
};

// Resolve the break group: the three gamemode totals, the box's IsBroken bool with its real bit,
// and check(). Retried on a backoff; after a capped number of passes with the classes already in
// hand it latches OFF with one warning, since an offset that is not there on a loaded class will
// not appear later and a guessed one would write into whatever now lives at it.
bool EnsureBreakResolved();

// The box's own break flag, raw -- the field the break and fix verbs set and check() re-skins
// from. False for an unresolved group or a null box, which is also what an unbroken box reads.
bool ReadIsBroken(void* box);

// Set the break flag and let the box re-skin itself from it. check() is notify-free -- no
// delegate, no minigame, unlike the verbs that normally reach this state -- and re-skins from
// IsBroken, the box's own `resisnant` (the blueprint's spelling), active and calc: while the glow
// effect is recently rendered it retargets only that effect's particle, otherwise it sets the body
// material, which is the OFF instance unless active && calc. So this pair applies a break
// authoritatively without firing the notice the verbs fire. Returns false if unresolved.
bool ApplyBreak(void* box, bool broken);

bool ReadAggregates(Aggregates& out);
bool WriteAggregates(const Aggregates& in);

// Bumps whenever a DIFFERENT gamemode object is resolved -- a world load or a save reload mints
// one. A poll's baseline is anchored to a generation: a baseline carried across a bump describes
// a gamemode that no longer exists, and the next reading off the new one would read as an edge.
uint32_t GamemodeGeneration();

}  // namespace ue_wrap::serverbox
