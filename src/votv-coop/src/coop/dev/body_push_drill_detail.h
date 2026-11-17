// coop/dev/body_push_drill_detail.h -- co-located private header of the body-push drill (coop/dev/body_push_drill.h).

#pragma once

#include "coop/dev/drill_ground.h"
#include "coop/dev/rest_compare.h"

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::body_push_drill::detail {

constexpr const char* kTag = "body-push";   // the drill's name on its `rig: VERDICT` line
constexpr int         kPushes = 5;        // the client's pushes
constexpr int         kLegs = kPushes + 1;   // and the host's own push after them

// The pushed prop is the woodchipper, the class the body-push change's own first drill walked into.
constexpr const wchar_t* kChipperClass = L"woodchipper_C";
constexpr const wchar_t* kChipperPath  = L"/Game/objects/woodchipper.woodchipper_C";
constexpr const wchar_t* kBoxPath      = L"/Game/objects/prop_box.prop_box_C";   // the control's box
constexpr float kControlDropCm = 100.f;   // the control's box starts this far above the woodchipper's top

constexpr float kPastCm         = 100.f;  // a walk's goal, past the prop's far side
constexpr float kReachCm        = 40.f;   // the director's stop distance from the goal
constexpr float kLiftCm         = 80.f;   // a spawn's lift above the measured floor, so the prop falls to rest
constexpr float kHalfCm         = 150.f;  // the footprint kept clear round the prop
constexpr float kCrowdCm        = 400.f;  // another woodchipper within this of the spot ends the drill
constexpr float kFindCm         = 700.f;  // the client's search radius round the host's measured spot
constexpr float kMovedCm        = 2.f;    // a copy that ended further than this from where it stood moved
constexpr float kContactCm      = 5.f;    // the pawn touched the prop when its capsule edge came within this of its bounds
// A push's rest mark waits this many still ticks, so the host's End and the copy's settle after it have landed and the pose
// marked is the final one.
constexpr int   kMarkTicks      = 180;
constexpr int   kWalkDeadlineS  = 300;   // a walk from a New Game start to the measured spot (the fireext drill's bound)
constexpr int   kMinPushes      = 3;      // the pushes with contact that moved a copy, at least, for a PASS

// Time bounds say INVALID and nothing else: a verdict is decided by the copies resting, never by a clock.
constexpr uint64_t kBindBoundMs = 240000;  // a spawn to its ids and its rest, a find of the copies: the drill family's step bound
constexpr uint64_t kPushBoundMs = 360000;  // a leg's wait for the other peer's line: the longest walk and its rest

// The chat lines the two peers say. The client's maxstep carries what it read; the host's ready carries the measured floor
// spot; a leg's end carries the director's state; a leg's rested carries how far the client's copy moved and its pose.
inline std::string StepLine(float cm) { return "bodypush maxstep " + std::to_string(cm); }
inline std::string StepPrefix() { return "bodypush maxstep "; }
inline std::string ReadyPrefix() { return "bodypush ready "; }
inline std::string DecidedLine(int leg) { return "bodypush decided " + std::to_string(leg); }
inline std::string EndPrefix(int leg) { return "bodypush end " + std::to_string(leg) + " "; }
inline std::string RestedPrefix(int leg) { return "bodypush rested " + std::to_string(leg) + " "; }
// The client's copy has no open row of its own (unparked, not syncing) before leg `leg` begins.
inline std::string ClosedLine(int leg) { return "bodypush closed " + std::to_string(leg); }
// The control before the pushes: the client says it found its copy, the host drops a box on the sleeping woodchipper and says
// the control is done.
inline const char* FoundLine() { return "bodypush found"; }
inline const char* ControlDoneLine() { return "bodypush control done"; }

using coop::dev::drill_ground::Dist;

// The horizontal gap from a pawn's capsule edge to a prop's bounds box (negative when they overlap), and the prop's height.
// False when the bounds cannot be read.
struct Footprint {
    ue_wrap::FVector origin{}, extent{};
};
bool ReadFootprint(void* actor, Footprint& out);
float GapCm(const ue_wrap::FVector& pawnAt, float pawnRadius, const Footprint& f);
inline float HeightCm(const Footprint& f) { return 2.f * f.extent.Z; }

using coop::dev::drill_ground::Settle;
using coop::dev::drill_ground::Stable;
using coop::dev::drill_ground::Restart;

// True once this peer said INVALID: nothing further runs.
bool Over();
// A new session runs the drill again.
void Rearm();
// Says what this peer waits on, once per change of the text, so a step that never proceeds names its wait in the log.
void Wait(char who, const char* fmt, ...);
// The dead marker: an INVALID line naming the peer and the reason, and the rig's INVALID verdict, once.
void Invalid(char who, const char* fmt, ...);

// The pushed prop's own engine hit events (ReceiveHit), counted per push on both peers and said in one line per push.
namespace hits {
void Install();                                            // the observer, once per process
bool Live();
void Track(void* prop, void* clientPawn, void* hostPawn);  // the copy counted, and the pawns a hit is attributed to
void BeginLeg(int leg);                                    // hits from here count under this push
void SayLeg(char who, int leg);
void NotifyFlag(char who, void* prop, bool setOn);          // read the body's hit-notify flag, log it, set it on
void Reset();
}  // namespace hits

namespace host {
void Tick(coop::net::Session& s);
void Reset();
}  // namespace host

namespace client {
void Tick(coop::net::Session& s);
void Reset();
}  // namespace client

}  // namespace coop::dev::body_push_drill::detail
