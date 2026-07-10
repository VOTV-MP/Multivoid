// coop/npc_world_enum.cpp -- see header. HOST-side OFF-INTERCEPTOR NPC enrollment:
// the level-load GUObjectArray walk + the EX_CallMath BeginDeferred spawn catch
// (2026-07-03, the wisp mirror lane). Both funnel into ONE enroll body
// (EnrollUntrackedNpcActor) that reaches the same end state as interceptor+POST.
//
// The two pieces of npc_sync host-side state this needs -- the host-sync-disabled gate and the
// live-actor -> ElementId reverse-map insert -- go through npc_sync's public accessors
// (IsHostNpcSyncDisabled / MapActorToNpcId). The Npc Element store is the shared
// MirrorManager<Npc> singleton (same one npc_sync.cpp / npc_mirror.cpp / npc_pose_host.cpp wrap).
// Log strings of the walk are preserved unchanged so existing diagnostics/asserts keep matching.

#include "coop/creatures/npc_world_enum.h"

#include "coop/element/element_deleter.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/mirror_managers.h"  // PropMirrors/NpcMirrors/WaMirrors
#include "coop/element/npc.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/creatures/kerfur_entity.h"  // K-3: reserve the stable KerfurId when a kerfur NPC is registered
#include "coop/creatures/npc_sync.h"
#include "coop/world/world_actor_sync.h"  // HostEnrollExSpawn -- the WA branch of the EX-catch drain
#include "ue_wrap/engine.h"   // GetActorLocation / GetActorRotation
#include "ue_wrap/kerfur.h"   // HasSaveKey -- the ConnectEdge savePersisted gate
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"      // NpcClass_Wisp (the ambient-wisp walk skip)
#include "ue_wrap/ufunction_hook.h"   // the EX_CallMath spawn-catch thunk

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace coop::npc_world_enum {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

// The single host/mirror Npc set (the same template singleton npc_sync.cpp +
// npc_mirror.cpp + npc_pose_host.cpp wrap; MirrorManager<Npc>::Instance() owns it).
using coop::element::NpcMirrors;   // canonical accessor (coop/element/mirror_managers.h)

// ---- the ONE enroll body ---------------------------------------------------------------
// Alloc + bind + reverse-map [+ KerfurId] + EntitySpawn broadcast (when connected) for an
// UNTRACKED live NPC actor -- the same end state the interceptor+POST reach for a fresh
// spawn. Returns the eid, or kInvalidId on failure (logged). Caller has already verified:
// live, allowlisted class, not tracked. Game thread.
coop::element::ElementId EnrollUntrackedNpcActor(void* obj, const std::wstring& clsName,
                                                 bool savePersisted, const char* logTag) {
    auto* s = coop::npc_sync::GetSession();
    if (!s) return coop::element::kInvalidId;
    auto npc = std::make_unique<coop::element::Npc>();
    std::string typeName8;
    for (size_t k = 0; k < clsName.size() && k < 63; ++k)
        typeName8.push_back(static_cast<char>(clsName[k]));
    npc->SetTypeName(std::move(typeName8));
    // AllocAndInstall takes the Registry mutex then the type mutex (host-authoritative order);
    // the reverse-map insert (MapActorToNpcId) is a LEAF taken separately AFTER, never nested.
    const coop::element::ElementId eid =
        NpcMirrors().AllocAndInstall(std::move(npc), /*isHost=*/true);
    if (eid == coop::element::kInvalidId) {
        UE_LOGW("npc-sync[%s]: AllocAndInstall kInvalidId for '%ls' (Registry full?) -- skipping",
                logTag, clsName.c_str());
        return coop::element::kInvalidId;
    }
    // CRIT-2: bind via a null-checked lookup (the POST observer's pattern). If the freshly-alloc'd
    // Element isn't retrievable, DRAIN it back out + do NOT leave a reverse-map entry pointing at
    // an unbound Element (which TickPoseStream/QueueConnectBroadcast would then iterate forever).
    coop::element::Npc* el = NpcMirrors().Get(eid);
    if (!el) {
        UE_LOGW("npc-sync[%s]: eid=%u not retrievable after AllocAndInstall -- draining (no bind)",
                logTag, eid);
        coop::element::ElementDeleter::Get().Enqueue(NpcMirrors().Take(eid));
        return coop::element::kInvalidId;
    }
    el->SetActor(obj, R::InternalIndexOf(obj));
    coop::npc_sync::MapActorToNpcId(obj, eid);
    // K-3 (kerfur redesign): a kerfur NPC also gets a stable host-range KerfurId reserved in the
    // KerfurEntity table (host authority, idempotent per actor).
    if (clsName.find(L"kerfurOmega") != std::wstring::npos) {
        coop::kerfur_entity::AllocKerfurId(obj, eid, coop::kerfur_entity::Form::Npc, clsName);
    }
    // v67 (kerfur_convert): make the registration VISIBLE now -- broadcast EntitySpawn for the
    // newly-registered NPC to connected peers. At the connect edge itself this duplicates
    // QueueConnectBroadcastForSlot's send for freshly-found NPCs -- harmless: the receiver's
    // OnEntitySpawn drops a duplicate eid early (a logged skip, not a re-install; audit I1).
    if (s->connected()) {
        coop::net::EntitySpawnPayload p{};
        const std::string& tn = el->GetTypeName();
        p.className.len = 0;
        for (size_t k = 0; k < tn.size() && k < 63; ++k)
            p.className.data[p.className.len++] = tn[k];
        p.elementId = static_cast<uint32_t>(eid);
        const auto loc = ue_wrap::engine::GetActorLocation(obj);
        const auto rot = ue_wrap::engine::GetActorRotation(obj);
        const auto scl = ue_wrap::engine::GetActorScale3D(obj);  // v99: mirror at true size
        p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
        p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
        p.scaleX = scl.X; p.scaleY = scl.Y; p.scaleZ = scl.Z;
        p.savePersisted = savePersisted ? 1 : 0;
        // scope A (v91 deterministic): carry the off->active dup RETIRE key for a window-turned-ON
        // kerfur (see npc_pose_host.cpp). Stamps for ANY origin -- harmless when there is no
        // off-prop mirror bound at that eid (the eid-keyed sweep finds nothing).
        const coop::element::ElementId offEid = coop::kerfur_entity::GetOriginOffEidForEid(eid);
        p.retireOffEid = (offEid == coop::element::kInvalidId) ? 0u : static_cast<uint32_t>(offEid);
        // v91 deterministic turn-on ghost adopt: carry the eid this kerfur converted FROM (the
        // initiator peer parked a ghost tagged with it). kInvalidId -> 0 -> no eid ghost adopt.
        const coop::element::ElementId fromEid = coop::kerfur_entity::GetConvertFromEidForEid(eid);
        p.convertFromEid = (fromEid == coop::element::kInvalidId) ? 0u : static_cast<uint32_t>(fromEid);
        if (!s->SendEntitySpawn(p)) {
            UE_LOGW("npc-sync[%s]: SendEntitySpawn failed for newly-registered eid=%u",
                    logTag, p.elementId);
        }
    }
    return eid;
}

// ---- the EX_CallMath spawn catch -------------------------------------------------------
// Source spawner classes whose BeginDeferred output is HOST-AUTHORITATIVE (mirrored). An
// EX_CallMath spawn from any OTHER source (the ambient ticker_wispSpawner, per-peer decor)
// is deliberately NOT enrolled -- the same standing per-peer-ambience decision as the
// colored wisp siblings. Future creature events (boars/grays/buster) add their trigger
// class here once their target class joins kNpcAllowlist.
constexpr const wchar_t* kExSpawnSourceClasses[] = {
    L"trigger_wispSwarm_C",   // the `wisps` event swarm -> up to 32x wisp_C over ~8-32 s
    L"piramidSpawner_C",      // the `piramid` event chain -> 4x killerwisp_C (npc lane) +
                              // 1x piramid2_C (WorldActor lane) -- runTrigger's BeginDeferred
                              // is EX_CallMath (proven 2026-07-04: zero interceptor catches on
                              // a live force run while the registry probe saw the BeginPlay)
};

// A source's output may be a WorldActor-lane class (piramid2_C): same catch seam, drained to
// world_actor_sync::HostEnrollExSpawn instead of the Npc enroll. Same NameEquals walk
// world_actor_sync itself uses (CRT-alloc-free; each compare renders the FName through the
// engine scratch) -- sits strictly BEHIND the source-class match, ambient spawns never reach it.
bool IsWaAllowlistedClass(void* cls) {
    if (!cls) return false;
    const auto& nm = R::NameOf(cls);
    for (size_t i = 0; i < P::name::kWorldActorAllowlistSize; ++i)
        if (R::NameEquals(nm, P::name::kWorldActorAllowlist[i])) return true;
    return false;
}

// Queue entries carry the internal index CAPTURED AT CATCH TIME so the drain validates with
// IsLiveByIndex -- a raw pointer cached across a tick boundary is the documented AV/recycle
// hazard class (reflection.h; the 2026-05-30 connect-edge precedent). Cleared on disconnect.
struct PendingExSpawn { void* actor; int32_t internalIdx; };
std::mutex g_pendingMx;
std::vector<PendingExSpawn> g_pendingExSpawns;        // queued actors awaiting the GT drain
constexpr size_t kMaxPendingExSpawns = 256;           // sanity cap (a swarm is 32)

// ufunction_hook post-native callback: fires for EVERY BeginDeferredActorSpawnFromClass
// dispatch (PE-visible AND EX_CallMath), DEEP inside the engine spawn -- keep it cheap.
// Cheapest-first gating (perf audit 2026-07-03): the ~ns atomic session/role/lifecycle gates
// run BEFORE the source-class FName compare (~100-200 ns render) -- a client exits in
// nanoseconds for every ambient spawn in the game. Fires PRE-Finish, so it only queues;
// the drain reads the real transform next pump tick.
//
// HOSTING-gated, NOT connected-gated (RULE 1 root fix 2026-07-05, the 0s hands-on failure):
// an event that fires while the host is ALONE (pyramid walking before the first client
// joins) must still ENROLL its actors -- identity/tracking is connection-independent; only
// the SEND is peer-dependent, and the join connect-snapshot replays tracked state. The old
// `!connected()` gate here silently dropped the piramid2_C + killerwisp catches, so the
// joiner's WA/npc snapshots had NOTHING and the client saw an empty world mid-event.
void OnBeginDeferredExSpawn(void* /*context*/, void* srcObj, void* spawned) {
    if (!spawned || !srcObj) return;
    auto* s = coop::npc_sync::GetSession();
    if (!s || s->role() != coop::net::Role::Host) return;
    // IsInstalled is the "lifecycle armed" proxy for BOTH lanes (they Install together at
    // StartCoopSession). The npc-specific IsHostNpcSyncDisabled gate moved to the drain's NPC
    // branch (2026-07-04) so a degraded npc lifecycle can't silently drop WorldActor catches.
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
    // Idempotent per (ufunction, cb) inside InstallPostHook; chains after any other Func-thunk
    // on the same UFunction (trash_collect's ambient-prop observer) -- both callbacks fire.
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
        // Index-paired liveness: the catch-time internal index makes a recycled slot read DEAD
        // instead of validating a different object at the same address (reflection.h pattern).
        if (!obj || !R::IsLiveByIndex(obj, e.internalIdx)) continue;  // died before Finish / recycled
        void* cls = R::ClassOf(obj);
        if (!cls) continue;
        if (!coop::npc_sync::IsAllowlistedClass(cls)) {
            // WorldActor-lane output (piramid2_C): hand to world_actor_sync's own enroll (it
            // re-gates on its allowlist/lifecycle + dedups via its reverse map + broadcasts).
            if (IsWaAllowlistedClass(cls))
                coop::world_actor_sync::HostEnrollExSpawn(obj);
            continue;
        }
        // npc-specific lifecycle gate (moved out of the catch 2026-07-04): without a working
        // destroy observer an Npc Element would leak -- skip the enroll, not the WA branch above.
        if (coop::npc_sync::IsHostNpcSyncDisabled()) continue;
        // Dedup vs the interceptor+POST path: a PE-dispatched spawn that ALSO matched a source
        // class was already allocated by the PRE + bound by the POST (both ran before this drain).
        if (coop::npc_sync::GetNpcIdForActor(obj) != coop::element::kInvalidId) continue;
        const std::wstring clsName = R::ToString(R::NameOf(cls));
        // savePersisted=0: an event-swarm spawn happens after any join; no peer has a local twin.
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
    // Disconnect edge: entries queued in the disconnecting tick must never survive into the
    // next session (a stale address could recycle into an unrelated live actor and pass the
    // liveness gate). Called from npc_sync::OnDisconnect.
    std::lock_guard<std::mutex> lk(g_pendingMx);
    g_pendingExSpawns.clear();
}

int RegisterExistingWorldNpcs(NpcEnumOrigin origin) {
    // HOST-only: register pre-existing/level-load NPC actors so the connect-snapshot mirrors them.
    // See npc_world_enum.h. Reaches the SAME end state as interceptor+POST for a fresh spawn (Npc
    // Element alloc + bound live actor + reverse-map entry) but for an actor that loaded with the level.
    auto* s = coop::npc_sync::GetSession();
    if (!s || s->role() != coop::net::Role::Host) return 0;
    // CRIT-1/HIGH-1 (world-enum audit): only register when the host NPC lifecycle is FULLY armed --
    // the interceptor + POST + K2_DestroyActor PRE observers installed (IsInstalled) AND the host
    // broadcast path was NOT disabled (IsHostNpcSyncDisabled, set when the observer table is full).
    // This is the SAME gate the interceptor uses: allocating an Npc Element without a guaranteed
    // destroy observer would leak it + drain the host-id free-stack (RULE 4 / RULE 1). Before Install
    // completes the allowlist is also unresolved, so this would no-op anyway.
    if (!coop::npc_sync::IsInstalled() || coop::npc_sync::IsHostNpcSyncDisabled()) return 0;

    const int32_t n = R::NumObjects();
    int registered = 0, found = 0, alreadyTracked = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        // Fast filter FIRST (matches prop.cpp's connect-edge walk): the subclass-aware allowlist
        // check is a few pointer compares over the SuperStruct chain -- most of GUObjectArray
        // (>99% of ~237k slots) isn't an NPC and never pays for the wstring allocs below.
        void* cls = R::ClassOf(obj);
        if (!cls || !coop::npc_sync::IsAllowlistedClass(cls)) continue;
        // MED-1: GC-liveness guard BEFORE the wstring allocs (skip a purged-but-not-yet-reaped slot
        // without paying for a NameOf alloc; also required before InternalIndexOf reads obj's mem).
        if (!R::IsLive(obj)) continue;
        // Skip CDOs (Default__<Class>) -- the class template, not a world instance.
        const std::wstring objName = R::ToString(R::NameOf(obj));
        if (objName.rfind(L"Default__", 0) == 0) continue;
        ++found;
        // Skip actors already coop-tracked (interceptor/dev-spawned this session, or a prior connect
        // re-scan). The reverse map is the O(1) gate -- without it we'd double-register every NPC.
        if (coop::npc_sync::GetNpcIdForActor(obj) != coop::element::kInvalidId) { ++alreadyTracked; continue; }
        const std::wstring clsName = R::ToString(R::NameOf(cls));
        // 2026-07-03 wisp scope gate: an UNTRACKED wisp_C here is AMBIENT ticker output (the
        // event-swarm wisps enroll at spawn via the EX-catch and are tracked already) -- ambient
        // wisps are per-peer local by design (the colored-sibling decision; sdk_profile.h). A
        // joiner reaches tracked swarm wisps via QueueConnectBroadcastForSlot, not this walk.
        if (clsName == P::name::NpcClass_Wisp) continue;
        const coop::element::ElementId eid = EnrollUntrackedNpcActor(
            obj, clsName,
            // v75 savePersisted policy by ORIGIN (see npc_world_enum.h): ConnectEdge -> a joiner
            // may have loaded this keyed save object itself -> adopt (HasSaveKey);
            // MidSessionConverge -> already-connected peers have no twin -> ALWAYS fresh-spawn.
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
