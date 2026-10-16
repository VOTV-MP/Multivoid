// coop/dev/explosion_drill_internal.h -- what the explosion drill's files share: the arm, the fixture and rest helpers
// (coop/dev/explosion_drill_fixture.cpp), the host's half (coop/dev/explosion_drill_host.cpp) and the client's half
// (coop/dev/explosion_drill_client.cpp). Dev only; game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <chrono>
#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dev::explosion_drill::detail {

using Clock = std::chrono::steady_clock;
using Vec = ue_wrap::FVector;

constexpr auto kBound = std::chrono::seconds(60);  // a failure bound, never a wait
constexpr float kFlyDistance = 100.f;
constexpr int   kPapers = 4;

enum class Arm : uint8_t { Off, Dissolve, Apply, Coast, SpawnMalformed };

// ---- the fixture and the rest helpers (explosion_drill_fixture.cpp) ----

float Dist(const Vec& a, const Vec& b);
float Len(const Vec& v);
bool Past(Clock::time_point t, std::chrono::seconds bound);

// A class from the object index or else the load by path (a class nobody loaded is not in the index); null when
// neither has it.
void* ResolveClass(const wchar_t* name, const wchar_t* path);

void Info(const char* fmt, ...);
void Warn(const char* fmt, ...);

// One body's rest: the Ticks it has stayed under the rest speed, a STATE and never a count of ticks waited.
struct Rest {
    int               ticks = 0;
    bool              have = false;
    Vec               last{};
    Clock::time_point lastAt{};
};
bool AtRest(const Rest& r);
void NoteSpeed(Rest& r, float speed);
// A body that does not simulate (the puppet, a kinematic copy): its speed is its location change over the time
// since the last Tick.
void NoteMoved(Rest& r, const Vec& p);
// A simulating prop's speed is its root's physics velocity; an unreadable body is not at rest.
void NotePhysics(Rest& r, void* actor);

// The blast point B: ahead of `body` along its yaw-forward at its Z, short of whatever the line to it hits (the trace
// is the static and dynamic one, pawns are not in its set).
enum class Fix : uint8_t { Wait, Ok, Blocked, NoTrace };
Fix ComputeB(void* body, void* ctx, Vec& out);

void RestLine(uint32_t eid, const Vec& at);

// ---- the two roles ----

// The host's half: one Tick of its phases, and back to the start.
void HostTick(coop::net::Session& s, Arm arm);
void HostReset();

// The client's half: one Tick of its phases, and back to the start.
void ClientTick(Arm arm);
void ClientReset();

}  // namespace coop::dev::explosion_drill::detail
