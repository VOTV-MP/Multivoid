// coop/props/constraint_ties.cpp -- see coop/props/constraint_ties.h.

#include "coop/props/constraint_ties.h"

#include "coop/net/net_clock.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_drive_join.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace coop::constraint_ties {
namespace {

namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace UH = ue_wrap::ufunction_hook;
namespace sg = ue_wrap::script_gate;
namespace PD = coop::prop_drive_host;

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
uint32_t g_seenGen = 0;                  // the world generation the by-name seed last ran for

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

// A tie made while one of its ends is driven or in a hand: that end is owed a join, so the other end gets a row too. Owe
// only appends to its list (no walk, no hooked native), which is all a native patch may do.
void OweIfDriven(void* owner) {
    if (PD::IsDriven(owner) || PD::InAHand(owner)) coop::prop_drive_join::Owe(owner, "tie made");
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
    OweIfDriven(a);
    OweIfDriven(b);
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

struct SeedCtx {
    int32_t off1;
    int32_t off2;
    size_t  total;
    size_t  seeded;
};

void SeedInstance(void* ctx, void* obj, int32_t) {
    auto* s = static_cast<SeedCtx*>(ctx);
    if (!R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
    ++s->total;
    if (g_ties.find(obj) != g_ties.end()) return;   // a call the seam saw wins over the level's names
    void* a = nullptr;
    void* b = nullptr;
    std::memcpy(&a, static_cast<const uint8_t*>(obj) + s->off1, sizeof(a));
    std::memcpy(&b, static_cast<const uint8_t*>(obj) + s->off2, sizeof(b));
    if (!a || !b || a == b || !R::IsLive(a) || !R::IsLive(b)) return;
    Tie& t = g_ties[obj];
    t.constraint.Set(obj);
    t.a.Set(a);
    t.b.Set(b);
    ++s->seeded;
}

// The first question after the world changed: the old world's entries go, and the constraints the level configures by name
// (ConstraintActor1/2, no SetConstrainedComponents call) are read once. A walk of the live components at a join edge,
// never per tick.
void SeedIfWorldChanged() {
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen == g_seenGen) return;
    g_seenGen = gen;
    SweepStale();
    void* cls = OI::ClassByName(L"PhysicsConstraintComponent");
    SeedCtx s{cls ? R::FindPropertyOffset(cls, L"ConstraintActor1") : -1,
              cls ? R::FindPropertyOffset(cls, L"ConstraintActor2") : -1, 0, 0};
    if (!cls || s.off1 < 0 || s.off2 < 0) {
        UE_LOGW("constraint_ties: world %u: the by-name constraints cannot be read (class %p, offsets %d/%d)", gen, cls,
                s.off1, s.off2);
        return;
    }
    OI::ForEachInstance(cls, &SeedInstance, &s);
    UE_LOGI("constraint_ties: world %u: seeded %zu by name of %zu", gen, s.seeded, s.total);
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

size_t TiesOf(void* actor, void** out, size_t cap) {
    if (!actor || !out || cap == 0) return 0;
    SeedIfWorldChanged();
    size_t n = 0;
    for (auto it = g_ties.begin(); it != g_ties.end();) {
        Tie& t = it->second;
        // Only an entry naming `actor` is read: a stale pointer that matches is validated by its own reference.
        if (t.a.Raw() != actor && t.b.Raw() != actor) {
            ++it;
            continue;
        }
        void* a = t.a.Get();
        void* b = t.b.Get();
        if (!t.constraint.Get() || !a || !b) {
            it = g_ties.erase(it);
            continue;
        }
        if (n < cap) out[n++] = (a == actor) ? b : a;
        ++it;
    }
    return n;
}

size_t Count() { return g_ties.size(); }

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
