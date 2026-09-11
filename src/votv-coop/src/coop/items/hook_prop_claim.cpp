// coop/items/hook_prop_claim.cpp -- the hook lane feeding the driven-prop channel: on the host,
// every prop a hook on this machine is tied to is claimed for as long as the tie holds.
//
// The tie is a PhysX constraint, and coop/items/hook_constraint puts every one of them on the host:
// the host's own hook in its owner phase, every adopted anchored hook, the host's mirror of a
// client's hook once it has bitten, and the hooks the world itself holds -- the save's anchored
// ones the load path re-ties and the level-placed variant that ties itself at begin-play, which no
// table of the lane names and the shared scan pass finds. All four sets are read here every pass.
// Game thread, host only.

#include "hook_sync_detail.h"

#include "coop/element/object_scan_hub.h"   // the world's own hooks, found by the shared pass
#include "coop/props/prop_drive_host.h"
#include "ue_wrap/actors/hook.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"  // Generation: the scan's stamp

#include <cstdint>
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

// The hooks the WORLD holds on this machine that no table of the lane names: the save's anchored
// hooks, re-tied by the game's own load path at world start, and the level-placed variant, which
// ties itself at begin-play. Their constraints are as real as an adopted hook's, and a prop they
// hold needs the same stream. Found by the shared scan pass, stamped with the world it walked,
// and read only while that stamp is current; liveness by the index the pass captured.
struct WorldHook { void* actor; int32_t idx; };
std::vector<WorldHook> g_world;         // the last completed pass
std::vector<WorldHook> g_worldScratch;  // the pass in progress
uint32_t               g_worldGen = 0;

bool HubIsInstance(void* obj) { return H::IsHookFamily(obj); }   // class-pure: a descent test
void HubPassBegin(void*, bool) { g_worldScratch.clear(); }
void HubMatch(void*, void* obj) {
    if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
    if (!R::IsLive(obj)) return;
    g_worldScratch.push_back(WorldHook{obj, R::InternalIndexOf(obj)});
}
size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    if (isFull) {
        g_world.swap(g_worldScratch);
    } else {
        for (auto it = g_world.begin(); it != g_world.end();)   // a tail pass: prune, then add
            it = R::IsLiveByIndex(it->actor, it->idx) ? it + 1 : g_world.erase(it);
        for (const WorldHook& w : g_worldScratch) {
            bool seen = false;
            for (const WorldHook& k : g_world) if (k.actor == w.actor) { seen = true; break; }
            if (!seen) g_world.push_back(w);
        }
    }
    g_worldScratch.clear();
    g_worldGen = worldGen;
    return g_world.size();
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
    for (const auto& kv : Mirrors())
        if (kv.second.bitten)
            if (void* h = kv.second.ref.Get()) CollectTied(h);
    if (g_worldGen == ue_wrap::world_identity::Generation())
        for (const WorldHook& w : g_world)
            if (R::IsLiveByIndex(w.actor, w.idx)) CollectTied(w.actor);
    // Every tied prop is claimed every pass: the claim is idempotent, and the channel ends a claim
    // on its own when a hand takes the prop, so a tie that outlives the hand is re-claimed here
    // the pass after the hand lets go.
    for (const auto& r : g_next)
        coop::prop_drive_host::Claim(r.Raw(), "hook");
    for (const auto& r : g_tied)
        if (!InNext(r.Raw())) coop::prop_drive_host::Release(r.Raw());
    g_tied.swap(g_next);
}

void ResetPropClaims() {
    g_tied.clear();
    g_next.clear();
    g_world.clear();
    g_worldScratch.clear();
    g_worldGen = 0;
}

void RegisterWorldHooksScan() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        "hook_world", nullptr, &H::EnsureResolved, &HubIsInstance,
        &HubPassBegin, &HubMatch, &HubPassComplete, /*settleScans*/ 15});
}

}  // namespace coop::hook_sync::detail
