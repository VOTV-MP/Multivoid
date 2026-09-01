// coop/props/native_pile_mirror.cpp -- see coop/props/native_pile_mirror.h.

#include "ue_wrap/core/gc_pin.h"
#include "coop/props/native_pile_mirror.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"  // Registry::Get().Get(eid) -> Element (SetSaveNative)
#include "coop/props/remote_prop.h"  // RegisterPropMirror

#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"            // ResolvePileMesh
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

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

// Apply the host-authoritative APPEARANCE to a chipPile native (chipType + scale + the host's visible-mesh
// WORLD rotation). Shared by Materialize (a fresh spawn) and RepositionBoundNative (an already-bound native
// we reuse). Does NOT spawn / root / position / bind -- pure appearance. See Materialize for the rationale
// on each line (init()-drives-both-mesh-components + mesh-COMPONENT rotation to avoid the double-rotate).
void SkinPileNative(void* native, uint8_t chipType, const ue_wrap::FRotator& meshWorldRot,
                    const ue_wrap::FVector& scale) {
    ue_wrap::prop::SetChipTypeAndRebuild(native, chipType);
    if (scale.X > 0.001f && scale.Y > 0.001f && scale.Z > 0.001f) E::SetActorScale3D(native, scale);
    if (void* comp = E::GetStaticMeshComponent(native)) E::SetComponentWorldRotation(comp, meshWorldRot);
}

// The pins this module holds, OWNED. A raw AddToRoot with the release written out by hand
// in three other modules is what this replaces: none of those paths runs at a session
// teardown, so a materialized native that simply outlived the session stayed rooted and
// anchored its world forever (ue_wrap/core/gc_pin.h has the measurement).
std::unordered_map<void*, ue_wrap::GcPin> g_pins;

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
    // `.Pin()` on the mapped handle, NOT `= GcPin(native)`: C++17 sequences the temporary's
    // Pin BEFORE the assignment, so on a repeated key the move-assign's Release would clear
    // the RootSet bit the temporary just set and leave the map claiming a pin that is not
    // held. Unreachable today (a rooted actor's address cannot be recycled) and exactly the
    // trap this class exists to remove.
    if (!g_pins[native].Pin(native)) {              // GC-pin -- a runtime spawn has no save/world ref
        // A failed pin voids the mirror's whole rules-of-existence argument (rooted -> never
        // GC'd -> never a stale index), so it must never fail silently.
        UE_LOGW("[PILE] native_pile_mirror: GC PIN FAILED for native=%p eid=%u -- this mirror "
                "can be collected out from under its cached pointer", native, eid);
        g_pins.erase(native);
    }
    E::SetActorTickEnabled(native, false);         // no autonomous per-frame ubergraph
    E::SetActorSimulatePhysics(native, false);     // kinematic resting pile (the host positions it)
    E::SetActorRootMovable(native);                // else SetActorLocation (host pose / b3) silently no-ops on a Static root
    // Skin the host's chipType the GAME's OWN way (chipType via the pile's init() -> both mesh components;
    // scale; and the host's visible-mesh WORLD rotation on the mesh COMPONENT, not the root -> no
    // double-rotate). The fresh SpawnActor above ran init once via UCS with the DEFAULT chipType=0; this
    // re-skins it to the host's variant + consumes the host's rotation (host->client, same axis as chipType).
    SkinPileNative(native, chipType, meshWorldRot, scale);

    if (!skipBind) {
        coop::remote_prop::RegisterPropMirror(eid, native, L"", className, senderSlot, rebindInPlace);
        if (auto* el = coop::element::Registry::Get().Get(eid)) el->SetSaveNative(true);  // mark bound-native
    }
    UE_LOGI("[PILE] native_pile_mirror: MATERIALIZED eid=%u native=%p class='%ls' chipType=%u "
            "(rooted, tick-off, kinematic, Movable, native collision) -- native hover GUI + rotation free",
            eid, native, className.c_str(), static_cast<unsigned>(chipType));
    return native;
}

void Unpin(void* actor) {
    UE_ASSERT_GAME_THREAD("native_pile_mirror::Unpin");
    if (!actor) return;
    g_pins.erase(actor);  // no-op when we never pinned it (save-loaded / game-native)
}

void OnDisconnect() {
    UE_ASSERT_GAME_THREAD("native_pile_mirror::OnDisconnect");
    if (g_pins.empty()) return;
    const size_t n = g_pins.size();
    g_pins.clear();
    UE_LOGI("[PILE] native_pile_mirror: OnDisconnect released %zu GC pin(s)", n);
}

void RepositionBoundNative(void* native, uint8_t chipType, const ue_wrap::FVector& loc,
                           const ue_wrap::FRotator& meshWorldRot, const ue_wrap::FVector& scale) {
    if (!native) return;
    UE_ASSERT_GAME_THREAD("native_pile_mirror::RepositionBoundNative");
    // Reuse an already-bound save-loaded native as the LAND mirror -- reposition + re-skin it to the host's
    // landed transform. NO spawn, NO bind (it is already the Element's bound mirror). Movable-force first:
    // a save-loaded native may be Static, so SetActorLocation would silently no-op (the b3 lesson). This is
    // the create-edge CLAIM (reuse the local result) that suppresses the parallel proxy spawn entirely.
    E::SetActorRootMovable(native);
    E::SetActorLocation(native, loc);
    SkinPileNative(native, chipType, meshWorldRot, scale);
}

}  // namespace coop::native_pile_mirror
