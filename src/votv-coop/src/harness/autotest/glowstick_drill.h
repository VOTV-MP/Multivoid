// harness/autotest/glowstick_drill.h -- what the glowstick drill's files share (autotest_glowstick.cpp, glowstick_drill_common.cpp,
// glowstick_drill_client.cpp, glowstick_drill_host.cpp). Private to them: co-located under src/, not include/.
//
// Armed by the row glowstick_drill on BOTH peers (the folder is VOTVCOOP_CAN_DIR); each arm is its own run, the host
// says `rig: VERDICT glowstick-<arm> PASS|FAIL|INVALID`, every verdict is decided at its event, the first FAIL ends the
// run, and a bound says INVALID only. The HOST spawns the glowstick (and a box) in front of the client's puppet; the
// CLIENT grabs it with the director's aimed grab and sticks it by the glowstick's own `ray`. No peer is teleported or
// walked. The arms and their reds are the row's text (config_registry_rows_dev_prop_intent.inc).

#pragma once

#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/glowstick_stick_judge.h"
#include "coop/save/join_window_baseline.h"
#include "coop/props/glowstick_sync.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_save_data.h"
#include "coop/props/remote_prop.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/engine_physics.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace harness::autotest::glowstick_drill {

namespace PS = harness::autotest::peer_signal;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace D = coop::director;
namespace RV = coop::rig_verdict;
namespace GS = coop::glowstick_sync;
namespace GJ = coop::glowstick_stick_judge;
using disc_drill::OnGameThread;
using ue_wrap::FVector;

constexpr int kStepSec = 120;
constexpr int kEventSec = 30;   // INVALID bound: an event the lanes owe within a round trip and a frame
constexpr float kBoxAheadCm = 40.f;    // the box the glowstick rests on stands this far in front of the client's puppet
constexpr float kBoxFarCm = 140.f;     // attached: the box stands this far ahead of the client, on the floor
constexpr float kGlowNearCm = 40.f;    // attached: the glowstick lies this far ahead, on the floor
constexpr float kAboveFloorCm = 15.f;  // a spawn point above the floor the trace found
constexpr float kFloorSearchCm = 300.f;
constexpr float kSideCm = 80.f;        // the floor point a floor stick is made at lies this far to the puppet's right
constexpr float kReachCm = 190.f;      // the camera's distance to a glowstick the grab takes: the player's trace is 200 cm (armLength)
constexpr float kHoldAboveCm = 40.f;   // the held glowstick starts this far over its surface, tip down, and steps down
constexpr float kSamePointCm = 3.f;    // a copy's point is the client's within this
constexpr float kNudgeCmPerSec = 400.f;   // the joiner's nudge of its own copy, away from the stick point
constexpr float kNudgeUpCmPerSec = 150.f;
constexpr float kHeldCm = 5.f;            // a constrained copy keeps its distance from the point: measured 35.0 at rest and 35.0 after; a free one
                                          // travels: 34.1 at rest and 170.7 after (the glowstick_restore_off red)
constexpr float kLiftCmPerSec = 700.f;   // the attached arm lifts its box with this upward velocity (a rise of about 250 cm)
constexpr float kMovedCm = 100.f;      // the pushed box has moved at least this far
constexpr float kFollowShare = 0.5f;   // a glowstick that follows its box travels at least this share of the box's travel
constexpr const wchar_t* kGlowPath = L"/Game/objects/prop_glowstick.prop_glowstick_C";
constexpr const wchar_t* kBoxPath = L"/Game/objects/prop_box.prop_box_C";

std::string TagStr();bool Aborted();
// Polls an event every 100 ms; a bound passed says INVALID and stops the other peer; the other peer's abort ends the
// wait at once with its reason.
template <class Pred>
bool Wait(const char* what, int seconds, Pred pred) {
    bool byPeer = false;
    const bool ok = PS::WaitFor(what, seconds, [&] {
        if (pred()) return true;
        byPeer = Aborted();
        return byPeer;
    });
    if (!ok) {
        PS::Mark("abort", std::string(what) + " not within " + std::to_string(seconds) + "s");
        return false;
    }
    if (!byPeer && !Aborted()) return true;
    std::string why;
    PS::Peek("abort", why);
    RV::Say(TagStr().c_str(), RV::Outcome::Invalid, "the other peer gave up while this one waited for %s: %s", what, why.c_str());
    return false;
}

inline bool WaitText(const char* name, std::string& out, int seconds = kStepSec) {
    return Wait(name, seconds, [&] { return PS::Peek(name, out); });
}
struct HostRead {
    int sticked = -1;
    FVector loc{};
    std::wstring key;
    bool driven = false;
    bool unsettled = false;
    int stickStatic = -1;
};

// ---- glowstick_drill_common.cpp ----
std::string Arm();
std::string TagStr();
// The arm's name as a C string, for the drill's log lines: they carry the verdict's tag stem (glowstick-<arm>:), which the
// rig's idle-tail instrument reads as the drill's own. The done lines keep `glowstick: DONE on host|client`, the rejoin marker.
const char* ArmCStr();
// A log line that begins with the arm's tag stem; GLOG and GLOGW below call it.
void Glog(bool warn, const char* fmt, ...);
bool ArmIs(const char* a);
bool Aborted();
void GiveUp(RV::Outcome outcome, const char* fmt, ...);
float Dist(const FVector& a, const FVector& b);
int Sticked(void* g);
FVector LocOf(void* g);
std::wstring AttachKeyOf(void* g);
void* ByKey(const std::wstring& key);
FVector Where(void* a);
std::string Num3(const FVector& v);
bool ParseNum3(std::istringstream& in, FVector& v);
std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);
bool ClientBody(void*& body, FVector& at);
bool WaitKey(void* actor, std::wstring& key);
bool SpawnAhead(const wchar_t* path, float ahead, float side, void*& actor, std::wstring& key, FVector* floorSide);
bool SpawnOnTop(const wchar_t* path, void* support, void*& actor, std::wstring& key);
HostRead ReadHost(const std::wstring& key);
void LogConstraint(const char* who, void* g);
GJ::dev::Counts Judged();

// ---- glowstick_drill_client.cpp ----
bool Grab(const std::wstring& key);
bool Stick(const std::wstring& key, const FVector& base);
void RunClient();
void RunRejoined();

// ---- glowstick_drill_host.cpp ----
void RunHost();

}  // namespace harness::autotest::glowstick_drill

#define GLOG(...) ::harness::autotest::glowstick_drill::Glog(false, __VA_ARGS__)
#define GLOGW(...) ::harness::autotest::glowstick_drill::Glog(true, __VA_ARGS__)
