// coop/dev/pried_drill_fixture.cpp -- see coop/dev/pried_drill_detail.h.

#include "pried_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/dev/director/director.h"
#include "coop/dev/drill_hand.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_pry_host.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_flags.h"
#include "ue_wrap/actors/prop_pry.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::pried_drill_detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace PP = ue_wrap::prop_pry;
namespace sg = ue_wrap::script_gate;
namespace OI = ue_wrap::object_index;
using ue_wrap::FVector;

// The two witnesses: the native that breaks a joint, and a post watch on the pry's own body (the lane's own watch is its
// own; this one only counts).
UE_FACT_FN(kRowCrowbarOpen, "pried_drill", nullptr, L"crowbarOpen", AnyClass);
constexpr int kTagBody = 0x50445742;  // 'PDWB'

struct Mark {
    ue_wrap::CachedObjRef prop;
    uint8_t  mask = 0;
    uint32_t bodies = 0;
};
std::vector<Mark> g_marks;  // a handful: the fixture and the odd stray

Mark& MarkOf(void* prop) {
    for (Mark& m : g_marks)
        if (m.prop.Alive() && m.prop.Raw() == prop) return m;
    if (g_marks.size() >= 16) g_marks.erase(g_marks.begin());
    g_marks.emplace_back();
    g_marks.back().prop.Set(prop);
    return g_marks.back();
}

// The lane's own BreakConstraint hook calls this (prop_pry_host::SetBreakListener): one patch on the seam.
void OnBreak(void* owner, int joint) { MarkOf(owner).mask |= static_cast<uint8_t>(1u << (joint - 1)); }

void OnBodyPost(const sg::Call& c) {
    if (PP::KindOf(c.object) != PP::Kind::None) ++MarkOf(c.object).bodies;
}

bool g_watchAsked = false, g_watchOk = false;
bool g_missSaid[3] = {};  // each unmet witness condition is said once, on its first miss

// ---- the client's real pry ----------------------------------------------------------------------------------------

enum class Stage : uint8_t { Take, Aim, Pry, End, Done, Failed };
Stage g_stage = Stage::Take;
const char* g_why = "";
ue_wrap::CachedObjRef g_pryer;

float Dist2(const FVector& a, const FVector& b) {
    return (a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z);
}

void CollectOne(void* ctx, void* obj, int32_t) {
    if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) static_cast<std::vector<void*>*>(ctx)->push_back(obj);
}

int Hidden(void* a) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!a || !R::FindBoolProperty(R::ClassOf(a), L"bHidden", off, mask)) return -1;
    return (static_cast<const uint8_t*>(a)[off] & mask) ? 1 : 0;
}

bool HandHoldsCrowbar() {
    void* const h = coop::hand_item::LocalHandActor();
    return h && R::ClassNameOf(h) == L"prop_crowbar_C";
}

PryState Fail(const char* why) {
    g_stage = Stage::Failed;
    g_why = why;
    return PryState::Failed;
}

// The crowbar in the hand, and the view turned to the prop: the use key's trace would have written lookAtActor, so it is
// written here, and a frame passes for the control rotation to take.
PryState Take(void* me, void* prop) {
    if (!TakeCrowbar() || !HandHoldsCrowbar()) return Fail("the crowbar did not come into the hand");
    FVector at{};
    E::TryGetActorLocation(prop, at);
    if (void* c = E::GetController(me)) E::SetControlRotation(c, coop::director::LookAt(E::GetCameraLocation(), at));
    g_stage = Stage::Aim;
    return PryState::Working;
}

// The game's own activatePrying: the crowbar in the hand calls the prop's nearest prying crowbar's begin.
PryState Pry(void* me, void* prop) {
    void* const hand = coop::hand_item::LocalHandActor();
    FVector at{};
    if (!hand || !E::TryGetActorLocation(prop, at)) return Fail("the prop or the hand is gone");
    const int32_t lo = R::FindPropertyOffset(R::ClassOf(me), L"lookAtActor");
    if (lo >= 0) std::memcpy(static_cast<uint8_t*>(me) + lo, &prop, sizeof(prop));
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(hand), L"activatePrying");
    const int32_t po = fn ? R::FindParamOffset(fn, L"player") : -1;
    const int32_t ao = fn ? R::FindParamOffset(fn, L"atLocation") : -1;
    bool ran = false;
    if (fn && po >= 0 && ao >= 0) {
        std::vector<uint8_t> frame(static_cast<size_t>(R::FunctionFrameSize(fn)), 0u);
        std::memcpy(frame.data() + po, &me, sizeof(me));
        std::memcpy(frame.data() + ao, &at, sizeof(at));
        ran = R::CallFunction(hand, fn, frame.data());
    }
    void* shown = nullptr;
    if (void* cls = OI::ClassByName(L"pryingCrowbar_C")) {
        std::vector<void*> all;
        OI::DrainAll();
        OI::ForEachInstance(cls, &CollectOne, &all);
        for (void* p : all) {
            FVector pa{};
            if (E::TryGetActorLocation(p, pa) && Dist2(pa, at) < 150.f * 150.f && Hidden(p) == 0) shown = p;
        }
    }
    if (!ran || !shown) return Fail("activatePrying left no prying crowbar shown (the game refused the pry here)");
    g_pryer.Set(shown);
    g_stage = Stage::End;
    return PryState::Working;
}

