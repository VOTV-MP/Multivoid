// coop/dev/director/ladder_climb.cpp -- see coop/dev/director/ladder_climb.h.
//
// The BP facts (bp_cpp ladder_C, mainPlayer_C; line numbers are the pseudo-C++'s):
// actionOptionIndex (ladder.cpp:535) takes the closest point of the segment a..b_off to the player, refuses
// ("Cannot get on") when a capsule there overlaps something or the player is over 125 uu from it, else calls
// mainPlayer.useLadder(this) (:1722-1760); it does not read its `action`. useLadder (mainPlayer.cpp:14483) sets the
// player's `controlObject` to the ladder, and clears it on release. The player's AnyKey press and release events
// (mainPlayer.cpp:11838, :11848; bodies :3476, :3481 into :3094, :3054) hand the key to
// controlObject->intComs_anyKey, which keeps "forward" and "back" in the ladder's key list (ladder.cpp:523); the
// ladder's tick moves the player toward the top while the last key held is "forward" and calls drop() at the top.

#include "coop/dev/director/ladder_climb.h"

#include "coop/dev/director/input_stub.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/hit_result.h"

#include <chrono>
#include <cmath>

namespace coop::director::ladder {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
using ue_wrap::FVector;

constexpr uint8_t  kActionUse = 4;        // enum_interactionActions `use`, as every device's use verb passes it
constexpr uint64_t kClimbBoundMs = 60000;  // failure bound of the climb, never a wait for a milestone
constexpr float    kAtTopCm = 200.f;      // an end this near the top point is the top
constexpr uint64_t kProgressMs = 5000;    // the climb says where it is this often

// The AnyKey pair's generated stub names (a game update renames the K2Node stubs).
InputStub g_press{L"InpActEvt_AnyKey_K2Node_InputKeyEvent_3"};
InputStub g_release{L"InpActEvt_AnyKey_K2Node_InputKeyEvent_4"};

R::InstanceOffset g_ownerLadder{L"ladder"};         // the tower's UChildActorComponent
R::InstanceOffset g_childActor{L"ChildActor"};
R::InstanceOffset g_pointA{L"a"};                   // the foot, a UBillboardComponent
R::InstanceOffset g_pointTop{L"b_off"};             // the top
R::InstanceOffset g_look{L"look"};                  // the box a player's look strikes
R::InstanceOffset g_controlObject{L"controlObject"};  // the player's: what its input is handed to

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void* ObjectAt(void* obj, R::InstanceOffset& off) {
    const int32_t o = obj ? off.Of(obj) : -1;
    if (o < 0) return nullptr;
    void* p = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(obj) + o);
    return (p && R::IsLive(p)) ? p : nullptr;
}

bool PointAt(void* ladder, R::InstanceOffset& off, FVector& out) {
    void* comp = ObjectAt(ladder, off);
    return comp && E::TryGetComponentLocation(comp, out);
}

float Dist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// How far `p` is from the segment a..b: the distance the ladder's mount tests against 125 uu.
float SegmentDist(const FVector& p, const FVector& a, const FVector& b) {
    const FVector ab{b.X - a.X, b.Y - a.Y, b.Z - a.Z};
    const float len2 = ab.X * ab.X + ab.Y * ab.Y + ab.Z * ab.Z;
    float t = len2 > 0.f ? ((p.X - a.X) * ab.X + (p.Y - a.Y) * ab.Y + (p.Z - a.Z) * ab.Z) / len2 : 0.f;
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    return Dist(p, FVector{a.X + ab.X * t, a.Y + ab.Y * t, a.Z + ab.Z * t});
}

// The key the game's settings bind to `bindName` ("forward", "jump"): lib_C::getKeybindFromName, the call the ladder
// compares its keys against (ladder.cpp:483, :490). Its FKey is read as its name.
bool KeyBind(void* player, const wchar_t* bindName, R::FName& out) {
    void* libCls = R::FindClass(L"lib_C");
    void* lib = R::FindClassDefaultObject(L"lib_C");
    void* fn = libCls ? R::FindFunction(libCls, L"getKeybindFromName") : nullptr;
    if (!fn || !lib) return false;
    ue_wrap::ParamFrame f(fn);
    const R::FName name = ue_wrap::fname_utils::StringToFName(bindName);
    if (!f.valid() || name.ComparisonIndex == 0 || !f.Set(L"name", name) || !f.Set<void*>(L"__WorldContext", player) ||
        !ue_wrap::Call(lib, f))
        return false;
    out = f.Get<R::FName>(L"bind");
    return out.ComparisonIndex != 0;
}

// The verb: ladder_C::actionOptionIndex(player, hit on the look box, use, the look box).
bool CallUse(void* player, void* ladder) {
    void* look = ObjectAt(ladder, g_look);
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(ladder), L"actionOptionIndex");
    if (!look || !fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) &&
           ue_wrap::hit_result::Write(f, L"hit", ladder, look, E::GetComponentLocation(look)) &&
           f.Set<uint8_t>(L"action", kActionUse) && f.Set<void*>(L"lookAtComponent", look) &&
           ue_wrap::Call(ladder, f);
}

}  // namespace

