// coop/props/trash_proxy.cpp -- see coop/props/trash_proxy.h.

#include "coop/props/trash_proxy.h"

#include "coop/element/element_deleter.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/prop.h"
#include "coop/props/remote_prop.h"       // ClearAnyDriveFor (evict the pose drive before destroying a proxy)
#include "coop/props/trash_clump_pose_stream.h"  // evict the per-eid carry drive before destroying a proxy
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/gc_pin.h"
#include "ue_wrap/core/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"            // ResolvePileMesh
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"          // FVector / FRotator

#include <utility>
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
    // A slot-validated reference: rooting makes the actor GC-immune in steady state, but a world
    // teardown still force-kills actors regardless of the root set, so cross-tick probes go
    // through the slot-validated ref, never a bare liveness read of the raw pointer.
    ue_wrap::CachedObjRef actor;
    // The GC pin, owned: erasing the entry releases it, so no teardown path can drop a proxy
    // while leaving its actor rooted. A hand-written unroot guarded on liveness did exactly that
    // at a world teardown, where every proxy reports not alive: hundreds of actors stayed rooted
    // and anchored the departed world through their outer chain, so the world was never
    // collected and the next map load adopted the corpse (see ue_wrap/core/gc_pin.h).
    ue_wrap::GcPin pin;
    void* comp      = nullptr;  // cached UStaticMeshComponent (invariant for the actor's life)
    int   ownerSlot = -1;       // originating peer slot (for the per-slot disconnect retire)
    bool  isClump   = false;    // current FORM (re-skinned pile<->clump) -- so NearestPileProxy can skip clumps
};

// eid to proxy. Game thread only (every entry point runs on the pump's game-thread task), so
// a plain map with no mutex is correct.
std::unordered_map<coop::element::ElementId, ProxyEntry> g_proxies;

void* g_smaClass  = nullptr;  // AStaticMeshActor UClass (the proxy class)
void* g_chipBase  = nullptr;  // actorChipPile_C     (class-kind base)
void* g_clumpBase = nullptr;  // prop_garbageClump_C (class-kind base)

// Apply the host's per-form scale to the proxy: a static-mesh actor defaults to unit scale,
// so without this the proxy renders smaller than the host's real pile or clump. The guard
// rejects a degenerate scale (zero or NaN, since a NaN comparison is false), so a malformed
// packet can never collapse the mirror invisibly.
void ApplyProxyScale(void* actor, const ue_wrap::FVector& scale) {
    if (!actor) return;
    if (scale.X > 0.001f && scale.Y > 0.001f && scale.Z > 0.001f)
        E::SetActorScale3D(actor, scale);
}

// Proxies stay collision-free. Giving the pile-form proxy query-only collision so the game's
// interaction trace hits it does nothing: the worn pile mesh has no simple collision body,
// so the trace never hits. Recognition is a camera-ray cone instead (EidForAimedPileProxy
// below), which needs no asset collision. The player passing through a mirrored pile is the
// known cost; the faithful fix for both movement blocking and occlusion-correct aim is a
// collider shape component on the proxy.

// The cached per-class-name kind: IsTrashProxyClass runs once per prop spawn (a join burst
// is thousands) and FindClass is a full object-array walk, so the cache makes only the dozen
// distinct class names ever walk. IsClumpClass folds into the same lookup, one FindClass per
// class. Game-thread serial, so no mutex.
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

// The fixed dirtball clump mesh, cached. Null if no clump asset is resident yet on this
// client; the caller then falls back to the pile mesh, so a proxy is never invisible.
void* ResolveClumpMesh() {
    static ue_wrap::CachedObjRef sDirtball;
    if (!sDirtball.Alive()) sDirtball.Set(R::FindObject(L"dirtball", L"StaticMesh"));
    return sDirtball.Raw();  // just validated/refilled
}

// The last-ditch mesh, the engine's basic cube, so a proxy is never invisible even if both
// the dirtball mesh and the pile mesh fail to resolve. Unreachable in practice: a loaded
// trash class pins its referenced meshes resident, and the pile mesh resolver caches the last
// good one, so a native load call is not warranted and the resident find is the
// proportionate guard. Last-good cached.
void* ResolveCubeFallback() {
    static ue_wrap::CachedObjRef sCube;
    if (!sCube.Alive()) sCube.Set(R::FindObject(L"Cube", L"StaticMesh"));
    return sCube.Raw();  // just validated/refilled
}