// The end the game's own callers run: opened, so the prop's crowbarOpen runs. lastPlayer is set first, as the use does.
PryState End(void* me) {
    void* const p = g_pryer.Get();
    void* const fn = p ? R::FindDispatchFunctionCached(R::ClassOf(p), L"End") : nullptr;
    const int32_t o1 = fn ? R::FindParamOffset(fn, L"pryOpen") : -1;
    if (!fn || o1 < 0) return Fail("the prying crowbar's End did not resolve");
    const int32_t lp = R::FindPropertyOffset(R::ClassOf(p), L"lastPlayer");
    if (lp >= 0) std::memcpy(static_cast<uint8_t*>(p) + lp, &me, sizeof(me));
    std::vector<uint8_t> frame(static_cast<size_t>(R::FunctionFrameSize(fn)), 0u);
    frame[static_cast<size_t>(o1)] = 1;
    if (!R::CallFunction(p, fn, frame.data())) return Fail("End did not run");
    g_stage = Stage::Done;
    return PryState::Ended;
}

}  // namespace

const wchar_t* AssetPath(Fixture f) {
    switch (f) {
    case Fixture::Plank:    return L"/Game/objects/prop_pryablePlank.prop_pryablePlank_C";
    case Fixture::Radiator: return L"/Game/objects/misc/radiator_base2.radiator_base2_C";
    case Fixture::Tv:       return L"/Game/objects/misc/securityTv.securityTv_C";
    }
    return L"";
}

const char* Token(Fixture f) {
    return f == Fixture::Plank ? "plank" : f == Fixture::Radiator ? "radiator" : "tv";
}

bool FixtureOfToken(const std::string& token, Fixture& out) {
    for (Fixture f : {Fixture::Plank, Fixture::Radiator, Fixture::Tv})
        if (token == Token(f)) { out = f; return true; }
    return false;
}

void* SpawnFixture(Fixture f, const FVector& at) {
    void* cls = ue_wrap::asset_load::LoadObjectByPath(AssetPath(f));
    void* prop = cls ? E::SpawnActor(cls, at) : nullptr;
    if (!prop) return nullptr;
    // The game places these static; one that spawned free is held the game's way, so the pry has something to free.
    if (!PR::IsStatic(prop) && !PR::IsFrozen(prop)) PR::CallSetPropProps(prop, true, false, false, false);
    return prop;
}

bool WitnessesLive() {
    if (!g_watchAsked) {
        g_watchAsked = true;
        g_watchOk = sg::WatchName(kRowCrowbarOpen, kTagBody, nullptr, &OnBodyPost);
    }
    coop::prop_pry_host::SetBreakListener(&OnBreak);  // the lane's Tick then installs the hook on either role
    const bool conds[3] = {g_watchOk, coop::prop_pry_host::BreakHookLive(), sg::NameWatchLive(kRowCrowbarOpen, kTagBody)};
    static const char* const kNames[3] = {"the crowbarOpen watch was refused", "BreakConstraint's hook is not on yet",
                                          "the crowbarOpen name has not resolved yet"};
    for (int i = 0; i < 3; ++i)
        if (!conds[i] && !g_missSaid[i]) {
            g_missSaid[i] = true;
            UE_LOGI("[PRIED-DRILL] witnesses not live: %s", kNames[i]);
        }
    return conds[0] && conds[1] && conds[2];
}

uint8_t JointMask(void* prop) {
    for (const Mark& m : g_marks)
        if (m.prop.Alive() && m.prop.Raw() == prop) return m.mask;
    return 0;
}

uint32_t BodiesRun(void* prop) {
    for (const Mark& m : g_marks)
        if (m.prop.Alive() && m.prop.Raw() == prop) return m.bodies;
    return 0;
}

bool TakeCrowbar() { return coop::dev::drill_hand::TakeIntoHandNow(L"crowbar", L"prop_crowbar_C"); }

PryState StepRealPry(void* me, void* prop) {
    if (!me || !prop) return Fail("the player or the prop is gone");
    switch (g_stage) {
    case Stage::Take: return Take(me, prop);
    case Stage::Aim:  g_stage = Stage::Pry; return PryState::Working;  // a frame for the control rotation to take
    case Stage::Pry:  return Pry(me, prop);
    case Stage::End:  return End(me);
    case Stage::Done: return PryState::Ended;
    case Stage::Failed: return PryState::Failed;
    }
    return PryState::Failed;
}

const char* RealPryWhy() { return g_why; }

void Reset() {
    g_marks.clear();
    g_stage = Stage::Take;
    g_why = "";
    g_pryer.Reset();
}

}  // namespace coop::dev::pried_drill_detail
