// coop/creatures/npc_world_enum.cpp -- see the header. The host-side off-interceptor NPC
// enrolment: the level-load object-array walk and the EX_CallMath deferred-spawn catch, both
// funnelled into one enrol body that reaches the same end state as the interceptor and POST
// pair. The two pieces of host-side npc_sync state this needs (the host-sync-disabled gate
// and the actor-to-eid reverse-map insert) go through npc_sync's public accessors; the NPC
// Element store is the shared mirror manager singleton. The walk's log strings are unchanged
// so existing diagnostics keep matching.

#include "coop/creatures/npc_world_enum.h"

#include "coop/element/element_deleter.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/mirror_managers.h"  // PropMirrors/NpcMirrors/WaMirrors
#include "coop/element/npc.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/creatures/kerfur_entity.h"  // reserve the stable kerfur id when a kerfur NPC is registered
#include "coop/creatures/npc_sync.h"
#include "coop/world/world_actor_sync.h"  // HostEnrollExSpawn -- the WA branch of the EX-catch drain
#include "ue_wrap/engine/engine.h"   // GetActorLocation / GetActorRotation
#include "ue_wrap/actors/kerfur.h"   // HasSaveKey -- the ConnectEdge savePersisted gate
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"      // NpcClass_Wisp (the ambient-wisp walk skip)
#include "ue_wrap/core/ufunction_hook.h"   // the EX_CallMath spawn-catch thunk

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace coop::npc_world_enum {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

// The single host and mirror NPC set, the same singleton the other NPC files wrap.
using coop::element::NpcMirrors;   // canonical accessor (coop/element/mirror_managers.h)

// The one enrol body: allocate, bind, reverse-map, reserve a kerfur id where applicable, and
// broadcast the spawn when connected, for an untracked live NPC actor; the same end state the
// interceptor and POST reach for a fresh spawn. Returns the eid, or the invalid id on failure
// (logged). The caller has verified live, allowlisted and untracked. Game thread.
coop::element::ElementId EnrollUntrackedNpcActor(void* obj, const std::wstring& clsName,
                                                 bool savePersisted, const char* logTag) {
    auto* s = coop::npc_sync::GetSession();
    if (!s) return coop::element::kInvalidId;
    auto npc = std::make_unique<coop::element::Npc>();
    std::string typeName8;
    for (size_t k = 0; k < clsName.size() && k < 63; ++k)
        typeName8.push_back(static_cast<char>(clsName[k]));
    npc->SetTypeName(std::move(typeName8));
    // The allocation takes the Registry mutex then the type mutex, the host-authoritative order;
    // the reverse-map insert is a leaf taken separately after, never nested.
    const coop::element::ElementId eid =
        NpcMirrors().AllocAndInstall(std::move(npc), /*isHost=*/true);
    if (eid == coop::element::kInvalidId) {
        UE_LOGW("npc-sync[%s]: AllocAndInstall kInvalidId for '%ls' (Registry full?) -- skipping",
                logTag, clsName.c_str());
        return coop::element::kInvalidId;
    }
    // Bind through a null-checked lookup, the POST observer's pattern: if the freshly allocated
    // Element is not retrievable, drain it back out and leave no reverse-map entry pointing at an
    // unbound Element, which the pose stream and the connect broadcast would iterate forever.
    coop::element::Npc* el = NpcMirrors().Get(eid);
    if (!el) {
        UE_LOGW("npc-sync[%s]: eid=%u not retrievable after AllocAndInstall -- draining (no bind)",
                logTag, eid);
        coop::element::ElementDeleter::Get().Enqueue(NpcMirrors().Take(eid));
        return coop::element::kInvalidId;
    }
    el->SetActor(obj, R::InternalIndexOf(obj));
    coop::npc_sync::MapActorToNpcId(obj, eid);
    // A kerfur NPC also gets a stable host-range kerfur id reserved in the kerfur entity table;
    // host authority, idempotent per actor.
    if (clsName.find(L"kerfurOmega") != std::wstring::npos) {
        coop::kerfur_entity::AllocKerfurId(obj, eid, coop::kerfur_entity::Form::Npc, clsName);
    }
    // Make the registration visible now: broadcast the spawn for the newly registered NPC to
    // connected peers. At the connect edge itself this duplicates the connect broadcast's send
    // for freshly found NPCs, harmlessly: the receiver drops a duplicate eid early, a logged skip
    // rather than a re-install.
    if (s->connected()) {
        coop::net::EntitySpawnPayload p{};
        const std::string& tn = el->GetTypeName();
        p.className.len = 0;
        for (size_t k = 0; k < tn.size() && k < 63; ++k)
            p.className.data[p.className.len++] = tn[k];
        p.elementId = static_cast<uint32_t>(eid);
        const auto loc = ue_wrap::engine::GetActorLocation(obj);
        const auto rot = ue_wrap::engine::GetActorRotation(obj);
        const auto scl = ue_wrap::engine::GetActorScale3D(obj);  // mirror at true size
        p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
        p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
        p.scaleX = scl.X; p.scaleY = scl.Y; p.scaleZ = scl.Z;
        p.savePersisted = savePersisted ? 1 : 0;
        // Carry the off-to-active dupe retire key for a window-turned-on kerfur (see
        // npc_pose_host.cpp). Stamped for any origin, harmless when no off-prop mirror is bound at
        // that eid, since the eid-keyed sweep finds nothing.
        const coop::element::ElementId offEid = coop::kerfur_entity::GetOriginOffEidForEid(eid);
        p.retireOffEid = (offEid == coop::element::kInvalidId) ? 0u : static_cast<uint32_t>(offEid);
        // The deterministic turn-on ghost adopt: carry the eid this kerfur converted from (the
        // initiator peer parked a ghost tagged with it); 0 means no eid ghost adopt.
        const coop::element::ElementId fromEid = coop::kerfur_entity::GetConvertFromEidForEid(eid);
        p.convertFromEid = (fromEid == coop::element::kInvalidId) ? 0u : static_cast<uint32_t>(fromEid);
        if (!s->SendEntitySpawn(p)) {
            UE_LOGW("npc-sync[%s]: SendEntitySpawn failed for newly-registered eid=%u",
                    logTag, p.elementId);
        }
    }
    return eid;
}

// The EX_CallMath spawn catch. The source spawner classes whose deferred-spawn output is
// host-authoritative and mirrored: the wisps event swarm (up to 32 wisps); the piramid event
// chain (four killer wisps on the NPC lane and one pyramid on the world-actor lane, a spawn
// the interceptor never sees); the ambient sky-wisp ticker and its colour variants (it
// anchors at absolute map coordinates, not around a player, so the host rolls them and the
// client's ticker is cancelled in the spawn authority); and the sell gun's coin mint, whose
// deferred spawn inside the sell verb is bytecode-internal on the host's own sale and on the
// re-commit of a client's sale alike, so the world-actor lane's interceptor never sees it and
// without this row the coin allowlist entry is inert. A future creature event adds its
// trigger class here once its target class joins the NPC allowlist.
constexpr const wchar_t* kExSpawnSourceClasses[] = {
    L"trigger_wispSwarm_C",   // the wisps event swarm
    L"piramidSpawner_C",      // the piramid event chain
    L"ticker_wispSpawner_C",  // the ambient sky wisps
    L"prop_coingun_C",        // the sell gun's coin mint
};

// A source's output may be a world-actor-lane class (the pyramid): the same catch seam,
// drained to the world-actor sync's own enrol instead of the NPC one. The same name-equality
// walk the world-actor sync uses (allocation-free; each compare renders the name through the
// engine scratch), and it sits strictly behind the source-class match, so ambient spawns
// never reach it.
bool IsWaAllowlistedClass(void* cls) {
    if (!cls) return false;
    const auto& nm = R::NameOf(cls);
    for (size_t i = 0; i < P::name::kWorldActorAllowlistSize; ++i)
        if (R::NameEquals(nm, P::name::kWorldActorAllowlist[i])) return true;
    return false;
}

// Queue entries carry the internal index captured at catch time, so the drain validates with
// the index-paired liveness check; a raw pointer cached across a tick boundary is the recycle
// hazard. Cleared on disconnect.
struct PendingExSpawn { void* actor; int32_t internalIdx; };
std::mutex g_pendingMx;
std::vector<PendingExSpawn> g_pendingExSpawns;        // queued actors awaiting the GT drain
constexpr size_t kMaxPendingExSpawns = 256;           // sanity cap (a swarm is 32)

// The Func-thunk post-native callback: fires for every deferred-spawn dispatch, visible or
// EX_CallMath, deep inside the engine spawn, so it stays cheap. Cheapest gates first: the
// atomic session, role and lifecycle gates run before the source-class name compare, so a
// client exits in nanoseconds for every ambient spawn in the game. It fires before the finish,
// so it only queues; the drain reads the real transform next pump tick. Hosting-gated, not
// connected-gated: an event that fires while the host is alone (the pyramid walking before
// the first client joins) must still enrol its actors, since identity and tracking are
// connection-independent and only the send is peer-dependent, with the join snapshot
// replaying tracked state. A connected gate here silently dropped the pyramid and killer-wisp
// catches, and the joiner saw an empty world mid-event.
void OnBeginDeferredExSpawn(void* /*context*/, void* srcObj, void* spawned) {
    if (!spawned || !srcObj) return;
    auto* s = coop::npc_sync::GetSession();
    if (!s || s->role() != coop::net::Role::Host) return;
    // IsInstalled is the lifecycle-armed proxy for both lanes (they install together at session
    // start). The NPC-specific disabled gate lives in the drain's NPC branch, so a degraded NPC
    // lifecycle cannot silently drop world-actor catches.
    if (!coop::npc_sync::IsInstalled()) return;
    void* srcCls = R::ClassOf(srcObj);
    if (!srcCls) return;
    bool sourceMatch = false;
    for (const wchar_t* name : kExSpawnSourceClasses) {
        if (R::NameEquals(R::NameOf(srcCls), name)) { sourceMatch = true; break; }
    }
    if (!sourceMatch) return;
    void* cls = R::ClassOf(spawned);
    if (!cls) return;
    if (!coop::npc_sync::IsAllowlistedClass(cls) && !IsWaAllowlistedClass(cls)) return;
    const int32_t idx = R::InternalIndexOf(spawned);
    std::lock_guard<std::mutex> lk(g_pendingMx);
    if (g_pendingExSpawns.size() >= kMaxPendingExSpawns) return;  // drain stalled? never grow unbounded
    g_pendingExSpawns.push_back({spawned, idx});
}

}  // namespace

