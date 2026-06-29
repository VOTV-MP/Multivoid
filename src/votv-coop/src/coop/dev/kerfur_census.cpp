// kerfur_census -- see header. CLIENT one-shot diagnostic; reads + logs only.
#include "coop/dev/kerfur_census.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/element/prop.h"
#include "coop/creatures/kerfur_entity.h"        // IsKerfurClass / IsKerfurPropClass
#include "coop/net/session.h"          // Session::role() + net::Role (complete type)
#include "coop/creatures/npc_sync.h"             // GetSession (client-only gate)
#include "coop/props/remote_prop_spawn.h"    // HasLoadTailQuiesced
#include "ue_wrap/engine.h"            // GetActorLocation
#include "ue_wrap/hot_path_guard.h"    // UE_ASSERT_GAME_THREAD
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"              // GetKeyString
#include "ue_wrap/reflection.h"

namespace coop::kerfur_census {

namespace R = ue_wrap::reflection;

namespace {

bool g_censusDone = false;

// actor -> host-allocated ElementId, built from a mirror-manager snapshot. An actor
// present here is a host-driven MIRROR (the reconcile bound it); absent = a local form
// the reconcile never claimed.
template <typename T>
std::unordered_map<void*, coop::element::ElementId> BuildMirrorMap() {
    std::unordered_map<void*, coop::element::ElementId> m;
    std::vector<T*> v;
    coop::element::MirrorManager<T>::Instance().Snapshot(v);
    for (T* e : v)
        if (e && e->GetActor()) m[e->GetActor()] = e->GetId();
    return m;
}

}  // namespace

void Reset() { g_censusDone = false; }

void TickOnceAtQuiescence() {
    UE_ASSERT_GAME_THREAD("kerfur_census::TickOnceAtQuiescence");
    if (g_censusDone) return;
    auto* s = coop::npc_sync::GetSession();
    if (!s || s->role() == coop::net::Role::Host) return;          // client-only
    if (!coop::remote_prop_spawn::HasLoadTailQuiesced()) return;   // capture the FINAL state
    g_censusDone = true;

    const auto npcMirror  = BuildMirrorMap<coop::element::Npc>();
    const auto propMirror = BuildMirrorMap<coop::element::Prop>();

    UE_LOGW("[KERFUR CENSUS] client load tail quiesced -- enumerating every live kerfur form "
            "(NPC active + prop off) with bound status:");

    int liveNpc = 0, untrackedNpc = 0, liveProp = 0, unclaimedProp = 0;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        if (!cls || !coop::kerfur_entity::IsKerfurClass(cls)) continue;  // any kerfur form (NPC or prop)
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;   // CDO
        const bool isProp = coop::kerfur_entity::IsKerfurPropClass(cls);
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(obj);
        const std::wstring leaf = R::ClassNameOf(obj);
        if (isProp) {
            ++liveProp;
            const std::wstring key = ue_wrap::prop::GetKeyString(obj);
            auto it = propMirror.find(obj);
            const bool bound = (it != propMirror.end());
            if (!bound) ++unclaimedProp;
            std::wstring status = bound
                ? (L"BOUND eid=" + std::to_wstring(it->second))
                : std::wstring(L"*** UNCLAIMED (no host mirror -- stale local off-prop; the host has this "
                               L"kerfur ACTIVE -> identity-key / scope-A side) ***");
            UE_LOGW("[KERFUR CENSUS]   PROP actor=%p class='%ls' key='%ls' pos=(%.1f, %.1f, %.1f) %ls",
                    obj, leaf.c_str(), key.c_str(), loc.X, loc.Y, loc.Z, status.c_str());
        } else {
            ++liveNpc;
            auto it = npcMirror.find(obj);
            const bool bound = (it != npcMirror.end());
            if (!bound) ++untrackedNpc;
            std::wstring status = bound
                ? (L"BOUND eid=" + std::to_wstring(it->second))
                : std::wstring(L"*** UNTRACKED (no host mirror -- stale local active twin the ghost sweep "
                               L"missed -> retire-authority / reverse-sibling side) ***");
            UE_LOGW("[KERFUR CENSUS]   NPC  actor=%p class='%ls' pos=(%.1f, %.1f, %.1f) %ls",
                    obj, leaf.c_str(), loc.X, loc.Y, loc.Z, status.c_str());
        }
    }
    UE_LOGW("[KERFUR CENSUS] TOTAL %d live NPC (%d UNTRACKED) + %d live PROP (%d UNCLAIMED). "
            "Host = 2 active + 4 off = 6; ANY untracked-NPC or unclaimed-PROP line above is the silent dup half: "
            "UNCLAIMED PROP -> identity-key (scope A); UNTRACKED NPC -> retire-side (reverse sibling).",
            liveNpc, untrackedNpc, liveProp, unclaimedProp);
}

}  // namespace coop::kerfur_census
