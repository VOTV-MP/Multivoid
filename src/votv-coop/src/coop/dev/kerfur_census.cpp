// kerfur_census -- see header. Read-only diagnostic; logs every live kerfur form on THIS peer.
#include "coop/dev/kerfur_census.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/element/prop.h"
#include "coop/creatures/kerfur_entity.h"        // IsKerfurClass / IsKerfurPropClass
#include "coop/net/session.h"          // Session::role() + net::Role (complete type)
#include "coop/creatures/npc_sync.h"             // GetSession
#include "coop/props/remote_prop_spawn.h"    // HasLoadTailQuiesced
#include "coop/props/join_membership_sweep.h"  // anti-smear 2026-06-30: claim+sweep extracted out of remote_prop_spawn
#include "coop/config/config.h"             // IsIniKeyTrue
#include "ue_wrap/engine.h"            // GetActorLocation
#include "ue_wrap/hot_path_guard.h"    // UE_ASSERT_GAME_THREAD
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"              // GetKeyString
#include "ue_wrap/reflection.h"

namespace coop::kerfur_census {

namespace R = ue_wrap::reflection;

namespace {

bool g_oneShotDone = false;        // default-mode (client one-shot at quiescence) latch
uint32_t g_periodicCtr = 0;        // [dev] kerfur_census=1 periodic throttle (per-peer call counter)
constexpr uint32_t kPeriodicEveryCalls = 600;  // ~10s at 60 fps (the tick fires per frame)

bool PeriodicEnabled() {
    static const bool s = coop::config::IsIniKeyTrue("kerfur_census");
    return s;
}

// actor -> host-allocated ElementId, from a mirror-manager snapshot. Present => host-driven MIRROR
// (the reconcile bound it); absent => a local form the reconcile never claimed. Only meaningful on
// the CLIENT (the host's own kerfurs are LOCAL, not mirrors -- on the host every actor is "absent").
template <typename T>
std::unordered_map<void*, coop::element::ElementId> BuildMirrorMap() {
    std::unordered_map<void*, coop::element::ElementId> m;
    std::vector<T*> v;
    coop::element::MirrorManager<T>::Instance().Snapshot(v);
    for (T* e : v)
        if (e && e->GetActor()) m[e->GetActor()] = e->GetId();
    return m;
}

// The actual census: enumerate every live kerfur form on THIS peer + log a per-form line + TOTAL.
// `isHost` switches the semantics of the per-form line (host: LOCAL count; client: bound vs unclaimed).
void EmitCensus(bool isHost) {
    const char* role = isHost ? "HOST" : "CLIENT";
    const auto npcMirror  = isHost ? std::unordered_map<void*, coop::element::ElementId>{}
                                   : BuildMirrorMap<coop::element::Npc>();
    const auto propMirror = isHost ? std::unordered_map<void*, coop::element::ElementId>{}
                                   : BuildMirrorMap<coop::element::Prop>();

    UE_LOGW("[KERFUR CENSUS][%s] enumerating every live kerfur form (NPC active + prop off):", role);

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
            std::wstring status;
            if (isHost) {
                status = L"LOCAL (host-authoritative off-prop)";
            } else {
                auto it = propMirror.find(obj);
                const bool bound = (it != propMirror.end());
                if (!bound) ++unclaimedProp;
                status = bound ? (L"BOUND eid=" + std::to_wstring(it->second))
                               : std::wstring(L"*** UNCLAIMED (no host mirror -- stale local off-prop) ***");
            }
            UE_LOGW("[KERFUR CENSUS][%s]   PROP actor=%p class='%ls' key='%ls' pos=(%.1f, %.1f, %.1f) %ls",
                    role, obj, leaf.c_str(), key.c_str(), loc.X, loc.Y, loc.Z, status.c_str());
        } else {
            ++liveNpc;
            std::wstring status;
            if (isHost) {
                status = L"LOCAL (host-authoritative active NPC)";
            } else {
                auto it = npcMirror.find(obj);
                const bool bound = (it != npcMirror.end());
                if (!bound) ++untrackedNpc;
                status = bound ? (L"BOUND eid=" + std::to_wstring(it->second))
                               : std::wstring(L"*** UNTRACKED (no host mirror -- stale local active twin) ***");
            }
            UE_LOGW("[KERFUR CENSUS][%s]   NPC  actor=%p class='%ls' pos=(%.1f, %.1f, %.1f) %ls",
                    role, obj, leaf.c_str(), loc.X, loc.Y, loc.Z, status.c_str());
        }
    }
    // The REAL measured total for THIS peer (no hardcoded other-peer literal -- diff the two peers' lines).
    if (isHost) {
        UE_LOGW("[KERFUR CENSUS][HOST] TOTAL %d live NPC + %d live PROP = %d kerfur(s) (host-authoritative count "
                "-- diff against the CLIENT census TOTAL to localize a 5-vs-6 desync).",
                liveNpc, liveProp, liveNpc + liveProp);
    } else {
        UE_LOGW("[KERFUR CENSUS][CLIENT] TOTAL %d live NPC (%d UNTRACKED) + %d live PROP (%d UNCLAIMED) = %d "
                "kerfur(s). Any UNTRACKED-NPC / UNCLAIMED-PROP line is a silent dup half (un-reconciled local form).",
                liveNpc, untrackedNpc, liveProp, unclaimedProp, liveNpc + liveProp);
    }
}

}  // namespace

void Reset() { g_oneShotDone = false; g_periodicCtr = 0; }

void Tick() {
    UE_ASSERT_GAME_THREAD("kerfur_census::Tick");
    auto* s = coop::npc_sync::GetSession();
    if (!s) return;
    const bool isHost = (s->role() == coop::net::Role::Host);

    if (PeriodicEnabled()) {
        // [dev] kerfur_census=1: periodic on BOTH peers so a hands-on can diff host vs client after the
        // world settles. Throttle by a per-peer call counter (the tick fires per frame on each peer).
        if (g_periodicCtr++ % kPeriodicEveryCalls != 0) return;
        EmitCensus(isHost);
        return;
    }

    // Default mode: the original CLIENT-only, one-shot-at-quiescence forward-dup diagnostic.
    if (isHost) return;
    if (g_oneShotDone) return;
    if (!coop::join_membership_sweep::HasLoadTailQuiesced()) return;  // capture the FINAL reconciled state
    g_oneShotDone = true;
    EmitCensus(/*isHost=*/false);
}

}  // namespace coop::kerfur_census
