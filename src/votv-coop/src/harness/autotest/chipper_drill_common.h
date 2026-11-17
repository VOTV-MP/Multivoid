// harness/autotest/chipper_drill_common.h -- TEST-ONLY: what the chipper feed drill and its carry share: the key and box
// readers, and the director's walk, grab and carry stand. Co-located private header of autotest_chipper_feed.cpp and
// autotest_chipper_carry.cpp.

#pragma once

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/peer_signal.h"


#include "coop/config/config.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"  // LookAt, StartBackgroundWalk
#include "coop/interactables/woodchipper_gate.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_park.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/woodchipper.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflected_offset.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"  // DestroyActor, controller, camera
#include "ue_wrap/engine/engine_physics.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace harness::autotest::chipper_drill {

namespace PS = harness::autotest::peer_signal;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace W = ue_wrap::woodchipper;
using disc_drill::OnGameThread;
using ue_wrap::FVector;

constexpr const char* kTag = "chipper-feed";
constexpr const wchar_t* kChipperPath = L"/Game/objects/woodchipper.woodchipper_C";
constexpr const wchar_t* kPaperPath = L"/Game/objects/prop_paper.prop_paper_C";
constexpr int kStepSec = 120;
constexpr int kEatSec = 60;
constexpr float kUprightDeg = 5.f;

constexpr float kRestCmS = 5.f;
constexpr int kRestReadings = 3;


inline void* ByKey(const std::wstring& key) {
    void* a = nullptr;
    return coop::prop_key_index::Resolve(key, &a) == coop::prop_key_index::Read::Hit ? a : nullptr;
}

inline bool Gone(const std::wstring& key) {
    bool gone = false;
    OnGameThread([&] {
        void* a = nullptr;
        gone = coop::prop_key_index::Resolve(key, &a) == coop::prop_key_index::Read::Miss;
    });
    return gone;
}

inline std::wstring KeyOfSpawn(void* actor) {
    std::wstring key;
    for (int i = 0; i < 100 && key.empty(); ++i) {
        OnGameThread([&] {
            if (!R::IsLive(actor)) return;
            std::wstring k = ue_wrap::prop::GetKeyString(actor);
            if (!k.empty() && k != L"None") key = std::move(k);
        });
        if (key.empty()) ::Sleep(100);
    }
    return key;
}

inline std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }

inline std::wstring Field(const std::string& text, const char* name) {
    const size_t at = text.find(name);
    if (at == std::string::npos) return {};
    const size_t from = at + std::strlen(name), to = text.find(' ', from);
    const std::string v = text.substr(from, to == std::string::npos ? std::string::npos : to - from);
    return std::wstring(v.begin(), v.end());
}

inline void* EatOf(void* chipper) {
    const int32_t off = chipper ? R::FindPropertyOffset(R::ClassOf(chipper), L"eat") : -1;
    void* eat = nullptr;
    if (off >= 0) std::memcpy(&eat, static_cast<const uint8_t*>(chipper) + off, sizeof(eat));
    return eat;
}

inline float Dist(const FVector& a, const FVector& b) {
    const float x = a.X - b.X, y = a.Y - b.Y, z = a.Z - b.Z;
    return std::sqrt(x * x + y * y + z * z);
}

// The eat box as it stands now: its place, its half extents (the scaled extent) and its three axes in the world, each a row
// of `u` (the box's own axes from its world rotation, no scaling in them).
struct EatFrame {
    FVector at{}, half{};
    float u[3][3] = {};
};

// Reads the eat box component's frame. False when the box does not read.
inline bool ReadEatFrame(void* eat, EatFrame& out) {
    void* fn = eat ? R::FindDispatchFunctionCached(R::ClassOf(eat), L"GetScaledBoxExtent") : nullptr;
    ue_wrap::ParamFrame f(fn);
    ue_wrap::FRotator rot{};
    if (!fn || !f.valid() || !ue_wrap::Call(eat, f) || !E::TryGetComponentLocation(eat, out.at) ||
        !E::TryGetComponentWorldRotation(eat, rot))
        return false;
    out.half = f.Get<FVector>(L"ReturnValue");
    const float k = 0.0174532925f;
    const float sp = std::sin(rot.Pitch * k), cp = std::cos(rot.Pitch * k);
    const float sy = std::sin(rot.Yaw * k), cy = std::cos(rot.Yaw * k);
    const float sr = std::sin(rot.Roll * k), cr = std::cos(rot.Roll * k);
    const float u[3][3] = {{cp * cy, cp * sy, sp},
                           {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp},
                           {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp}};
    std::memcpy(out.u, u, sizeof(u));
    return true;
}

