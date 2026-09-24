// harness/autotest/broomstroke_world.h -- the broom drill's reach into the world: a broom in the
// hand, a place to stand, the button, the notifies counted, and every census its verdicts are read
// from. Private to the drill (autotest_broomstroke.cpp); every row it logs carries the machine's
// millisecond tick, which both processes of a drill read from one clock.
//
// A census walks only the instances of the classes it asks about, through the object index, so the
// drill does not render a name for every object in the world while it measures a roll.

#pragma once

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/types.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace harness::autotest::broom_world {

// Post `body` to the game thread and wait until it stores a non-zero into its argument.
template <class Fn>
int RunGT(Fn&& body) {
    auto done = std::make_shared<std::atomic<int>>(0);
    ue_wrap::game_thread::Post([done, body]() mutable { body(*done); });
    while (done->load() == 0) ::Sleep(5);
    return done->load();
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b);

// Put a broom in this peer's hand with the game's own pickup. False when the hand holds none.
bool EquipBroom(const char* who);

// Stand beside `target` with the game's own teleport, facing it turned by `bodyTurnDeg`, then turn
// the camera onto it. A body turned less than a puppet's turn-in-place threshold keeps its facing
// on the other peers while this one's camera, and so its heading, turns onto the target. Logs the
// segment the stroke will trace and the heading it will push along.
void AimAt(const ue_wrap::FVector& target, float bodyTurnDeg, const char* who, const char* phase);

// Stand at `at` facing `yawDeg`, with the game's own teleport; `at` is a body location this peer
// stood at before, so the teleport's trace accepts it.
void StandAt(const ue_wrap::FVector& at, float yawDeg);

// Turn the camera onto `target` where the body stands, and log the AIM row.
void LookAt(const ue_wrap::FVector& target, const char* who, const char* phase);

// This peer's body location and facing. False when there is no local player.
bool LocalBody(ue_wrap::FVector& at, float& yawDeg);

// Watch the broom's stroke notify on this peer: every "clean" notify on any broom is logged at its
// entry as a NOTIFY row with its holder's heading and velocity, whether this peer's own broom or a
// mirror the host runs a client's stroke on, and counted once it is over: when its body returns
// where `bodiesRunHere`, at its entry where the lane refuses the body. False when the gate refused
// the watch.
bool WatchNotifies(const char* who, bool bodiesRunHere);

// Press the broom's right mouse button and hold it until `strokes` strokes have run to their end on
// this peer or `holdMs` has passed, then release it where no stroke is in progress. Logs PRESS and
// RELEASE rows. The strokes seen.
int Swing(const char* who, const char* phase, int strokes, DWORD holdMs);

// At the end of the next stroke of this peer's own broom, take the push back from every clump among
// `ids` that the stroke set moving, before a physics step moves it, and silence the hit the clump
// re-piles on: a broom's clump arms its re-pile a random 0.5 to 1 s after its birth, so a clump only
// held still re-piles on the first settling hit after that. Silenced, it drops, lies un-piled, and
// its carry closes at rest. Logs a FREEZE row per clump. One stroke; Thaw undoes it.
void FreezeNextStroke(const std::vector<uint32_t>& ids);

// Give the clump `eid` the freeze silenced its hit back, so its next landing re-piles it as the
// game's would; ThawAll does it for every clump still silenced. Log a THAW row per clump.
void Thaw(uint32_t eid, const char* who);
void ThawAll(const char* who);

// Set the velocity of the clump `eid` is, as a knock from a body would. False when it is no clump.
bool Knock(uint32_t eid, const ue_wrap::FVector& velocity, const char* who, const char* phase);

// The chip pile nearest this peer's body that an element owns and that no other chip pile lies within
// `aloneCm` of (0 takes the nearest) and, given `floorAt`, within `withinCm` of it. False when none is.
bool PickChipPile(const char* who, const char* phase, ue_wrap::FVector& outPos, float aloneCm = 0.f,
                  const ue_wrap::FVector* floorAt = nullptr, float withinCm = 0.f);

// Spawn a chip pile at each of `at`, on a floor a pile lay on. Host only: a pile the host spawns
// reaches the clients through the adoption scan. Logs a SPAWNED row. How many spawned.
int SpawnPilesAt(const std::vector<ue_wrap::FVector>& at, const char* who, const char* tag);

// Spawn `count` chip piles on a ring of `radiusCm` around `center`, which is a pile on the floor.
int SpawnHeap(const ue_wrap::FVector& center, int count, float radiusCm, const char* who);

// How many element-owned chip piles are within `radiusCm` of `center`.
int ChipPilesNear(const ue_wrap::FVector& center, float radiusCm);

// Every element-owned chip pile within `radiusCm` of `center`, by id; and a CENSUS row.
void CensusPiles(const ue_wrap::FVector& center, float radiusCm, const char* who, const char* phase,
                 const char* tag, std::vector<uint32_t>* outIds);

// One pass of the tracker: each followed id's form and place, logged as a TRACK row when the form
// changed or it moved more than 5 cm; and the clumps no element owns within `radiusCm`, logged as an
// UNNAMED row when their count changes.
struct TrackState { uint32_t eid; int form; ue_wrap::FVector pos; };   // form: 0 gone, 1 pile, 2 clump
void TrackStep(std::vector<TrackState>& states, const ue_wrap::FVector& center, float radiusCm,
               int& unnamedLast, const char* who, const char* phase);
// Each followed id's form and place at the end of the tracker, as a FINAL row whether or not it moved.
void TrackFinal(const std::vector<TrackState>& states, const char* who, const char* phase);
// Each of `ids`' form and place now, unlogged.
std::vector<TrackState> FormsOf(const std::vector<uint32_t>& ids);

// Where the striker is as this peer sees it: its own body, or its puppet. False when there is no body
// or its location cannot be read.
bool StrikerBody(bool striking, uint8_t strikerSlot, ue_wrap::FVector& at);

// The dispenser phase's subject: the keyed dispenser pile nearest the base, the key breaking ties,
// so both peers name the same one; its trash count read at the pick.
struct Dispenser {
    void*            pile = nullptr;
    int32_t          idx = 0;
    std::wstring     key;
    ue_wrap::FVector pos{};
};
bool PickDispenser(const std::shared_ptr<Dispenser>& out, const char* who, int& trashOut);
bool DispenserAlive(const std::shared_ptr<Dispenser>& d);
int CountTrashNear(const ue_wrap::FVector& at, float radiusCm);   // outside every hand; game thread

// Every element-owned prop near `center`, outside every hand, as PROP rows and a CENSUS row.
void CensusProps(const ue_wrap::FVector& center, float radiusCm, const char* who, const char* phase,
                 const char* tag);
// The prop nearest `center` a stroke can move: element-owned, in no hand, neither static nor frozen.
bool PickProp(const ue_wrap::FVector& center, float radiusCm, const char* who, const char* phase,
              ue_wrap::FVector& outPos);

}  // namespace harness::autotest::broom_world
