// ue_wrap/devices/serverbox.h -- the signal server (AserverBox_C) engine wrapper: the box list the
// gamemode owns, the two verbs that move a disc in and out of one, and the break state the farm
// runs on. What the lane above it does with all three is docs/devices.md.
//
// The box list is mainGamemode_C.servers, and its INDEX is the identity every server lane
// addresses a box by: the level places the boxes, so both peers build the same ordering.
//
// Resolution comes in three independent groups -- the verbs, the box list, and the break state --
// each on its own backoff and its own latch. A member one group cannot find must not cost the
// others theirs: a lane that only wants the boxes is not a lane that wants pocessFloppy.
//
// No network logic, no coop state (principle 7). Game thread only.

#pragma once

#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

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

// ---- repair ------------------------------------------------------------------------------------------

// The repair group, on its own latch: the box's `damaged` flag (a break that came from damage; the
// minigame pays no points for it), its `minigame` type (rolled inside breakServer; the gamemode's widget
// enters with it), its fix and breakServer verbs, and the gamemode's one repair widget (serverMinigame,
// a ui_serverMinigame_C whose end(true) calls fix). Retried on a backoff, latched off with one warning.
bool EnsureRepairResolved();

struct RepairState {
    bool    damaged = false;
    int32_t minigame = 0;
};
bool ReadRepairState(void* box, RepairState& out);
bool WriteRepairState(void* box, const RepairState& in);

// The box's own fix() and breakServer(), reflected. False for a null box or an unresolved verb.
bool CallFix(void* box);
bool CallBreakServer(void* box);

// Whether `obj` is the gamemode's repair widget, the caller of a player's own fix.
bool IsRepairWidget(void* obj);

// [dev] A player's finished repair as the game runs it: the gamemode's widget, its `server` set to `box`,
// then its end(correct), whose success pays the player and calls the box's fix.
bool CallRepairEnd(void* box, bool correct);

// The repair widget's reward inputs, as its end(correct) reads them: its lol mode (the larger rewards) and the solve
// time its Tick accumulates. False when either member did not resolve. Game thread.
bool ReadRepairReward(void* widget, bool& lol, float& time);

// The repair minigame's record, saveSlot.servertimeBest (0 until a first solve): read, and written the way
// end(correct) writes it. False when the saveSlot or the member did not resolve. Game thread.
bool ReadRepairBest(float& out);
bool WriteRepairBest(float value);

// ---- server upgrades ------------------------------------------------------------------------------

// A box takes up to three physical upgrades. Its install is playerUsedOn with a held prop_serverUpg_C:
// below the cap it raises `upgrades`, destroys the held prop and runs updUpgrades (the upg1..3 meshes).
// Its take-out is actionOptionIndex with action 4 while the player looks at the take-out box: above 0
// it lowers `upgrades`, spawns a serverUpg_1 at the player and hands it over, then updUpgrades. The
// count is the box's own, saved in its data, and canBreak weights the break roll by it.
inline constexpr const wchar_t* kBoxClass    = L"serverBox_C";
inline constexpr const wchar_t* kInstallVerb = L"playerUsedOn";
inline constexpr const wchar_t* kTakeOutVerb = L"actionOptionIndex";
inline constexpr int32_t kMaxUpgrades = 3;

// The box's upgrade level, serverBox_C.upgrades, which initialServerUpgradeSpawn_C rolls at a new
// game (clamped 0..3). False for a null box or an unresolved member.
bool ReadUpgrades(void* box, int32_t& out);

// Write the level and run the box's updUpgrades, the one painter its install and take-out run. False for
// a null box or an unresolved member or verb.
bool WriteUpgrades(void* box, int32_t level);

// The box's index in the gamemode's server list, the identity every server lane uses; -1 when absent.
int32_t IndexOf(void* box);

// Whether `cls` is an upgrade prop's class: prop_serverUpg_C or one under it. The game makes
// prop_serverUpg_1_C, the class its serverUpg_1 row names, for the store, the take-out and the hand.
bool IsUpgradeClass(void* cls);

// Every live upgrade prop, of any class under prop_serverUpg_C (the index lists instances by their exact
// class). Game thread.
using UpgradeFn = void (*)(void* ctx, void* upgrade);
void ForEachUpgrade(UpgradeFn fn, void* ctx);

// An upgrade as the box's take-out spawns one: the class its serverUpg_1 row names (prop_serverUpg_1_C, whose
// defaults carry the row's name), at `at`. The row reads no name, so nothing is written after the spawn.
// Null while the class is not loaded or the spawn fails.
void* SpawnUpgradeProp(const FVector& at);

// The box's take-out component (takeUpgrade), and whether the box reads the player's look on it
// (lookatUpgrades): its getActionOptions sets it when the player's look reaches the box, from whether the
// look is on that component, and it holds until the look changes. Null/false when unresolved.
void* TakeOutComponent(void* box);
bool ReadLooksAtTakeOut(void* box, bool& out);

// The box's install and take-out, called as the player's use and E-press call them: playerUsedOn with the
// player (the body reads the upgrade from the player's hand, `holding_actor`), a hit on the box and the
// held actor and its name; actionOptionIndex with action 4 and a hit on the take-out component. [dev]
bool CallInstall(void* box, void* player, void* held, const reflection::FName& heldName);
bool CallTakeOut(void* box, void* player);

// How many initialServerUpgradeSpawn_C are alive: the one-shot actor that rolls those levels at
// the gamemode's begin-play and then destroys itself. Counted from the object index; the class
// lookup walks the array only on a miss, and after a miss waits for the next world. 0 while the
// class is not loaded.
int32_t CountUpgradeSpawners();

}  // namespace ue_wrap::serverbox
