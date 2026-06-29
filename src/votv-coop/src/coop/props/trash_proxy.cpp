// coop/trash_proxy.cpp -- see coop/trash_proxy.h.

#include "coop/props/trash_proxy.h"

#include "coop/element/element_deleter.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/prop.h"
#include "coop/props/remote_prop.h"       // ClearAnyDriveFor (evict the pose drive before destroying a proxy)
#include "coop/props/trash_clump_pose_stream.h"  // v85: evict the per-eid carry drive before destroying a proxy
#include "ue_wrap/engine.h"
#include "ue_wrap/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"            // ResolvePileMesh
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"          // FVector / FRotator

#include <cmath>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::trash_proxy {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

struct ProxyEntry {
    void* actor     = nullptr;
    void* comp      = nullptr;  // cached UStaticMeshComponent (invariant for the actor's life)
    int   ownerSlot = -1;       // originating peer slot (for the per-slot disconnect retire)
    bool  isClump   = false;    // current FORM (re-skinned pile<->clump) -- so NearestPileProxy can skip clumps
};

// eid -> proxy. GAME-THREAD only (every entry point runs on the net-pump game
// thread), so a plain map with no mutex is correct.
std::unordered_map<coop::element::ElementId, ProxyEntry> g_proxies;

void* g_smaClass  = nullptr;  // AStaticMeshActor UClass (the proxy class)
void* g_chipBase  = nullptr;  // actorChipPile_C     (class-kind base)
void* g_clumpBase = nullptr;  // prop_garbageClump_C (class-kind base)

// v83: apply the host's per-form scale to the proxy. An AStaticMeshActor defaults to unit
// scale, so without this the proxy rendered SMALLER than the host's real pile/clump. The
// comparison guard rejects a degenerate scale (zero OR NaN -- NaN > x is false) so a malformed
// packet can never collapse the mirror invisibly; a v83 sender always sends a real scale.
void ApplyProxyScale(void* actor, const ue_wrap::FVector& scale) {
    if (!actor) return;
    if (scale.X > 0.001f && scale.Y > 0.001f && scale.Z > 0.001f)
        E::SetActorScale3D(actor, scale);
}

// (RETIRED 2026-06-23, RULE 2: the phase-2a "give the pile-form proxy QueryOnly collision so the game's
// interaction trace hits it, read lookatActorCurrent" approach was DISPROVEN by the harness -- the trace
// gate logged hit=0: the worn pile mesh has no simple collision body, so SetCollisionEnabled on the
// proxy's StaticMeshComponent is a no-op for a bTraceComplex=false trace. Recognition is now a camera-ray
// math cone -- EidForAimedPileProxy below -- which needs NO asset collision. Proxies stay NoCollision (the
// player passing through a mirrored pile is the pre-existing phase-1 regression; the FAITHFUL fix for both
// movement-block AND occlusion-correct aim is a future garbageCollider-analog SHAPE component on the proxy.)

// Cached per-className class-kind: IsTrashProxyClass runs once per PropSpawn (a
// ~2000-prop join burst) and FindClass is a ~237k-entry GUObjectArray walk -- the
// cache makes only the ~dozen distinct class names ever walk. Folds IsClumpClass
// into the SAME lookup (one FindClass per class, not two). GT-serial -> no mutex.
struct ClassKind { bool isTrash = false; bool isClump = false; };
std::unordered_map<std::wstring, ClassKind> g_classKind;

ClassKind ResolveClassKind(const std::wstring& className) {
    if (className.empty()) return {};
    if (auto it = g_classKind.find(className); it != g_classKind.end()) return it->second;
    if (!g_chipBase)  g_chipBase  = R::FindClass(L"actorChipPile_C");
    if (!g_clumpBase) g_clumpBase = R::FindClass(L"prop_garbageClump_C");
    void* cls = R::FindClass(className.c_str());
    if (!cls) return {};  // class not loaded yet -- do NOT cache (it could load later)
    ClassKind k;
    void* const trashBases[2] = { g_chipBase, g_clumpBase };
    k.isTrash = R::IsDescendantOfAny(cls, trashBases, 2);
    if (k.isTrash) {
        void* const clumpBase[1] = { g_clumpBase };
        k.isClump = R::IsDescendantOfAny(cls, clumpBase, 1);
    }
    g_classKind[className] = k;
    return k;
}

// The fixed dirtball clump mesh (cached). May be null if no clump asset is
// resident yet on this client -- the caller then falls back to the pile mesh so a
// proxy is never invisible.
void* ResolveClumpMesh() {
    static void* sDirtball = nullptr;
    if (!sDirtball || !R::IsLive(sDirtball)) sDirtball = R::FindObject(L"dirtball", L"StaticMesh");
    return sDirtball;
}