void InstallExSpawnCatch(void* beginDeferredFn) {
    if (!beginDeferredFn) return;
    // Idempotent per UFunction and callback inside the hook install; it chains after any other
    // Func thunk on the same UFunction (the trash collect's ambient-prop observer), and both fire.
    if (ue_wrap::ufunction_hook::InstallPostHook(beginDeferredFn, &OnBeginDeferredExSpawn)) {
        UE_LOGI("npc-sync[ex-spawn]: Func-thunk catch installed on BeginDeferred (source-gated: "
                "trigger_wispSwarm_C -> wisp_C, piramidSpawner_C -> killerwisp_C + piramid2_C; "
                "EX_CallMath spawns now enroll)");
    } else {
        UE_LOGW("npc-sync[ex-spawn]: InstallPostHook FAILED -- EX_CallMath creature spawns will "
                "NOT mirror this session (event-swarm wisps host-only)");
    }
}

void DrainPendingExSpawns() {
    auto* s = coop::npc_sync::GetSession();
    if (!s || s->role() != coop::net::Role::Host) return;
    std::vector<PendingExSpawn> pending;
    {
        std::lock_guard<std::mutex> lk(g_pendingMx);
        if (g_pendingExSpawns.empty()) return;
        pending.swap(g_pendingExSpawns);
    }
    for (const PendingExSpawn& e : pending) {
        void* obj = e.actor;
        // Index-paired liveness: the catch-time internal index makes a recycled slot read dead
        // instead of validating a different object at the same address.
        if (!obj || !R::IsLiveByIndex(obj, e.internalIdx)) continue;  // died before Finish / recycled
        void* cls = R::ClassOf(obj);
        if (!cls) continue;
        if (!coop::npc_sync::IsAllowlistedClass(cls)) {
            // World-actor-lane output (the pyramid): hand it to the world-actor sync's own enrol,
            // which re-gates on its allowlist and lifecycle, dedups through its reverse map and
            // broadcasts.
            if (IsWaAllowlistedClass(cls))
                coop::world_actor_sync::HostEnrollExSpawn(obj);
            continue;
        }
        // The NPC-specific lifecycle gate: without a working destroy observer an NPC Element would
        // leak, so the enrol is skipped, not the world-actor branch above.
        if (coop::npc_sync::IsHostNpcSyncDisabled()) continue;
        // Dedup against the interceptor and POST path: a dispatched spawn that also matched a
        // source class was already allocated by the PRE and bound by the POST, both before this
        // drain.
        if (coop::npc_sync::GetNpcIdForActor(obj) != coop::element::kInvalidId) continue;
        const std::wstring clsName = R::ToString(R::NameOf(cls));
        // Not save-persisted: an event-swarm spawn happens after any join, so no peer has a local
        // twin.
        const coop::element::ElementId eid =
            EnrollUntrackedNpcActor(obj, clsName, /*savePersisted=*/false, "ex-spawn");
        if (eid != coop::element::kInvalidId) {
            const auto loc = ue_wrap::engine::GetActorLocation(obj);
            UE_LOGI("npc-sync[ex-spawn]: enrolled '%ls' eid=%u at (%.0f, %.0f, %.0f) "
                    "(EX_CallMath BeginDeferred, source-gated catch)",
                    clsName.c_str(), eid, loc.X, loc.Y, loc.Z);
        }
    }
}