void* Find(void* owner) {
    void* comp = ObjectAt(owner, g_ownerLadder);
    if (!comp || R::ClassNameOf(comp) != L"ChildActorComponent") return nullptr;
    void* actor = ObjectAt(comp, g_childActor);
    void* base = R::FindClass(L"ladder_C");
    if (!actor || !base) return nullptr;
    return R::IsDescendantOfAny(R::ClassOf(actor), &base, 1) ? actor : nullptr;
}

bool Foot(void* ladder, FVector& out) { return PointAt(ladder, g_pointA, out); }
bool Top(void* ladder, FVector& out) { return PointAt(ladder, g_pointTop, out); }

void Climb::Finish(void* player, void* ladder, const char* why, bool drop) {
    out_.why = why;
    phase_ = Phase::Done;
    if (!out_.mounted) return;
    out_.climbMs = NowMs() - startMs_;
    if (player && R::IsLive(player)) {
        if (!E::TryGetActorLocation(player, out_.end) && !out_.why) out_.why = "the player's place does not read";
        // The key goes up as a person's does; on a ladder still held, that takes "forward" off the ladder's list.
        if (!PressKeyEvent(player, g_release, forward_)) UE_LOGW("director/ladder: the release of forward did not run");
        // A climb that ran out is let go as a person lets go: the jump key, which the ladder answers with drop()
        // (ladder.cpp:483-:422), so the player is not left hanging where the climb stopped.
        if (drop) {
            R::FName jump{};
            if (!KeyBind(player, L"jump", jump) || !PressKeyEvent(player, g_press, jump))
                UE_LOGW("director/ladder: the jump that lets go of the ladder did not run");
        }
    }
    FVector top{};
    out_.atTop = ladder && R::IsLive(ladder) && Top(ladder, top) && Dist(out_.end, top) <= kAtTopCm;
}

void Climb::Release(void* player, void* ladder) {
    if (phase_ == Phase::Climb) Finish(player, ladder, "the climb was let go before its end");
}

bool Climb::Tick(void* player, void* ladder) {
    if (phase_ == Phase::Done) return true;
    if (!player || !ladder || !R::IsLive(player) || !R::IsLive(ladder)) {
        Finish(player, ladder, "the player or the ladder is gone");
        return true;
    }
    if (phase_ == Phase::Mount) {
        FVector me{}, foot{}, top{};
        if (!E::TryGetActorLocation(player, me) || !Foot(ladder, foot) || !Top(ladder, top)) {
            Finish(player, ladder, "the player's or the ladder's place does not read");
            return true;
        }
        out_.end = me;   // where a climb that never started stands
        if (!KeyBind(player, L"forward", forward_)) {
            Finish(player, ladder, "the game's forward key does not read");
            return true;
        }
        const bool called = CallUse(player, ladder);
        out_.mounted = called && ObjectAt(player, g_controlObject) == ladder;
        if (!out_.mounted) {
            UE_LOGW("director/ladder: mount refused (verb ran=%d): the player at (%.0f,%.0f,%.0f) is %.0f uu from the "
                    "ladder's foot-to-top line (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f); the verb takes up to 125, and a "
                    "capsule that overlaps something there is refused too",
                    called ? 1 : 0, me.X, me.Y, me.Z, SegmentDist(me, foot, top), foot.X, foot.Y, foot.Z, top.X, top.Y,
                    top.Z);
            Finish(player, ladder, called ? "the ladder refused the mount" : "the use verb did not run");
            return true;
        }
        FVector at{};
        E::TryGetActorLocation(player, at);
        UE_LOGI("director/ladder: mounted at (%.0f,%.0f,%.0f), %.0f uu from the line", at.X, at.Y, at.Z,
                SegmentDist(me, foot, top));
        startMs_ = NowMs();
        nextLogMs_ = startMs_ + kProgressMs;
        phase_ = Phase::Climb;
        if (!PressKeyEvent(player, g_press, forward_)) {
            Finish(player, ladder, "the press of forward did not run");
            return true;
        }
        return false;
    }
    const uint64_t now = NowMs();
    if (ObjectAt(player, g_controlObject) != ladder) {
        Finish(player, ladder, nullptr);
        return true;
    }
    if (now - startMs_ > kClimbBoundMs) {
        Finish(player, ladder, "the climb ran past its 60 s bound", /*drop*/true);
        return true;
    }
    if (now >= nextLogMs_) {
        nextLogMs_ = now + kProgressMs;
        FVector at{};
        if (E::TryGetActorLocation(player, at))
            UE_LOGI("director/ladder: climbing, at (%.0f,%.0f,%.0f) after %llu ms", at.X, at.Y, at.Z,
                    static_cast<unsigned long long>(now - startMs_));
    }
    return false;
}

}  // namespace coop::director::ladder
