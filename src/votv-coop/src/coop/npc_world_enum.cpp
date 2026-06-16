// coop/npc_world_enum.cpp -- see header.
//
// Extracted verbatim from npc_sync.cpp (K-0, 2026-06-16); behavior-preserving. The two
// pieces of npc_sync host-side state this walk needs -- the host-sync-disabled gate and the
// live-actor -> ElementId reverse-map insert -- now go through npc_sync's public accessors
// (IsHostNpcSyncDisabled / MapActorToNpcId) instead of the file-static globals it used while
// it lived inside npc_sync.cpp. The Npc Element store is the shared MirrorManager<Npc>
// singleton (same one npc_sync.cpp / npc_mirror.cpp / npc_pose_host.cpp wrap). Log strings are
// preserved unchanged so existing diagnostics/asserts keep matching.

#include "coop/npc_world_enum.h"

#include "coop/element/element_deleter.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/npc_sync.h"
#include "ue_wrap/engine.h"   // GetActorLocation / GetActorRotation
#include "ue_wrap/kerfur.h"   // HasSaveKey -- the ConnectEdge savePersisted gate
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <cstdint>
#include <memory>
#include <string>

namespace coop::npc_world_enum {
namespace {

namespace R = ue_wrap::reflection;

// The single host/mirror Npc set (the same template singleton npc_sync.cpp +
// npc_mirror.cpp + npc_pose_host.cpp wrap; MirrorManager<Npc>::Instance() owns it).
inline coop::element::MirrorManager<coop::element::Npc>& NpcMirrors() {
    return coop::element::MirrorManager<coop::element::Npc>::Instance();
}

}  // namespace

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
            UE_LOGW("npc-sync[world-enum]: AllocAndInstall kInvalidId for '%ls' (Registry full?) -- skipping",
                    clsName.c_str());
            continue;
        }
        // CRIT-2: bind via a null-checked lookup (the POST observer's pattern). If the freshly-alloc'd
        // Element isn't retrievable, DRAIN it back out + do NOT leave a reverse-map entry pointing at
        // an unbound Element (which TickPoseStream/QueueConnectBroadcast would then iterate forever).
        coop::element::Npc* el = NpcMirrors().Get(eid);
        if (!el) {
            UE_LOGW("npc-sync[world-enum]: eid=%u not retrievable after AllocAndInstall -- draining (no bind)", eid);
            coop::element::ElementDeleter::Get().Enqueue(NpcMirrors().Take(eid));
            continue;
        }
        el->SetActor(obj, R::InternalIndexOf(obj));
        coop::npc_sync::MapActorToNpcId(obj, eid);
        ++registered;
        // v67 (kerfur_convert): make the registration VISIBLE now -- broadcast
        // EntitySpawn for the newly-registered NPC to connected peers.
        // BP-internal spawns (EX_CallMath BeginDeferred -- e.g. prop_kerfurOmega.
        // spawnKerfuro's kerfur) never pass the interceptor, so without this a
        // mid-session spawn existed host-only until the next connect edge. At
        // the connect edge itself this duplicates QueueConnectBroadcastForSlot's
        // send for freshly-found NPCs -- harmless: the receiver's OnEntitySpawn
        // drops a duplicate eid early (NpcMirrors().Get(eid) != nullptr -- a
        // logged skip, not a re-install; audit I1 2026-06-12).
        if (s->connected()) {
            coop::net::EntitySpawnPayload p{};
            const std::string& tn = el->GetTypeName();
            p.className.len = 0;
            for (size_t k = 0; k < tn.size() && k < 63; ++k)
                p.className.data[p.className.len++] = tn[k];
            p.elementId = static_cast<uint32_t>(eid);
            const auto loc = ue_wrap::engine::GetActorLocation(obj);
            const auto rot = ue_wrap::engine::GetActorRotation(obj);
            p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
            p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
            // v75: savePersisted flag for client-side class-match ADOPTION -- but gated on the
            // caller's INTENT (origin), not just the actor's has-a-key property (RCA 2026-06-15).
            // ConnectEdge: a JOINER may have loaded this keyed save object itself -> let it adopt its
            // local twin (savePersisted = HasSaveKey). MidSessionConverge: a kerfur turned ON
            // mid-session reaches ALREADY-CONNECTED peers who have NO twin (they cancelled their own
            // local turn-on) -> ALWAYS 0 -> fresh-spawn now. (K-1: this MidSessionConverge=>0 rule is
            // the immediate-relief fix; it stays under the redesign.) See the header.
            p.savePersisted =
                (origin == NpcEnumOrigin::ConnectEdge && ue_wrap::kerfur::HasSaveKey(obj)) ? 1 : 0;
            if (!s->SendEntitySpawn(p)) {
                UE_LOGW("npc-sync[world-enum]: SendEntitySpawn failed for newly-registered eid=%u",
                        p.elementId);
            }
        }
    }
    if (found > 0)
        UE_LOGI("npc-sync[world-enum]: registered %d pre-existing world NPC(s) "
                "(%d allowlisted instance(s) in world, %d already tracked; scanned %d GUObjectArray slots)",
                registered, found, alreadyTracked, n);
    return registered;
}

}  // namespace coop::npc_world_enum
