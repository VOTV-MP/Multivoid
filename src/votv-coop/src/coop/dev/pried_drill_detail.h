// coop/dev/pried_drill_detail.h -- the pried drill's fixtures and witnesses, split from the arms' state machine
// (pried_drill.cpp) so that file keeps to the arms. Private to src/coop/dev/. Game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>

namespace coop::dev::pried_drill_detail {

// The three pryable classes a fixture can be.
enum class Fixture : uint8_t { Plank, Radiator, Tv };

const wchar_t* AssetPath(Fixture f);  // the class's cooked path, for the host's spawn
const char* Token(Fixture f);         // "plank" | "radiator" | "tv", the word the ready line carries
bool FixtureOfToken(const std::string& token, Fixture& out);

// The host's fixture: spawned at `at`, held static as the game places it (a copy that spawned free is held static
// the game's way), then left to the lane's birth. Null when the class did not load or the spawn failed.
void* SpawnFixture(Fixture f, const ue_wrap::FVector& at);

// The drill's engine-side witnesses, installed on whichever peer runs the arm: BreakConstraint's native post hook (which
// joints of a prop the engine's own function ran on) and a post watch on crowbarOpen (how many bodies completed on a
// prop). Asked every tick until both are live; true once they are. UFunction IsBroken reads 0 after BreakConstraint
// (measured on a rig run), so the dispatch of the native itself is the witness of a joint's break.
bool WitnessesLive();

// The joints of `prop` BreakConstraint ran on since the witnesses went live, bit N-1 for PhysicsConstraintN; 0 for none.
uint8_t JointMask(void* prop);

// The crowbarOpen bodies that ran to their end on `prop` since the witnesses went live. A cancelled call is not one.
uint32_t BodiesRun(void* prop);

// This peer's local player takes a crowbar into its hand, as a pick-up does. False when it did not.
bool TakeCrowbar();

// The client's pry of `prop` by the game's own route: aim at it, the crowbar in the hand's activatePrying, then the
// prying crowbar's End(pryOpen = true), which is the only caller of crowbarOpen. One stage a call.
enum class PryState : uint8_t { Working, Ended, Failed };
PryState StepRealPry(void* me, void* prop);
const char* RealPryWhy();  // what a Failed pry stopped at

// Back to nothing held: the witnesses' tables and the pry's stage. Net disconnect.
void Reset();

}  // namespace coop::dev::pried_drill_detail
