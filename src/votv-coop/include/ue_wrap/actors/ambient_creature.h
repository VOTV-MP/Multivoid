// ue_wrap/actors/ambient_creature.h -- the deer, the walking tree and the walking mannequin, the dirt holes and the beehive
// branches: a tree's size and landing, a deer's dissolve, a walker's view and stand, a hole's place and a branch's nest. Each
// class is read by reflection on the game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <string>
#include <vector>

namespace ue_wrap::ambient_creature {

// The live deer and walking trees.
void ListDeer(std::vector<void*>& out);
void ListTrees(std::vector<void*>& out);

// A tree's mesh scale and whether it has landed: its mesh shows from the landing on.
bool ReadTree(void* tree, float& scale, bool& landed);

// A copy never lands by itself, as its movement does not run: this writes what the landing and the begin play did.
void WriteTree(void* tree, float scale, bool landed);

bool Destroy(void* creature);

// The deer's own dissolve; it ends in the deer's destroy.
bool Dissolve(void* deer);

// The walking mannequins.
void ListWalkers(std::vector<void*>& out);

// Writes its anim's stand: the pose it holds while its `looking` is true. False when the walker has no anim or its class has no
// such field.
bool WriteWalkerStanding(void* walker, bool standing);

// The controller the walker's own tick reads its gaze through: GetPlayerController(walker, 0). Null when it does not resolve.
// Game thread.
void* WalkerGazeController(void* worldContext);

// The parts of the walker's view test, each read apart: its mesh's bounds, the game's frustum test of them for this controller, and
// the mesh's WasRecentlyRendered(3). A part that was not read says so; `diagnostics` also reads the rest of the test after a frustum
// miss and the mesh's and the actor's hidden bits, for a drill that must say which part fails.
// Game thread: the calls and their frames are kept between calls.
struct WalkerView {
    void*             mesh = nullptr;
    ue_wrap::FVector  origin{}, extent{};
    bool              boundsRead = false;
    bool              frustumRead = false, inFrustum = false;
    bool              renderedRead = false, rendered = false;
    bool              visibleRead = false, visible = false;            // the mesh's bVisible
    bool              hiddenInGameRead = false, hiddenInGame = false;  // the mesh's bHiddenInGame
    bool              actorHiddenRead = false, actorHidden = false;    // the walker's bHidden
    bool              InView() const { return inFrustum && rendered; }
};
WalkerView WalkerViewParts(void* walker, void* playerController, bool diagnostics = false);

// The walker's own view test, as its tick runs it: inside this controller's frustum and rendered of late. Game thread.
bool WalkerInView(void* walker, void* playerController);

// Its `looking` flag: true while it holds still.
bool ReadWalkerLooking(void* walker);

// Is this actor a walker? Game thread: the class is held between calls.
bool IsWalker(void* actor);

// The walker's class object, null until it is loaded; the pointer a caller on any thread compares `ClassOf(actor)` with.
// Resolved on the game thread.
void* WalkerClass();

// A hexahive's and a sus hole's dirt holes, and the beehive spawner's branches.
void ListHoles(std::vector<void*>& out);
void ListBranches(std::vector<void*>& out);

// A plain copy of either: a branch spawned this way makes no nest of its own.
void* SpawnFixture(bool branch, const FVector& at, const FRotator& rot);

// A beehive branch's nest: `hiveKey` is the key its nest had (empty for None, which `touched` and a keyless save leave), and
// `hive` is the nest actor it holds, null unless that actor is live. False when the branch has no such fields. Game thread.
bool ReadBranch(void* branch, std::wstring& hiveKey, void*& hive);

// The game's own binder half, for a copy whose nest is a mirror: write `hive`, then run the branch's `getBee`, which binds the
// nest's `touched` delegate to the branch.
bool BindBranchNest(void* branch, void* nest);

// The game's own forget: the branch's `touched` unbinds the nest, nulls `hive` and sets `hiveKey` to None.
bool ReleaseBranchNest(void* branch);

// Null `hive` and leave the nest and its binding alone: for a branch about to end, whose `ReceiveDestroyed` would otherwise
// destroy the nest it holds. Game thread.
void DropBranchNest(void* branch);

// The sus hole spawner's own test of a spot: a trace 10 m above `point` to 10 m below it against static geometry, kept when it
// hits grass. `at` is the hit and `rot` its normal as a rotator, as the spawner places the hole; the host runs it on the point
// a client named. Game thread.
bool HoleSpot(const FVector& point, FVector& at, FRotator& rot);

}  // namespace ue_wrap::ambient_creature
