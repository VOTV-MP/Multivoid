// coop/props/constraint_ties.cpp -- see coop/props/constraint_ties.h.

#include "coop/props/constraint_ties.h"

#include "coop/net/net_clock.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace coop::constraint_ties {
namespace {

namespace R  = ue_wrap::reflection;
namespace UH = ue_wrap::ufunction_hook;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowSetConstrainedComponents, "constraint_ties", L"PhysicsConstraintComponent", L"SetConstrainedComponents",
           Declared);
UE_FACT_FN(kRowBreakConstraint, "constraint_ties", L"PhysicsConstraintComponent", L"BreakConstraint", Declared);

// One live tie: the constraint component and the owner actors of the two components it holds, each through a
// slot-validated reference.
struct Tie {
    ue_wrap::CachedObjRef constraint;
    ue_wrap::CachedObjRef a;
    ue_wrap::CachedObjRef b;
};
std::unordered_map<void*, Tie> g_ties;   // by the constraint component's address
size_t   g_sweepAt = 256;                // the size at which a new entry first drops the stale ones

bool g_preOn = false;
bool g_postOn = false;
bool g_preRefused = false;
bool g_liveSaid = false;
uint64_t g_tryMs = 0;
int32_t g_offComp1 = -1;
int32_t g_offComp2 = -1;

constexpr size_t kBreakSlots = 2;
BreakFn g_breakFns[kBreakSlots] = {};

// A component's owner actor: its Outer, a field read of the engine's own, so no UFunction runs inside a native patch.
void* OwnerOf(void* component) { return component ? R::OuterOf(component) : nullptr; }

void SweepStale() {
    for (auto it = g_ties.begin(); it != g_ties.end();) {
        const Tie& t = it->second;
        if (!t.constraint.Alive() || !t.a.Alive() || !t.b.Alive()) it = g_ties.erase(it);
        else ++it;
    }
}

// Two different owner actors make a tie; one actor (a laptop's no-collide pair, a hook's own parts), the world or nothing
// makes none, and a constraint re-tied to such an end is untied.
void Record(void* constraint, void* a, void* b) {
    if (!a || !b || a == b) {
        g_ties.erase(constraint);
        return;
    }
    const bool fresh = g_ties.find(constraint) == g_ties.end();
    if (fresh && g_ties.size() >= g_sweepAt) {
        SweepStale();
        g_sweepAt = std::max<size_t>(256, g_ties.size() * 2);
    }
    Tie& t = g_ties[constraint];
    t.constraint.Set(constraint);
    t.a.Set(a);
    t.b.Set(b);
}

// The call always runs: the index only reads it.
sg::Verdict OnSetConstrainedPre(void* context, void* /*sourceObject*/, const uint8_t* parms) {
    if (!context || !parms || g_offComp1 < 0 || g_offComp2 < 0) return sg::Verdict::Run;
    void* c1 = nullptr;
    void* c2 = nullptr;
    std::memcpy(&c1, parms + g_offComp1, sizeof(c1));
    std::memcpy(&c2, parms + g_offComp2, sizeof(c2));
    Record(context, OwnerOf(c1), OwnerOf(c2));
    return sg::Verdict::Run;
}

void OnBreakPost(void* context, void* /*sourceObject*/, void* /*result*/) {
    if (!context) return;
    g_ties.erase(context);
    for (BreakFn fn : g_breakFns)
        if (fn) fn(context);
}

}  // namespace

void Install() {
    if (g_postOn && (g_preOn || g_preRefused)) return;
    const uint64_t now = coop::net::NowMs();
    if (now < g_tryMs) return;
    g_tryMs = now + 1000;   // the class is looked up through the object array on a miss
    if (!g_postOn) {
        const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowBreakConstraint);
        g_postOn = fn && UH::InstallPostHook(fn, &OnBreakPost, /*armed=*/true);
    }
    // The pre hook steps the call's parameters through the VM's exec-handler table, which the script gate fills at boot.
    if (!g_preOn && !g_preRefused && sg::ExecHandlers()) {
        const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowSetConstrainedComponents);
        if (fn) {
            g_offComp1 = R::FindParamOffset(fn.Fn(), L"Component1");
            g_offComp2 = R::FindParamOffset(fn.Fn(), L"Component2");
            if (g_offComp1 < 0 || g_offComp2 < 0 || !UH::InstallPreHook(fn, &OnSetConstrainedPre, /*armed=*/true)) {
                g_preRefused = true;
                UE_LOGE("constraint_ties: no pre hook on SetConstrainedComponents (offsets %d/%d) -- no tie is learned",
                        g_offComp1, g_offComp2);
            } else {
                g_preOn = true;
            }
        }
    }
    if (g_postOn && g_preOn && !g_liveSaid) {
        g_liveSaid = true;
        UE_LOGI("constraint_ties: seams live (SetConstrainedComponents pre, BreakConstraint post)");
    }
}

bool AddBreakListener(BreakFn fn) {
    if (!fn) return false;
    for (BreakFn f : g_breakFns)
        if (f == fn) return true;
    for (BreakFn& f : g_breakFns)
        if (!f) {
            f = fn;
            return true;
        }
    return false;
}

bool BreakSeamLive() { return g_postOn; }

}  // namespace coop::constraint_ties
