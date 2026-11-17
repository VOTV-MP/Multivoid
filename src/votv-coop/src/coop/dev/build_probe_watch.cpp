// coop/dev/build_probe_watch.cpp -- the building probe's watches on the wall's damage bodies and the unfinished wall's
// overlap delegate, and the leg window that collects what they saw; see coop/dev/build_probe.h.

#include "build_probe_detail.h"

#include "coop/net/net_clock.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::build_probe::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;

UE_FACT_FN(kRowDamage, "build_probe", L"customWall_C", L"damage", Declared);
UE_FACT_FN(kRowAddDamage, "build_probe", L"customWall_C", L"addDamage", Declared);
UE_FACT_FN(kRowImpact, "build_probe", L"customWall_C", L"impactDamageCPP", Declared);
// Every class's addDamage: the actor each hit of a leg struck, whoever the target class is.
UE_FACT_FN(kRowAnyAddDamage, "build_probe", nullptr, L"addDamage", AnyClass);
// One literal on one line: the fact-row tools read a row's function name from a single string.
UE_FACT_FN(kRowOverlap, "build_probe", L"customWall_unfinished_C", L"BndEvt__customWall_unfinished_overlap_K2Node_ComponentBoundEvent_1_ComponentBeginOverlapSignature__DelegateSignature", Declared);

constexpr int kTagDamage = 0x42504430;   // 'BPD0'
constexpr int kTagAddDamage = 0x42504431;
constexpr int kTagImpact = 0x42504432;
constexpr int kTagOverlap = 0x42504433;
constexpr int kTagAnyAdd = 0x42504434;
constexpr uint64_t kRetryMs = 1000;
constexpr size_t   kMaxLegRoutes = 40;
constexpr size_t   kMaxLegTargets = 16;
constexpr uint32_t kMaxLines = 300;

struct Row {
    const ue_wrap::facts::Function* row;
    int                             tag;
    SG::PreFn                       pre;
    const char*                     name;
    bool                            registered = false;
    bool                            settled = false;
    bool                            live = false;
};

SG::Verdict OnDamage(const SG::Call& c);
SG::Verdict OnAddDamage(const SG::Call& c);
SG::Verdict OnImpact(const SG::Call& c);
SG::Verdict OnOverlap(const SG::Call& c);
SG::Verdict OnAnyAddDamage(const SG::Call& c);

Row g_rows[5] = {
    {&kRowDamage, kTagDamage, &OnDamage, "customWall_C::damage"},
    {&kRowAddDamage, kTagAddDamage, &OnAddDamage, "customWall_C::addDamage"},
    {&kRowImpact, kTagImpact, &OnImpact, "customWall_C::impactDamageCPP"},
    {&kRowOverlap, kTagOverlap, &OnOverlap, "customWall_unfinished_C::overlap delegate"},
    {&kRowAnyAddDamage, kTagAnyAdd, &OnAnyAddDamage, "addDamage (any class)"},
};

uint64_t g_nextTryMs = 0;
bool     g_statesSaid = false;
uint32_t g_lines = 0;
uint32_t g_overlaps = 0;

// A leg's window: the routes seen since LegBegin, in order.
bool                     g_inLeg = false;
std::vector<std::string> g_routes;
// The actors the leg's addDamage calls struck, each with its caller, in order and without a repeat in a row.
std::vector<std::string> g_targets;

// One parameter per row and function: a ParamOffset keeps the function it was asked with, so rows do not share one.
R::ParamOffset g_dmgOffDamage{L"damage"}, g_dmgOffAdd{L"damage"}, g_dmgOffImpact{L"damage"};
R::ParamOffset g_actorOffAdd{L"actor"}, g_actorOffImpact{L"actor"}, g_otherOff{L"OtherActor"};

const uint8_t* ParamBytes(const SG::Call& c, int32_t offset) {
    if (offset < 0 || !c.locals) return nullptr;
    const uint8_t* out = SG::OutParamPtr(c, offset);
    return out ? out : c.locals + offset;
}

float DamageOf(const SG::Call& c, R::ParamOffset& off) {
    float v = -1.f;
    if (const uint8_t* b = ParamBytes(c, off.Of(c.function))) std::memcpy(&v, b, sizeof(v));
    return v;
}

void* ActorOf(const SG::Call& c, R::ParamOffset& off) {
    void* a = nullptr;
    if (const uint8_t* b = ParamBytes(c, off.Of(c.function))) std::memcpy(&a, b, sizeof(a));
    return a && R::IsLive(a) ? a : nullptr;
}

std::string Caller(const SG::Call& c) {
    if (!c.callerObject || !c.callerFunction) return "native";
    return ClassOrNull(c.callerObject) + "." + Narrow(R::ToString(R::NameOf(c.callerFunction)));
}