void ClearPendingExSpawns() {
    // The disconnect edge: entries queued in the disconnecting tick must never survive into the
    // next session, since a stale address could recycle into an unrelated live actor and pass
    // the liveness gate. Called from the npc_sync disconnect.
    std::lock_guard<std::mutex> lk(g_pendingMx);
    g_pendingExSpawns.clear();
}

int RegisterExistingWorldNpcs(NpcEnumOrigin origin) {
    // Host only: register the pre-existing and level-load NPC actors so the connect snapshot
    // mirrors them. See the header. The same end state as the interceptor and POST for a fresh
    // spawn (the Element allocated, the live actor bound, the reverse-map entry), for an actor
    // that loaded with the level.
    auto* s = coop::npc_sync::GetSession();
    if (!s || s->role() != coop::net::Role::Host) return 0;
    // Only when the host NPC lifecycle is fully armed: the interceptor, the POST and the destroy
    // PRE observers installed, and the host broadcast path not disabled (set when the observer
    // table is full). The same gate the interceptor uses: allocating an NPC Element without a
    // guaranteed destroy observer would leak it and drain the host-id free stack. Before the
    // install completes the allowlist is unresolved too, so this would no-op anyway.
    if (!coop::npc_sync::IsInstalled() || coop::npc_sync::IsHostNpcSyncDisabled()) return 0;

    const int32_t n = R::NumObjects();
    int registered = 0, found = 0, alreadyTracked = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        // The fast filter first, matching the prop connect-edge walk: the subclass-aware allowlist
        // check is a few pointer compares over the super chain, and almost all of the object array
        // is not an NPC and never pays for the string allocations below.
        void* cls = R::ClassOf(obj);
        if (!cls || !coop::npc_sync::IsAllowlistedClass(cls)) continue;
        // The liveness guard before the string allocations: skip a purged but not yet reaped slot
        // without paying for a name render; also required before the index read touches the
        // object's memory.
        if (!R::IsLive(obj)) continue;
        // Skip the class-default objects, the class template rather than a world instance.
        const std::wstring objName = R::ToString(R::NameOf(obj));
        if (objName.rfind(L"Default__", 0) == 0) continue;
        ++found;
        // Skip actors already tracked (spawned through the interceptor or the dev path this
        // session, or a prior connect re-scan). The reverse map is the constant-time gate; without
        // it every NPC would be double-registered.
        if (coop::npc_sync::GetNpcIdForActor(obj) != coop::element::kInvalidId) { ++alreadyTracked; continue; }
        const std::wstring clsName = R::ToString(R::NameOf(cls));
        // An untracked plain wisp is left alone here: ticker and swarm output enrols at spawn
        // through the catch, and a joiner reaches tracked wisps through the connect broadcast, not
        // this walk.
        if (clsName == P::name::NpcClass_Wisp) continue;
        const coop::element::ElementId eid = EnrollUntrackedNpcActor(
            obj, clsName,
            // The save-persisted policy by origin (see the header): at the connect edge a joiner
            // may have loaded this keyed save object itself, so adopt when it has a save key; a
            // mid-session converge means already-connected peers have no twin, so always
            // fresh-spawn.
            origin == NpcEnumOrigin::ConnectEdge && ue_wrap::kerfur::HasSaveKey(obj),
            "world-enum");
        if (eid == coop::element::kInvalidId) continue;
        ++registered;
    }
    if (found > 0)
        UE_LOGI("npc-sync[world-enum]: registered %d pre-existing world NPC(s) "
                "(%d allowlisted instance(s) in world, %d already tracked; scanned %d GUObjectArray slots)",
                registered, found, alreadyTracked, n);
    return registered;
}

}  // namespace coop::npc_world_enum