// MEDIUM-1 last-ditch: the engine basic Cube, so a proxy is NEVER invisible even if
// both the dirtball clump mesh and the chipType pile mesh fail to resolve. In
// practice unreachable -- a trash class being loaded (we resolved IsTrashProxyClass
// against it) pins its referenced meshes resident, and ResolvePileMesh last-good-
// caches -- so a full native StaticLoadObject primitive is not warranted; FindObject
// (resident) is the proportionate guard. Last-good cached.
void* ResolveCubeFallback() {
    static void* sCube = nullptr;
    if (!sCube || !R::IsLive(sCube)) sCube = R::FindObject(L"Cube", L"StaticMesh");
    return sCube;
}

// Skin the proxy's StaticMeshComponent for the requested form. SetStaticMesh
// recomputes the component's bounds + collision body in the SAME call (atomic --
// no window where the visual is the new form but the bound is the old). `worldCtx`
// is a live UObject for getChipPileType's WorldContext (the proxy actor itself).
//
// PILE  = the chipType pile mesh directly (its own slot-0 material is correct). We
//         re-skin ONE shared component back and forth (unlike the game's separate
//         actors), so CLEAR any leftover clump material override: SetMaterial(0,null)
//         reverts slot 0 to the mesh asset default.
// CLUMP = the FIXED dirtball mesh + the pile mesh's slot-0 MATERIAL. This is the
//         VERIFIED game behavior (prop_garbageClump_C::setTex bytecode = SetMaterial(
//         0, getChipPileType(chipType).GetMaterial(0)) on the fixed StaticMesh -- a
//         MATERIAL swap, NOT a mesh swap). Mesh fallback chain dirtball -> pile mesh
//         -> Cube so the clump is never invisible.
void SkinProxy(void* worldCtx, void* comp, uint8_t chipType, bool isClump) {
    if (!comp) return;
    void* pileMesh = ue_wrap::prop::ResolvePileMesh(chipType, worldCtx);  // last-good cached
    if (isClump) {
        void* mesh = ResolveClumpMesh();
        const char* meshSrc = "dirtball";
        if (!mesh) { mesh = pileMesh;            meshSrc = "PILE-FALLBACK(dirtball UNRESOLVED -> clump renders identical to a pile!)"; }
        if (!mesh) { mesh = ResolveCubeFallback(); meshSrc = "CUBE-FALLBACK"; }
        if (mesh) E::SetStaticMesh(comp, mesh);
        void* mat = pileMesh ? E::GetStaticMeshMaterial(pileMesh, 0) : nullptr;
        E::SetComponentMaterial(comp, 0, mat);  // null -> dirtball default (acceptable fallback)
        // [diag 2026-06-21] WHICH mesh the clump got: if 'PILE-FALLBACK', the ToClump re-skin is a
        // visual no-op (clump looks exactly like the pile) -- the prime suspect for the hands-on
        // "the proxy stays a pile" symptom. Event-driven (once per convert/grab), not a hot path.
        UE_LOGI("[PILE] trash_proxy: SkinProxy CLUMP chipType=%u mesh-src=%s mesh=%p mat=%p",
                static_cast<unsigned>(chipType), meshSrc, mesh, mat);
    } else {
        void* mesh = pileMesh ? pileMesh : ResolveCubeFallback();
        if (mesh) E::SetStaticMesh(comp, mesh);
        E::SetComponentMaterial(comp, 0, nullptr);  // clear any clump override -> pile mesh's own material
    }
}

}  // namespace

bool IsTrashProxyClass(const std::wstring& className) { return ResolveClassKind(className).isTrash; }
bool IsClumpClass(const std::wstring& className)      { return ResolveClassKind(className).isClump; }