void Note(const char* row, const SG::Call& c, float dmg, void* actor) {
    float health = -1.f;
    ReadFloat(c.object, L"health", health);
    const std::string caller = Caller(c);
    if (g_inLeg && g_routes.size() < kMaxLegRoutes) g_routes.push_back(std::string(row) + "<-" + caller);
    if (g_lines >= kMaxLines) return;
    ++g_lines;
    Info("route row=%s dmg=%.1f health=%.1f actor=%s caller=%s depth=%d fromOurCode=%d", row, static_cast<double>(dmg),
         static_cast<double>(health), ClassOrNull(actor).c_str(), caller.c_str(), c.depth, c.fromOurCode ? 1 : 0);
}

SG::Verdict OnDamage(const SG::Call& c) {
    Note("damage", c, DamageOf(c, g_dmgOffDamage), nullptr);
    return SG::Verdict::Run;
}
SG::Verdict OnAddDamage(const SG::Call& c) {
    Note("addDamage", c, DamageOf(c, g_dmgOffAdd), ActorOf(c, g_actorOffAdd));
    return SG::Verdict::Run;
}
SG::Verdict OnImpact(const SG::Call& c) {
    Note("impactDamageCPP", c, DamageOf(c, g_dmgOffImpact), ActorOf(c, g_actorOffImpact));
    return SG::Verdict::Run;
}

// The receiver of an addDamage is the struck actor (the call's `actor` is the damager): what a melee swing's or a blast's
// own trace resolved to, as the game's script saw it.
SG::Verdict OnAnyAddDamage(const SG::Call& c) {
    if (!g_inLeg || g_targets.size() >= kMaxLegTargets) return SG::Verdict::Run;
    const std::string entry = ClassOrNull(c.object) + "<-" + Caller(c);
    if (g_targets.empty() || g_targets.back() != entry) g_targets.push_back(entry);
    return SG::Verdict::Run;
}

// The unfinished wall's overlap delegate, before its body: a prop or clump the wall will take as material.
SG::Verdict OnOverlap(const SG::Call& c) {
    ++g_overlaps;
    void* other = ActorOf(c, g_otherOff);
    int32_t bricks = -1;
    ReadInt(c.object, L"bricks", bricks);
    std::wstring propName;
    if (other && ue_wrap::prop::IsDescendantOfProp(other)) propName = ue_wrap::prop::GetPropNameString(other);
    Info("overlap fired #%u: other=%s propName=%ls bricks=%d caller=%s", g_overlaps, ClassOrNull(other).c_str(),
         propName.c_str(), bricks, Caller(c).c_str());
    return SG::Verdict::Run;
}

void SayStates() {
    g_statesSaid = true;
    for (const Row& r : g_rows) Info("watch %s: %s", r.name, r.live ? "live" : "dead");
    if (!g_rows[2].live) Info("impactDamageCPP: not observable");
}

}  // namespace

bool InstallWatches() {
    if (g_statesSaid) return true;
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextTryMs) return false;
    g_nextTryMs = now + kRetryMs;
    bool all = true;
    for (Row& r : g_rows) {
        if (r.settled) continue;
        if (!r.registered) r.registered = SG::WatchName(*r.row, r.tag, r.pre, nullptr);
        if (!r.registered) {
            if (SG::IsInstalled()) r.settled = true;  // refused for good: said by the gate, dead here
            else all = false;
            continue;
        }
        SG::ResolvePendingNames();
        if (SG::NameWatchLive(*r.row, r.tag)) r.settled = r.live = true;
        else if (SG::NameWatchSettled(*r.row, r.tag)) r.settled = true;
        else all = false;
    }
    for (const Row& r : g_rows) all = all && r.settled;
    if (all) SayStates();
    return all;
}

uint32_t OverlapFires() { return g_overlaps; }

void LegBegin(const char*) {
    g_inLeg = true;
    g_routes.clear();
    g_targets.clear();
}

bool LegRouteFired() { return !g_routes.empty(); }

void LegEnd(const char* leg, void* wall, float healthBefore) {
    g_inLeg = false;
    std::string chain;
    for (const std::string& r : g_routes) chain += (chain.empty() ? "" : " ; ") + r;
    std::string struck;
    for (const std::string& t : g_targets) struck += (struck.empty() ? "" : " ; ") + t;
    float after = -1.f;
    const bool alive = wall && R::IsLive(wall) && ReadFloat(wall, L"health", after);
    Info("leg=%s routes=%zu chain=[%s] addDamage-struck=[%s] health %.1f -> %s wall=%s %s", leg, g_routes.size(),
         chain.c_str(), struck.c_str(), static_cast<double>(healthBefore),
         alive ? std::to_string(static_cast<double>(after)).c_str() : "gone", alive ? "alive" : "destroyed",
         g_rows[2].live ? "" : "impactDamageCPP=not-observable");
}

bool ReadFloat(void* obj, const wchar_t* name, float& out) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(obj) + off, sizeof(out));
    return true;
}

bool WriteFloat(void* obj, const wchar_t* name, float value) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    std::memcpy(static_cast<uint8_t*>(obj) + off, &value, sizeof(value));
    return true;
}

bool ReadInt(void* obj, const wchar_t* name, int32_t& out) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(obj) + off, sizeof(out));
    return true;
}

}  // namespace coop::dev::build_probe::detail
