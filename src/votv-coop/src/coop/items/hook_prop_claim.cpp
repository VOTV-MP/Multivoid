// coop/items/hook_prop_claim.cpp -- the hook lane feeding the driven-prop channel: on the host,
// every prop a real hook of this machine is tied to is claimed for as long as the tie holds.
//
// The tie is a PhysX constraint the game builds on the machine that ran attach_a -- the host's own
// hook in its owner phase, and every adopted anchored hook -- so those are the hooks read here. A
// mirror of a client's hook carries no constraint and moves nothing, and a client's own hook
// dragging a host prop is that client's copy diverging, which no host stream can describe; that
// direction is named in hook_sync.h and is not this file's. Game thread, host only.

#include "hook_sync_detail.h"

#include "coop/props/prop_drive_host.h"
#include "ue_wrap/actors/hook.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/reflection.h"

#include <vector>

namespace coop::hook_sync::detail {
namespace {

namespace H  = ue_wrap::hook;
namespace PR = ue_wrap::prop;
namespace R  = ue_wrap::reflection;

// The props seen tied on the last pass, held through slot-validated references: a pointer seen
// before is answered by its own reference and never probed bare, the owner poll's rule for a
// field the game can leave dangling. Only a pointer seen for the first time reaches the bare
// liveness probe, once. Both vectors are kept so the 20 Hz pass allocates nothing.
std::vector<ue_wrap::CachedObjRef> g_tied;
std::vector<ue_wrap::CachedObjRef> g_next;

const ue_wrap::CachedObjRef* Known(void* p) {
    for (const auto& r : g_tied)
        if (r.Raw() == p) return &r;
    return nullptr;
}

bool InNext(void* p) {
    for (const auto& r : g_next)
        if (r.Raw() == p) return true;
    return false;
}

// The keyed props `hook` is tied to, appended to g_next without duplicates.
void CollectTied(void* hook) {
    void* a = nullptr;
    void* b = nullptr;
    if (!H::AttachedActors(hook, a, b)) return;
    for (void* p : {a, b}) {
        if (!p || InNext(p)) continue;
        if (const ue_wrap::CachedObjRef* known = Known(p)) {
            if (known->Alive()) g_next.push_back(*known);   // its own reference answers
            continue;
        }
        if (!R::IsLive(p) || !PR::IsDescendantOfProp(p)) continue;
        ue_wrap::CachedObjRef r;
        r.Set(p);
        g_next.push_back(r);
    }
}

}  // namespace

void TickPropClaims() {
    g_next.clear();
    for (const Owned& o : OwnedHooks())
        if (void* h = o.ref.Get()) CollectTied(h);
    for (const Adopted& a : AdoptedHooks())
        if (void* h = a.ref.Get()) CollectTied(h);
    // Every tied prop is claimed every pass: the claim is idempotent, and the channel ends a claim
    // on its own when a hand takes the prop, so a tie that outlives the hand is re-claimed here
    // the pass after the hand lets go.
    for (const auto& r : g_next)
        coop::prop_drive_host::Claim(r.Raw(), "hook");
    for (const auto& r : g_tied)
        if (!InNext(r.Raw())) coop::prop_drive_host::Release(r.Raw());
    g_tied.swap(g_next);
}

void ResetPropClaims() { g_tied.clear(); g_next.clear(); }

}  // namespace coop::hook_sync::detail