void* SpawnProxy(coop::element::ElementId eid, uint8_t chipType, bool isClump, int ownerSlot,
                 const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot, const ue_wrap::FVector& scale) {
    UE_ASSERT_GAME_THREAD("trash_proxy::SpawnProxy");
    if (!g_smaClass) g_smaClass = R::FindClass(L"StaticMeshActor");
    if (!g_smaClass) {
        UE_LOGW("trash_proxy: StaticMeshActor class unresolved -- cannot spawn proxy eid=%u", eid);
        return nullptr;
    }
    // Re-spawn convergence (a re-seed / duplicate / snapshot-bootstrap spawn for an eid
    // we already mirror): return the existing proxy, never a second actor. CRITICALLY,
    // do NOT re-skin here (HIGH-1): the proxy's FORM (pile<->clump) is owned exclusively
    // by the ctx-ordered convert channel (initial SpawnProxy form + ReskinProxy). A
    // trailing/stale PropSpawn carries the spawn CLASS, which for a pile that the host
    // has since grabbed is the chipPile class (isClump=false) -- re-skinning on it would
    // flip a correctly-converted CLUMP back to a PILE. Form changes ride PropConvert only.
    if (auto it = g_proxies.find(eid);
        it != g_proxies.end() && it->second.actor && R::IsLive(it->second.actor)) {
        it->second.ownerSlot = ownerSlot;  // a re-bracket may re-stamp the owner
        return it->second.actor;
    }
    void* actor = E::SpawnActor(g_smaClass, loc, /*inertPawn=*/false);
    if (!actor) {
        UE_LOGW("trash_proxy: SpawnActor(StaticMeshActor) failed eid=%u", eid);
        return nullptr;
    }
    void* comp = E::GetStaticMeshComponent(actor);             // resolve ONCE (invariant for the actor's life)
    // AStaticMeshActor defaults to STATIC mobility -> at runtime SetStaticMesh AND SetActorLocation
    // are silently no-ops on a Static component (AreDynamicDataChangesAllowed()==false): the proxy
    // would be INVISIBLE (mesh never applies) and unable to follow the carry, while the convert/throw
    // SOUNDS still fire (the events process). The proxy is a kinematic host-driven follower we re-skin
    // + move every frame, so make it Movable BEFORE the first skin/transform. (Caught hands-on
    // 2026-06-21: "client hears interaction + throw sounds but no visual mirror" -- the autonomous
    // smoke can't see this, screenshots are black + it only checks log markers.)
    E::SetComponentMobility(comp, /*EComponentMobility::Movable=*/2);
    E::SetActorRotation(actor, rot);
    R::AddToRoot(actor);                                        // never GC'd -> never stale -> no dup
    E::SetActorRootCollisionEnabled(actor, /*ECollisionEnabled::NoCollision=*/0);  // kinematic follower (aim-grab is a camera-ray cone, not collision)
    SkinProxy(actor, comp, chipType, isClump);
    ApplyProxyScale(actor, scale);                              // v83: host-sized (else default unit -> too small)
    g_proxies[eid] = ProxyEntry{ actor, comp, ownerSlot, isClump };
    UE_LOGI("[PILE] trash_proxy: SPAWN eid=%u %s chipType=%u actor=%p ownerSlot=%d "
            "(AStaticMeshActor, rooted, NoCollision)",
            eid, isClump ? "clump" : "pile", static_cast<unsigned>(chipType), actor, ownerSlot);
    return actor;
}

void* ReskinProxy(coop::element::ElementId eid, uint8_t chipType, bool isClump, const ue_wrap::FVector& scale) {
    UE_ASSERT_GAME_THREAD("trash_proxy::ReskinProxy");
    auto it = g_proxies.find(eid);
    if (it == g_proxies.end()) return nullptr;
    void* actor = it->second.actor;
    if (!actor || !R::IsLive(actor)) return nullptr;  // a rooted proxy is never stale -- defensive
    SkinProxy(actor, it->second.comp, chipType, isClump);  // in place -> binding untouched -> no dup
    ApplyProxyScale(actor, scale);                         // v83: re-apply the per-form scale (clump != pile size)
    it->second.isClump = isClump;                          // track the new form (NearestPileProxy skips clumps)
    return actor;
}

void RetireProxy(coop::element::ElementId eid) {
    UE_ASSERT_GAME_THREAD("trash_proxy::RetireProxy");
    auto it = g_proxies.find(eid);
    if (it == g_proxies.end()) return;
    void* actor = it->second.actor;
    g_proxies.erase(it);
    // ClearAnyDriveFor -> Destroy -> RemoveFromRoot -> unbind. Evict the drive FIRST
    // so neither remote_prop::Tick nor ForceRelease touches the actor we destroy (a
    // stale g_drives entry to a freed actor would UAF). GC runs on the game thread
    // and cannot interleave this synchronous sequence, so the "un-rooted, not-yet-
    // destroyed" window is moot; destroy marks PendingKill, then un-root makes the
    // memory GC-reapable (a rooted PendingKill actor would never be reaped = leak).
    coop::remote_prop::ClearAnyDriveFor(actor);
    coop::trash_clump_pose_stream::ClearDriveForEid(eid);  // v85: drop the host-auth per-eid carry drive too
    if (actor && R::IsLive(actor)) {
        E::DestroyActor(actor);
        R::RemoveFromRoot(actor);
    }
    // Unbind the Prop mirror (deferred dtor outside the manager mutex -- the
    // documented teardown pattern; ~Prop -> ~Element -> Registry::UnregisterMirror).
    coop::element::ElementDeleter::Get().Enqueue(
        coop::element::MirrorManager<coop::element::Prop>::Instance().Take(eid));
    UE_LOGI("[PILE] trash_proxy: RETIRE eid=%u actor=%p (drive-evicted, destroyed, un-rooted, unbound)",
            eid, actor);
}

