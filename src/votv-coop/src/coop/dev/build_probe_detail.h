// coop/dev/build_probe_detail.h -- the building probe's private seams between its files (src tree, not include/); see
// coop/dev/build_probe.h.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::build_probe::detail {

enum class Role : uint8_t { Solo, Host, Client };
const char* RoleName(Role r);

// A leg's tick answer: still going, finished, or unable to measure (the INVALID verdict was said, once).
enum class Res : uint8_t { Working, Done, Invalid };

constexpr const char* kTag = "build-probe";

// "build-probe: ..." at info and at warning level.
void Info(const char* fmt, ...);
void Warn(const char* fmt, ...);
// The verdict INVALID with this reason; returns Res::Invalid.
Res Invalid(const char* fmt, ...);

std::string Narrow(const std::wstring& w);
std::string ClassOrNull(void* obj);

// ---- build_probe_census.cpp ----

// Every live nail, wall, unfinished wall and poster of the current world, one line each and a summary, from a world
// whose edge was at `edgeMs` (the net clock). `generation` numbers the worlds this process census saw, from 1.
void RunCensus(Role role, int generation, uint64_t edgeMs);

// Asks the prop key index again for each piece it did not hold at the census, once its version moved; a piece that
// began to be held says how long after the edge. Cheap while nothing moved.
void PollKeyIndex(uint64_t nowMs);

// The keys of this census against the one before, by class and place; one line per piece that changed or went and a
// summary line. A no-op for the first census.
void SayKeyDiff(int generation);

// The summary of what the key index never held in this world, said once at the end of the run.
void SayKeyIndexEnd();

// The level's own wall: the persistent customWall_C the census found, or null while none lives.
void* LevelWall();

// ---- build_probe_watch.cpp ----

// Registers the four watches on the wall's bodies and the unfinished wall's overlap delegate, retried each second.
// True once every one is settled (live, or dead and said). Game thread.
bool InstallWatches();

// The three damage routes of the wall, and the delegate's firings.
uint32_t OverlapFires();

// A leg's window on the routes: the routes that fired since LegBegin, in order, each with its caller.
void LegBegin(const char* leg);
bool LegRouteFired();
// The leg's line: routes, caller chain, the wall's health before and after.
void LegEnd(const char* leg, void* wall, float healthBefore);

// A float or an int field of `obj` by name, through reflection. False when it does not resolve.
bool ReadFloat(void* obj, const wchar_t* name, float& out);
bool WriteFloat(void* obj, const wchar_t* name, float value);
bool ReadInt(void* obj, const wchar_t* name, int32_t& out);

// ---- build_probe_damage.cpp ----

// The host's three legs at the level's wall: its walk, the thrown brick, the swung axe, the explosion beside it.
Res TickDamageLegs();
void ResetDamageLegs();

// ---- build_probe_brick.cpp ----

// The brick leg, one tick of each half. The host spawns the unfinished wall before slot 1's puppet and tells the client;
// the client spawns its own at that place and throws a brick into it.
Res TickBrickHost();
Res TickBrickClient();
void ResetBrick();

// ---- build_probe_aim.cpp: the camera and the thrown brick, both halves ----

// Turn the local player's view to `at` (the controller's rotation, read on the next tick).
void FaceTowards(void* player, const ue_wrap::FVector& at);
// Whether the view lies within a few degrees of the direction to `at`.
bool ViewOn(const ue_wrap::FVector& at);

// Every live prop of the generic prop class whose row is `brick`, into `out`; and the first one not in `before`.
void SnapshotBricks(std::vector<void*>& out);
void* NewBrick(const std::vector<void*>& before);
// The center of the actor's bounds, else its place; false when neither reads.
bool CentreOf(void* actor, ue_wrap::FVector& out);

}  // namespace coop::dev::build_probe::detail
