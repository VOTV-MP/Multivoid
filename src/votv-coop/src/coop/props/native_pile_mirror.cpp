// coop/props/native_pile_mirror.cpp -- see coop/props/native_pile_mirror.h.

#include "coop/props/native_pile_mirror.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"  // Registry::Get().Get(eid) -> Element (SetSaveNative)
#include "coop/props/remote_prop.h"  // RegisterPropMirror

#include "ue_wrap/engine.h"
#include "ue_wrap/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"            // ResolvePileMesh
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace coop::native_pile_mirror {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// Cached pile UClass per class name (FindClass is a ~237k-entry GUObjectArray walk; the trash family is
// a handful of distinct names). GT-serial -> no mutex.
std::unordered_map<std::wstring, void*> g_clsCache;

void* ResolvePileClass(const std::wstring& className) {
    if (className.empty()) return nullptr;
    auto it = g_clsCache.find(className);
    if (it != g_clsCache.end() && it->second) return it->second;
    void* cls = R::FindClass(className.c_str());
    if (cls) g_clsCache[className] = cls;  // do not cache a miss (could load later)
    return cls;
}

}  // namespace

void* Materialize(coop::element::ElementId eid, const std::wstring& className, uint8_t chipType,
                  const ue_wrap::FVector& loc, const ue_wrap::FRotator& meshWorldRot,
                  const ue_wrap::FVector& scale, int senderSlot, bool skipBind, bool rebindInPlace) {
    UE_ASSERT_GAME_THREAD("native_pile_mirror::Materialize");
    void* cls = ResolvePileClass(className);
    if (!cls) {
        UE_LOGW("[PILE] native_pile_mirror: class '%ls' not loaded -- cannot materialize native eid=%u",
                className.c_str(), eid);
        return nullptr;
    }
    void* native = E::SpawnActor(cls, loc, /*inertPawn=*/false);
    if (!native) {
        UE_LOGW("[PILE] native_pile_mirror: SpawnActor('%ls') FAILED eid=%u", className.c_str(), eid);
        return nullptr;
    }
    // The PROVEN inert recipe (2026-06-30 collision-ON probe: 60s live + inert, hover GUI present):
    R::AddToRoot(native);                          // GC-pin -- a runtime spawn has no save/world ref
    E::SetActorTickEnabled(native, false);         // no autonomous per-frame ubergraph
    E::SetActorSimulatePhysics(native, false);     // kinematic resting pile (the host positions it)
    E::SetActorRootMovable(native);                // else SetActorLocation (host pose / b3) silently no-ops on a Static root
    // Skin the host's chipType the GAME's OWN way: write chipType + dispatch the pile's init(), which does
    // getChipPileType(chipType) -> SetStaticMesh on BOTH the pile's StaticMesh AND Collision components (RE:
    // actorChipPile 'init' export 80 -- the same call loadData/loadPrimitiveData make). The fresh SpawnActor
    // above already ran init once via UserConstructionScript, but with the DEFAULT chipType=0 -> a type-0
    // pile; this re-init re-skins it to the host's variant. An external single SetStaticMesh CANNOT do this
    // (the pile owns two mesh components + a per-variant collision mesh), which is why the runtime mirror
    // showed a wrong-type / wrong-texture pile. The per-instance random mesh rotation from UCS is preserved
    // (init only touches the meshes, not the component rotation).
    ue_wrap::prop::SetChipTypeAndRebuild(native, chipType);
    if (scale.X > 0.001f && scale.Y > 0.001f && scale.Z > 0.001f) E::SetActorScale3D(native, scale);
    // Consume the HOST's authoritative visible-mesh rotation (host->client, the SAME delivery axis as
    // chipType): the pile's visual roll lives on the StaticMesh COMPONENT's rotation, and the host captured
    // it via GetVisibleMeshWorldRotation (f79bbe84). Apply it to the SAME mesh component's WORLD rotation
    // (symmetric with the capture) so the client MATCHES the host -- NOT SetActorRotation on the root, which
    // would compound on top of the component's UCS random roll = a double-rotate. Without this the native
    // kept its own random roll and diverged ~50% per pile. (init() above skins the mesh but does not touch
    // rotation, so the UCS roll is what we override here.)
    if (void* comp = E::GetStaticMeshComponent(native)) E::SetComponentWorldRotation(comp, meshWorldRot);

    if (!skipBind) {
        coop::remote_prop::RegisterPropMirror(eid, native, L"", className, senderSlot, rebindInPlace);
        if (auto* el = coop::element::Registry::Get().Get(eid)) el->SetSaveNative(true);  // mark bound-native
    }
    UE_LOGI("[PILE] native_pile_mirror: MATERIALIZED eid=%u native=%p class='%ls' chipType=%u "
            "(rooted, tick-off, kinematic, Movable, native collision) -- native hover GUI + rotation free",
            eid, native, className.c_str(), static_cast<unsigned>(chipType));
    return native;
}

}  // namespace coop::native_pile_mirror