bool IsProxy(coop::element::ElementId eid) {
    return g_proxies.find(eid) != g_proxies.end();
}

void* NearestPileProxy(const ue_wrap::FVector& fromLoc, float* outDistCm) {
    UE_ASSERT_GAME_THREAD("trash_proxy::NearestPileProxy");
    void* best = nullptr;
    float best2 = -1.f;
    for (const auto& kv : g_proxies) {
        const ProxyEntry& e = kv.second;
        if (e.isClump) continue;                          // want a PILE form (a clump is the transient carry)
        if (!e.actor || !R::IsLive(e.actor)) continue;
        const ue_wrap::FVector p = E::GetActorLocation(e.actor);
        const float dx = p.X - fromLoc.X, dy = p.Y - fromLoc.Y, dz = p.Z - fromLoc.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (best2 < 0.f || d2 < best2) { best2 = d2; best = e.actor; }
    }
    if (outDistCm) *outDistCm = (best2 >= 0.f) ? std::sqrt(best2) : -1.f;
    return best;
}

void* ProxyActorForEid(coop::element::ElementId eid) {
    UE_ASSERT_GAME_THREAD("trash_proxy::ProxyActorForEid");
    auto it = g_proxies.find(eid);
    if (it == g_proxies.end()) return nullptr;
    void* actor = it->second.actor;
    return (actor && R::IsLive(actor)) ? actor : nullptr;
}

coop::element::ElementId EidForAimedPileProxy(const ue_wrap::FVector& camLoc, const ue_wrap::FVector& camFwd,
                                             float maxRangeCm, float minDot) {
    UE_ASSERT_GAME_THREAD("trash_proxy::EidForAimedPileProxy");
    // Camera-ray cone: the PILE-form proxy that is within `maxRangeCm` AND most centered on the aim ray
    // (largest dot >= minDot). `camFwd` MUST be unit length. This is the client-grab recognition -- it needs
    // NO asset collision (the trace approach was disproven: the pile mesh has no simple collision body). The
    // host re-validates the eid (live + IsChipPile) in OnGrabIntent, so a cone mis-pick in a dense cluster is
    // bounded. Returns kInvalidId if nothing qualifies.
    coop::element::ElementId best = coop::element::kInvalidId;
    float bestDot = minDot;
    for (const auto& kv : g_proxies) {
        const ProxyEntry& e = kv.second;
        if (e.isClump) continue;                          // a clump-form proxy is the transient carry -- not grabbable
        if (!e.actor || !R::IsLive(e.actor)) continue;
        const ue_wrap::FVector p = E::GetActorLocation(e.actor);
        const float dx = p.X - camLoc.X, dy = p.Y - camLoc.Y, dz = p.Z - camLoc.Z;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > maxRangeCm || dist < 1.f) continue;
        const float dot = (dx * camFwd.X + dy * camFwd.Y + dz * camFwd.Z) / dist;  // cos(angle), camFwd unit
        if (dot > bestDot) { bestDot = dot; best = kv.first; }
    }
    return best;
}

void OnDisconnectForSlot(int slot) {
    UE_ASSERT_GAME_THREAD("trash_proxy::OnDisconnectForSlot");
    // CRITICAL-1 fix: a PER-SLOT disconnect (a single peer dropping while the
    // session stays up) drains that slot's Prop mirrors via DrainMirrorsForSlot,
    // which does NOT call RetireProxy -> the rooted proxy would leak. Retire every
    // proxy owned by `slot` HERE (called before remote_prop::OnDisconnectForSlot),
    // so the actor is un-rooted + destroyed + unbound through g_proxies (the
    // authoritative tracker) before the generic mirror drain runs.
    std::vector<coop::element::ElementId> eids;
    for (const auto& kv : g_proxies)
        if (kv.second.ownerSlot == slot) eids.push_back(kv.first);
    if (eids.empty()) return;
    for (auto eid : eids) RetireProxy(eid);
    UE_LOGI("[PILE] trash_proxy: OnDisconnectForSlot(%d) retired %zu proxy mirror(s)", slot, eids.size());
}

void OnDisconnect() {
    UE_ASSERT_GAME_THREAD("trash_proxy::OnDisconnect");
    if (g_proxies.empty()) return;
    const size_t n = g_proxies.size();
    // Snapshot eids first -- RetireProxy mutates the map.
    std::vector<coop::element::ElementId> eids;
    eids.reserve(n);
    for (const auto& kv : g_proxies) eids.push_back(kv.first);
    for (auto eid : eids) RetireProxy(eid);
    UE_LOGI("[PILE] trash_proxy: OnDisconnect retired %zu proxy mirror(s)", n);
}

}  // namespace coop::trash_proxy