// Skin the proxy's mesh component for the requested form. Setting the static mesh recomputes
// the component's bounds and collision body in the same call, so there is no window where
// the visual is the new form but the bound is the old. `worldCtx` is a live object for the
// pile-type lookup's world context (the proxy actor itself). A pile is the chip-type pile
// mesh directly, with its own slot-0 material; one shared component is re-skinned back and
// forth, unlike the game's separate actors, so any leftover clump material override is
// cleared (a null material reverts slot 0 to the mesh's default). A clump is the fixed
// dirtball mesh plus the pile mesh's slot-0 material, the game's own behaviour (the clump
// blueprint swaps the material on a fixed mesh, never the mesh). The mesh fallback chain is
// dirtball, then the pile mesh, then the cube, so the clump is never invisible.
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
        // Which mesh the clump got: on the pile fallback the clump re-skin is a visual no-op (the
        // clump looks exactly like the pile). Event-driven, once per convert or grab, not hot.
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
    // Re-spawn convergence (a re-seed, duplicate or snapshot-bootstrap spawn for an eid already
    // mirrored): return the existing proxy, never a second actor, and do not re-skin. The
    // proxy's form is owned exclusively by the context-ordered convert channel (the initial
    // form here plus ReskinProxy); a trailing or stale spawn carries the spawn class, which for a
    // pile the host has since grabbed is the pile class, and re-skinning on it would flip a
    // correctly converted clump back to a pile.
    if (auto it = g_proxies.find(eid);
        it != g_proxies.end() && it->second.actor.Alive()) {
        it->second.ownerSlot = ownerSlot;  // a re-bracket may re-stamp the owner
        return it->second.actor.Raw();     // just Alive()-validated
    }
    void* actor = E::SpawnActor(g_smaClass, loc, /*inertPawn=*/false);
    if (!actor) {
        UE_LOGW("trash_proxy: SpawnActor(StaticMeshActor) failed eid=%u", eid);
        return nullptr;
    }
    void* comp = E::GetStaticMeshComponent(actor);             // resolve ONCE (invariant for the actor's life)
    // A static-mesh actor defaults to static mobility, and at runtime both the mesh set and the
    // location set are silent no-ops on a static component: the proxy would be invisible and
    // unable to follow the carry while the convert and throw sounds still fire. The proxy is a
    // kinematic host-driven follower re-skinned and moved every frame, so make it movable before
    // the first skin or transform.
    E::SetComponentMobility(comp, /*EComponentMobility::Movable=*/2);
    E::SetActorRotation(actor, rot);
    // Pinned below, into the entry that owns it (see the entry's pin).
    E::SetActorRootCollisionEnabled(actor, /*ECollisionEnabled::NoCollision=*/0);  // kinematic follower (aim-grab is a camera-ray cone, not collision)
    SkinProxy(actor, comp, chipType, isClump);
    ApplyProxyScale(actor, scale);                              // host-sized (else default unit -> too small)
    ProxyEntry pe;
    pe.actor.Set(actor);  // fresh from SpawnActor
    // Never collected, so never stale, so no duplicate; released when the entry dies. A failure
    // voids the header's rules-of-existence argument, so it is never silent.
    if (!pe.pin.Pin(actor)) {
        UE_LOGW("[PILE] trash_proxy: GC PIN FAILED for actor=%p eid=%u -- the proxy can be "
                "collected out from under its cached pointer", actor, eid);
    }
    pe.comp = comp;
    pe.ownerSlot = ownerSlot;
    pe.isClump = isClump;
    g_proxies[eid] = std::move(pe);  // ProxyEntry owns a GcPin -> move-only
    UE_LOGI("[PILE] trash_proxy: SPAWN eid=%u %s chipType=%u actor=%p ownerSlot=%d "
            "(AStaticMeshActor, rooted, NoCollision)",
            eid, isClump ? "clump" : "pile", static_cast<unsigned>(chipType), actor, ownerSlot);
    return actor;
}

void* ReskinProxy(coop::element::ElementId eid, uint8_t chipType, bool isClump, const ue_wrap::FVector& scale) {
    UE_ASSERT_GAME_THREAD("trash_proxy::ReskinProxy");
    auto it = g_proxies.find(eid);
    if (it == g_proxies.end()) return nullptr;
    void* actor = it->second.actor.Get();  // slot-validated (rooted, but teardown can kill)
    if (!actor) return nullptr;
    SkinProxy(actor, it->second.comp, chipType, isClump);  // in place -> binding untouched -> no dup
    ApplyProxyScale(actor, scale);                         // re-apply the per-form scale (clump != pile size)
    it->second.isClump = isClump;                          // track the new form (NearestPileProxy skips clumps)
    return actor;
}

