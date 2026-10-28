// coop/dev/swinger_drill_join.h -- [dev] the joiner arm of the swinger drill (swinger_drill=rejoin): a client that
// comes back into a session whose host's lid is already at rest judges its own copy of that lid against the host's.
//
// Life 1 is the measure arm. The host says one word at each client's world-ready edge (Session::IsSlotWorldReady,
// polled with a per-slot latch), live in the lobby chat: `go` while it has given no verdict; `rest <lane key> <pitch>
// <yaw> <roll> <x> <y> <z>`, its copy of the verdict's door now, after a PASS that moved the door; else `void <token>`,
// the reason life 2 cannot judge a pose. The client reads that word from its first connected tick (a live row fades
// after a few seconds, a join takes longer), waits for it before it picks a lid, and acts on no chat row of a past
// life (a seeded row is retained, a live one is not), so no clock decides which life it is. On `rest` the client does
// not drag: it waits for coop::swinger_join_pose to place its copy, waits for the copy to be still, and judges it
// against the host's rest pose with the settle tolerances. See swinger_drill.h.

#pragma once

#include "ue_wrap/core/types.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::net { class Session; }

namespace coop::dev::swinger_drill_join {

// The tolerances swinger_drill.cpp and this arm share: two reads this close in orientation and place are one rest, a
// copy that rests within the settle pair of another is the same, and a wait on an edge fails after the bound.
inline constexpr float kRestDeg   = 0.3f;
inline constexpr float kRestCm    = 0.5f;
inline constexpr float kSettleDeg = 3.f;
inline constexpr float kSettleCm  = 5.f;
inline constexpr auto  kSampleEvery = std::chrono::milliseconds(250);
inline constexpr auto  kClientBound = std::chrono::seconds(60);
// The host's lid census may stay empty this long after a peer's world-ready before the host calls the run INVALID.
inline constexpr auto  kArmBoundMs  = std::chrono::milliseconds(60000);
// A client that has heard no word this long after its first connected tick fails: a bound on a lost word, never a
// wait the run depends on.
inline constexpr auto  kWordBound   = std::chrono::seconds(240);

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b);

// The drill line's text split on spaces.
std::vector<std::string> SplitWords(const std::string& text);

// HOST: the verdict of the measure arm, kept for the greeting. `laneKey` names the verdict's door (empty when no
// lid is known), `result` is PASS, FAIL or INVALID, `fromArmDeg` the angle between the door at rest and where it
// began (negative when unread). It belongs to the host process, not to the session: the client's leave ends the
// session, and life 2 is greeted after it. A dev drill: the process exit ends it.
void NoteVerdict(const std::wstring& laneKey, const char* result, float fromArmDeg);

// HOST: for each client slot that became world-ready since the last call, say its word through `say`: `go` with no
// verdict yet; `rest` with the pose of the verdict's door after a PASS that moved it; else `void <token>`
// (life1-FAIL, life1-INVALID, rest-at-start, door-unreadable, rest-closed).
using SayFn = void (*)(const std::string&);
void GreetReadySlots(coop::net::Session& session, SayFn say);

// HOST: true on a tick when the host may try to arm its lid census: a peer's world is ready (the milestone the rig
// waits on, as door_drill's) and kSampleEvery has passed since the last try. A try that finds no lid is asked again;
// when kArmBoundMs has passed since the first try, it prints `host INVALID` once and is never true again.
bool HostArmDue(coop::net::Session& session, std::chrono::steady_clock::time_point now);

// The host's word as the client heard it.
enum class Word : uint8_t { None, Go, Rest, Void };

// CLIENT: reads the host's word from the chat rows tagged `tag` (live rows only), every call until one is heard.
// `rest` starts the judge (Begin), `void` and the word bound fail the run. Returns the word heard on this call.
Word PollWord(const char* tag, std::chrono::steady_clock::time_point now);

// CLIENT: the host's `go` has been heard (life 1 may pick a lid).
bool HeardGo();

// CLIENT: the host's `rest` word arrived: judge the copy of `laneKey` against this place and orientation.
void Begin(const std::string& laneKey, const ue_wrap::FRotator& rot, const ue_wrap::FVector& at);

// True from Begin until the verdict is given: the client does not act while it judges.
bool Active();

// CLIENT: one step of the judge, every drill tick.
void Tick(std::chrono::steady_clock::time_point now);

// The session ended: the greeting latch and the client's judge and word state; never the host's stored verdict.
void Reset();

}  // namespace coop::dev::swinger_drill_join
