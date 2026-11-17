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

// ---- build_probe_watch.cpp ----

// Registers the four watches on the wall's bodies and the unfinished wall's overlap delegate, retried each second.
// True once every one is settled (live, or dead and said). Game thread.
bool InstallWatches();

// The three damage routes of the wall, and the delegate's firings.
uint32_t OverlapFires();

// A leg's window on the routes: the routes that fired since LegBegin, in order, each with its caller.
void LegBegin(const char* leg);
bool LegRouteFired();
// The leg's line: routes, caller chain, the actors every addDamage of the leg struck, the wall's health before and after.
void LegEnd(const char* leg, void* wall, float healthBefore);

// A float or an int field of `obj` by name, through reflection. False when it does not resolve.
bool ReadFloat(void* obj, const wchar_t* name, float& out);
bool WriteFloat(void* obj, const wchar_t* name, float value);
bool ReadInt(void* obj, const wchar_t* name, int32_t& out);

// ---- build_probe_place.cpp ----

// Traces a ground line from 300 cm above `ask` to 1000 cm below (ignoring the live actor `ctx`), spawns `classPath` at the
// hit and reads its place back. The line says the ask, the hit, its actor and the read-back; null with the INVALID verdict
// said when no ground was hit, nothing spawned or the read-back is over 50 cm from the hit. `placed` is the read-back
// place. Game thread.
void* PlaceOnGround(void* ctx, const wchar_t* classPath, const ue_wrap::FVector& ask, const char* what,
                    ue_wrap::FVector& placed);

// The level's own wall: the customWall_C of the current world's persistent level, or null while none lives.
void* LevelWall();

// ---- build_probe_damage.cpp ----

// The host's legs, first at the level's wall and then at a customWall_C of its own placed beside it: its walk, the thrown
// brick, the swung axe, the explosion beside it.
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

// A thrown brick's distance to a wall's bounding box, read each tick from the throw on: the closest it came, the last one
// read (at rest, or before it was gone) and whether the closest was within a touch.
struct Approach {
    float min = -1.f, last = -1.f;
    void Reset() { min = last = -1.f; }
    void Sample(void* brick, void* wall);
    bool Touched() const;
    std::string Say() const;   // "closest=<cm> last=<cm> touched=<0|1>", or "not sampled"
};

}  // namespace coop::dev::build_probe::detail