void RetireProxy(coop::element::ElementId eid) {
    UE_ASSERT_GAME_THREAD("trash_proxy::RetireProxy");
    auto it = g_proxies.find(eid);
    if (it == g_proxies.end()) return;
    void* actor = it->second.actor.Raw();      // for pointer-compare drive evict + logs
    void* liveActor = it->second.actor.Get();  // slot-validated for the destroy call
    // Take the pin out of the entry before erasing, so the map can be erased first (nothing
    // below may re-enter and find a half-retired entry) while the unroot still happens last,
    // after the destroy: the destroy marks pending kill, then the unroot makes that memory
    // reapable.
    ue_wrap::GcPin pin = std::move(it->second.pin);
    g_proxies.erase(it);
    // Evict the drive first, so neither the drive tick nor the force release touches the actor
    // being destroyed (a stale drive entry to a freed actor would be a use after free).
    coop::remote_prop::ClearAnyDriveFor(actor);
    coop::trash_clump_pose_stream::ClearDriveForEid(eid);  // drop the host-auth per-eid carry drive too
    // The destroy is conditional because destroying needs a live actor. The unroot is not: the
    // pin releases at scope exit whatever state the actor is in. Splitting the two is the whole
    // fix; pairing them under one liveness guard is what left hundreds of rooted actors
    // anchoring a dead world.
    if (liveActor) E::DestroyActor(liveActor);
    // Unbind the Prop mirror (a deferred destructor outside the manager mutex, the documented
    // teardown pattern; the element destructor unregisters the mirror).
    coop::element::ElementDeleter::Get().Enqueue(
        coop::element::MirrorManager<coop::element::Prop>::Instance().Take(eid));
    // The pin has not released yet here; it releases when `pin` leaves scope, one line below.
    // Said plainly because a log line is the first thing a reader cites as evidence of ordering.
    UE_LOGI("[PILE] trash_proxy: RETIRE eid=%u actor=%p (drive-evicted, destroyed, unbound; "
            "un-root on scope exit)", eid, actor);
}

void RetireProxyActorOnly(coop::element::ElementId eid) {
    UE_ASSERT_GAME_THREAD("trash_proxy::RetireProxyActorOnly");
    auto it = g_proxies.find(eid);
    if (it == g_proxies.end()) return;
    void* actor = it->second.actor.Raw();      // pointer-compare drive evict + logs
    void* liveActor = it->second.actor.Get();  // slot-validated for the destroy call
    // The same take-the-pin, erase, drive-evict, destroy, release order as RetireProxy, without
    // the element unbind: the caller (the convert's nativize hand-off) has already rebound the
    // eid onto the native pile in place, so draining the element here would delete the element
    // the native now owns. The actor-only half of the teardown.
    ue_wrap::GcPin pin = std::move(it->second.pin);
    g_proxies.erase(it);
    coop::remote_prop::ClearAnyDriveFor(actor);
    coop::trash_clump_pose_stream::ClearDriveForEid(eid);
    if (liveActor) E::DestroyActor(liveActor);
    UE_LOGI("[PILE] trash_proxy: RETIRE-ACTOR-ONLY eid=%u actor=%p (drive-evicted, destroyed; "
            "un-root on scope exit; Element KEPT -- rebound to the native pile by the caller)",
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
        void* a = e.actor.Get();  // slot-validated
        if (!a) continue;
        const ue_wrap::FVector p = E::GetActorLocation(a);
        const float dx = p.X - fromLoc.X, dy = p.Y - fromLoc.Y, dz = p.Z - fromLoc.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (best2 < 0.f || d2 < best2) { best2 = d2; best = a; }
    }
    if (outDistCm) *outDistCm = (best2 >= 0.f) ? std::sqrt(best2) : -1.f;
    return best;
}

void* ProxyActorForEid(coop::element::ElementId eid) {
    UE_ASSERT_GAME_THREAD("trash_proxy::ProxyActorForEid");
    auto it = g_proxies.find(eid);
    if (it == g_proxies.end()) return nullptr;
    return it->second.actor.Get();  // slot-validated
}

coop::element::ElementId EidForAimedPileProxy(const ue_wrap::FVector& camLoc, const ue_wrap::FVector& camFwd,
                                             float maxRangeCm, float minDot) {
    UE_ASSERT_GAME_THREAD("trash_proxy::EidForAimedPileProxy");
    // The camera-ray cone: the pile-form proxy within `maxRangeCm` and most centred on the aim
    // ray (the largest dot at or above `minDot`). `camFwd` must be unit length. This is the
    // client-grab recognition, and it needs no asset collision (the pile mesh has no simple
    // collision body, so a trace cannot hit it). The host re-validates the eid (live, a chip
    // pile) on the grab intent, so a cone mis-pick in a dense cluster is bounded. Invalid if
    // nothing qualifies.
    coop::element::ElementId best = coop::element::kInvalidId;
    float bestDot = minDot;
    for (const auto& kv : g_proxies) {
        const ProxyEntry& e = kv.second;
        if (e.isClump) continue;                          // a clump-form proxy is the transient carry -- not grabbable
        void* a = e.actor.Get();  // slot-validated
        if (!a) continue;
        const ue_wrap::FVector p = E::GetActorLocation(a);
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
    // A per-slot disconnect (a single peer dropping while the session stays up) drains that
    // slot's Prop mirrors through the generic drain, which does not retire proxies, so a rooted
    // proxy would leak. Retire every proxy owned by `slot` here, before the generic drain runs,
    // so the actor is unrooted, destroyed and unbound through the authoritative tracker.
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
    // Snapshot the eids first; RetireProxy mutates the map.
    std::vector<coop::element::ElementId> eids;
    eids.reserve(n);
    for (const auto& kv : g_proxies) eids.push_back(kv.first);
    for (auto eid : eids) RetireProxy(eid);
    UE_LOGI("[PILE] trash_proxy: OnDisconnect retired %zu proxy mirror(s)", n);
}

}  // namespace coop::trash_proxy
