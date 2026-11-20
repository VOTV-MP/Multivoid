// coop/dev/constraint_join_drill_detail.h -- co-located private header of the constraint-join drill
// (coop/dev/constraint_join_drill.h).

#pragma once

#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::constraint_join_drill::detail {

constexpr const char* kTag = "constraint-join";   // the drill's name on its `rig: VERDICT` line

enum class Arm : uint8_t { Off, C1, C2, C3 };
Arm ArmOf();
constexpr size_t kMaxBoxes = 6;
// c1 is a chain of five nailed in line, c2 a pair, c3 a chain of six.
inline size_t BoxCount(Arm a) { return a == Arm::C1 ? 5 : a == Arm::C2 ? 2 : kMaxBoxes; }

constexpr const wchar_t* kBoxClass = L"prop_box_C";
constexpr const wchar_t* kBoxPath  = L"/Game/objects/prop_box.prop_box_C";
constexpr const wchar_t* kNailPath = L"/Game/objects/nail.nail_C";

constexpr float kStepCm       = 125.f;   // the footprint's step bound: the real step is a box's width plus kPropGapCm
constexpr float kPropGapCm    = 1.f;     // between neighbours' faces
constexpr float kLiftCm       = 60.f;    // a spawn's lift above the measured floor, so each box falls to rest
constexpr float kFootCm       = 75.f;    // the footprint kept clear round the set, beyond its ends
constexpr float kCrowdCm      = 400.f;   // another box within this of the spot ends the search for it
constexpr float kStandGapCm   = 80.f;    // the standpoint is this far past A's face, on the side away from B
constexpr float kStandReachCm = 30.f;    // the walk to the standpoint stops this near it
constexpr float kCarryCm      = 300.f;   // the carry walk, away from the chain
constexpr float kCarryReachCm = 30.f;
constexpr float kHeldOffCm    = 30.f;    // the held box has left the floor when it stands this far from where it rested
constexpr float kAwayCm       = 500.f;   // c2: the host's body is more than this from every box at the client's take
constexpr float kEndPoseCm    = 5.f;     // a copy at an End stands within this of the End's pose
constexpr int   kLiftBoundTicks = 180;
constexpr int   kWalkDeadlineS  = 300;
constexpr int   kQuietTicks     = 60;    // still ticks before a box is called at rest
constexpr int   kDriveFreeTicks = 180;   // and no drive open on any box for this many ticks before the set is called built
constexpr uint64_t kSpreadBoundMs = 500; // two rest probes: the bound the members' End times are reported against
// Time bounds say INVALID and nothing else: a verdict is decided by an event, never by a clock.
constexpr uint64_t kBoundMs = 360000;

// The set as the host tells it to the client: how many boxes, each one's element id, and where box A stands.
struct SetInfo {
    size_t           n = 0;
    uint32_t         eid[kMaxBoxes] = {};
    ue_wrap::FVector a{};
};
constexpr const char* kReadyPrefix = "cj ready ";
constexpr const char* kFinalPrefix = "cj final ";
std::string ReadyLine(const SetInfo& s);
bool ParseReady(const std::string& rest, SetInfo& out);
// One final line per box: its index, element id and the place the host's copy rests at.
std::string FinalLine(size_t i, uint32_t eid, const ue_wrap::FVector& at);
bool ParseFinal(const std::string& rest, size_t& i, uint32_t& eid, ue_wrap::FVector& at);

bool IsHost();
// The rejoined client's second life, which the rig tells by this variable (--rejoin-env).
bool SecondLife();
// True once this peer said a verdict: nothing further runs.
bool Over();
void Rearm();
// Says what this peer waits on, once per change of the text, so a step that never proceeds names its wait in the log.
void Wait(char who, const char* fmt, ...);
// `rig: VERDICT constraint-join INVALID -- <why>`: the drill could not measure.
void Invalid(char who, const char* fmt, ...);
// `rig: VERDICT constraint-join FAIL -- <leg>: <what>`: it measured a failure; no later leg runs.
void Fail(char who, const char* leg, const char* fmt, ...);
// PASS, once.
void Pass(const char* fmt, ...);

// One peer's carry of box A: the director walks it to a standpoint beside A on the side away from the chain, the use key's
// grab takes A, the view is raised until A leaves the floor, a carry walk takes it kCarryCm further from the chain, and
// dropGrabObject lets go. Says INVALID itself when a step cannot run. Game thread.
class CarryAct {
public:
    enum class Status : uint8_t { Working, Done, Failed };
    // `away` is the unit direction from the chain's A toward where it is carried, flat.
    void Begin(void* boxA, const ue_wrap::FVector& away, char who);
    Status Tick();
    void Reset();
private:
    enum class Step : uint8_t { Idle, Start, Walk, Grab, Lift, Carry, Done };
    void To(Step s);
    bool Expired() const;
    Step                                          step_ = Step::Idle;
    char                                          who_ = '?';
    uint64_t                                      stepMs_ = 0;
    int                                           liftTicks_ = 0;
    ue_wrap::CachedObjRef                         box_;
    ue_wrap::FVector                              away_{}, rest_{};
    std::shared_ptr<coop::director::BackgroundWalk> walk_;
    std::unique_ptr<coop::director::AimedGrab>    grab_;
};

namespace host {
void Tick(coop::net::Session& s);
void Reset();
// True while a c2 host has said its final lines and waits for the client's second life: a session end keeps its state.
bool KeepsAcrossDisconnect();
}  // namespace host

namespace client {
void Tick(coop::net::Session& s);
void Reset();
}  // namespace client

}  // namespace coop::dev::constraint_join_drill::detail