// Whether `point` lies inside the eat box component `eat`: a point test against its frame. `local` and `half`, when given,
// take the point in the box's axes and the box's half extents, and `boxAt` the box's own place now, for the log. False
// when the box does not read.
inline bool PointInEatBox(void* eat, const FVector& point, FVector* local = nullptr, FVector* half = nullptr,
                          FVector* boxAt = nullptr) {
    EatFrame fr;
    if (!ReadEatFrame(eat, fr)) return false;
    const float d[3] = {point.X - fr.at.X, point.Y - fr.at.Y, point.Z - fr.at.Z};
    float t[3];
    for (int i = 0; i < 3; ++i) t[i] = d[0] * fr.u[i][0] + d[1] * fr.u[i][1] + d[2] * fr.u[i][2];
    if (local) *local = FVector{t[0], t[1], t[2]};
    if (half) *half = fr.half;
    if (boxAt) *boxAt = fr.at;
    return std::fabs(t[0]) <= fr.half.X && std::fabs(t[1]) <= fr.half.Y && std::fabs(t[2]) <= fr.half.Z;
}

// CLIENT: one director walk to `to`, true when it arrived. `carry` keeps what the hand holds.
inline bool WalkTo(const FVector& to, float reachCm, int deadlineS, bool carry) {
    std::shared_ptr<coop::director::BackgroundWalk> walk;
    OnGameThread([&] { walk = coop::director::StartBackgroundWalk(to, reachCm, deadlineS, carry); });
    if (!walk) return false;
    PS::WaitFor("the director's walk ends", deadlineS + 30, [&] { return walk->state.load() != 0; }, false);
    return walk->state.load() == 1;
}

// CLIENT: the paper into the grab, as the use key takes it, from a spot the director walked to.
// The use key reaches 170 cm from the camera, which stands 157 cm above the floor a paper lies on: the player must stand
// within about 60 cm of it, horizontally, or the trace falls short (a ring of 70 cm left it 175 cm away, and 6 tries failed).
constexpr float kGrabRingCm = 35.f;
inline bool GrabPaper(const std::wstring& paperKey) {
    for (int attempt = 0; attempt < 6; ++attempt) {
        FVector pl{};
        OnGameThread([&] {
            if (void* p = ByKey(paperKey)) E::TryGetActorLocation(p, pl);
        });
        const float a = 3.14159f + 0.6f * static_cast<float>(attempt);
        if (!WalkTo(FVector{pl.X + kGrabRingCm * std::cos(a), pl.Y + kGrabRingCm * std::sin(a), pl.Z}, 12.f,
                       attempt == 0 ? 300 : 60,
                       false))
            continue;
        std::shared_ptr<coop::director::AimedGrab> grab;
        OnGameThread([&] {
            void* me = coop::players::Registry::Get().Local();
            void* p = ByKey(paperKey);
            if (me && p) grab = std::make_shared<coop::director::AimedGrab>(me, p);
        });
        coop::director::GrabState st = coop::director::GrabState::Failed;
        for (int i = 0; grab && i < 1200; ++i) {
            OnGameThread([&] { st = grab->Tick(); });
            if (st != coop::director::GrabState::Working) break;
            ::Sleep(16);
        }
        if (st == coop::director::GrabState::Grabbed) return true;
        UE_LOGI("chipper-feed: CLIENT grab try %d failed (%s)", attempt + 1, grab ? grab->Why() : "no paper or player");
    }
    return false;
}

// What the last carry came closest to: the paper's distance from the box's centre at its last write.
inline float g_carryClosestCm = 1e9f;

// autotest_chipper_carry.cpp. CLIENT: the carry into the box the drill makes; what ended it.
enum class CarryResult { Done, Gone, Lost, NoSide, NotEaten };
CarryResult CarryToBox(const std::wstring& chipKey, const std::wstring& paperKey, bool held);

// True once the actor named by `key` has been still for kRestReadings readings, 500 ms apart, under kRestCmS; false
// when it is gone or `boundSec` runs out.
bool AtRest(const std::wstring& key, int boundSec);


}  // namespace harness::autotest::chipper_drill
