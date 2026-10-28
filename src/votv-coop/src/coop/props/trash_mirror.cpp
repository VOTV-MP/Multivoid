// coop/props/trash_mirror.cpp -- see coop/props/trash_mirror.h.

#include "ue_wrap/core/gc_pin.h"
#include "coop/props/trash_mirror.h"

#include "coop/dev/pile_defer_drill.h"  // [dev] the pile deferral drill counts a deferred eid's mirrors
#include "coop/element/element.h"
#include "coop/element/element_deleter.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/prop.h"
#include "coop/element/registry.h"  // Registry::Get().Get(eid) -> Element (SetSaveNative)
#include "coop/props/pile_look.h"           // Forget (a retired eid keeps no look)
#include "coop/props/prop_echo_suppress.h"  // MarkIncomingDestroy (an authoritative destroy is not our news)
#include "coop/props/prop_element_tracker.h"  // UnmarkKnownKeyedProp
#include "coop/props/remote_prop.h"  // RegisterPropMirror / ClearAnyDriveFor
#include "coop/props/trash_native_hold.h"  // Hold: the one recipe that parks a trash body
#include "coop/props/trash_clump_pose_stream.h"  // ClearDriveForEid (the per-eid carry drive)

#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"            // SetChipTypeAndRebuild / IsGarbageClump
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::trash_mirror {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// Apply the host-authoritative APPEARANCE to a trash native: chip type, scale, and the host
// actor's rotation. The re-init draws a pile's child mesh a NEW random turn and scale every time
// it runs; the host's draw replaces it at the bind or right after a re-skin
// (coop/props/pile_look.h). Shared by Materialize (a fresh spawn) and RepositionBoundNative
// (an already-bound native we reuse). It does NOT spawn, root, position or bind -- appearance
// only. SetChipTypeAndRebuild covers both forms: it writes the enum byte and then calls the
// actor's own init, which rebuilds a pile's mesh and re-materials a clump.
void SkinTrashNative(void* native, uint8_t chipType, const ue_wrap::FRotator& rot,
                    const ue_wrap::FVector& scale) {
    ue_wrap::prop::SetChipTypeAndRebuild(native, chipType);
    if (scale.X > 0.001f && scale.Y > 0.001f && scale.Z > 0.001f) E::SetActorScale3D(native, scale);
    // A pile's init() ends by making its root Static again, and a Static root ignores a rotation
    // write while the call still reports success: a pile the host landed on a slope would lie flat
    // here. Movable first, every time the re-init has run.
    E::SetActorRootMovable(native);
    E::SetActorRotation(native, rot);
}

// The mirrors this module MADE, with the pin each one owns. A GcPin releases from its
// destructor, so erasing the entry is the whole release; a raw AddToRoot released by hand in
// three other modules used to leave a mirror that merely outlived its session rooted, anchoring
// its world forever. The entry also answers what only this map can: whether an actor is one we
// spawned -- a client's OWN save-loaded pile is bound as a mirror too and is never ours to
// destroy -- and which peer's expression made it, so one leaver's mirrors retire before the
// generic mirror drain unbinds them and takes the answer away.
struct MadeMirror {
    ue_wrap::GcPin pin;
    coop::element::ElementId eid = coop::element::kInvalidId;
    int ownerSlot = -1;
    // The actor's object-array slot at the spawn. A pinned actor's address cannot be recycled, but
    // an entry whose pin failed has no such guarantee, so the address alone does not name it.
    int32_t idx = -1;
};
std::unordered_map<void*, MadeMirror> g_made;

// Destroy one mirror we made and release its pin, in that order: the destroy marks pending kill
// and the un-root is what makes that memory reapable. Pairing them under one liveness guard is
// what left hundreds of rooted actors anchoring a dead world. False when `actor` is not ours,
// which is the whole test a caller needs before destroying anything.
bool DestroyIfOurs(void* actor) {
    auto it = g_made.find(actor);
    if (it == g_made.end()) return false;
    ue_wrap::GcPin pin = std::move(it->second.pin);  // out of the map, released at scope exit
    const int32_t idx = it->second.idx;
    g_made.erase(it);
    if (R::IsLiveByIndex(actor, idx)) E::DestroyActor(actor);
    return true;
}

}  // namespace

void* Materialize(coop::element::ElementId eid, const std::wstring& className, uint8_t chipType,
                  const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot,
                  const ue_wrap::FVector& scale, int senderSlot, bool skipBind, bool rebindInPlace,
                  void* (*resolve)(const wchar_t*)) {
    UE_ASSERT_GAME_THREAD("trash_mirror::Materialize");
    void* cls = className.empty() ? nullptr
                : (resolve ? resolve(className.c_str()) : R::FindClass(className.c_str()));
    if (!cls) {
        UE_LOGW("[PILE] trash_mirror: class '%ls' not loaded -- cannot materialize native eid=%u",
                className.c_str(), eid);
        return nullptr;
    }
    void* native = E::SpawnActor(cls, loc, /*inertPawn=*/false);
    if (!native) {
        UE_LOGW("[PILE] trash_mirror: SpawnActor('%ls') FAILED eid=%u", className.c_str(), eid);
        return nullptr;
    }
    // `.Pin()` on the mapped handle, NOT `= GcPin(native)`: C++17 sequences the temporary's Pin
    // BEFORE the assignment, so on a repeated key the move-assign's Release would clear the RootSet
    // bit the temporary had just set, leaving the map claiming a pin nobody holds. Unreachable
    // today, since a rooted actor's address cannot be recycled, and exactly the trap this class
    // exists to remove.
    MadeMirror& made = g_made[native];
    made.eid = eid;
    made.ownerSlot = senderSlot;
    made.idx = R::InternalIndexOf(native);
    if (!made.pin.Pin(native)) {                    // GC-pin -- a runtime spawn has no save/world ref
        // A failed pin voids the mirror's whole rules-of-existence argument (rooted -> never
        // GC'd -> never a stale index), so it must never fail silently. The entry STAYS, with an
        // empty pin: it is also the only record that this actor is one we spawned, and without it
        // Retire keeps the mirror alive as "the client's own" and OnDisconnect leaves it a ghost.
        UE_LOGW("[PILE] trash_mirror: GC PIN FAILED for native=%p eid=%u -- this mirror "
                "can be collected out from under its cached pointer", native, eid);
    }
    // The parking, classified (coop-sync-doctrine step 4). Each removes a SECOND AUTHOR of a state
    // the host already authors and replicates -- the pose, and the pile-clump transition -- so
    // after it exactly one peer authors each, which is what makes these root-cause parkings and
    // not blindfolds. All three are restorable; the session teardown destroys what it made rather
    // than restoring, because a mirror expresses a remote entity and there is none left.
    coop::trash_native_hold::Hold(native);         // tick off, kinematic, root Movable: the host authors it
    // Skin the host's chip type the GAME's OWN way: the chip type through the actor's own init,
    // then the scale and the host actor's rotation. The SpawnActor above already ran init once,
    // through the construction script, with the default chip type of 0; this re-skins it to the
    // host's variant.
    SkinTrashNative(native, chipType, rot, scale);
    // A clump renders in a hand: it is the carried form, driven to a puppet's hand by the pose
    // stream with no holder to be attached to, so its collision would block the very player
    // carrying it. The landed form is a pile with the game's own collision.
    // Presentation, not a parking: a clump renders in a hand or on a host-driven flight path, and
    // on the authoritative peer the game itself holds it, so there is no moment at which local
    // collision would be right.
    const bool isClump = ue_wrap::prop::IsGarbageClump(native);
    if (isClump) E::SetActorEnableCollision(native, false);

    if (!skipBind) {
        coop::remote_prop::RegisterPropMirror(eid, native, L"", className, senderSlot, rebindInPlace);
        if (auto* el = coop::element::Registry::Get().Get(eid)) el->SetSaveNative(true);  // mark bound-native
    }
    coop::dev::pile_defer_drill::NoteMaterialised(static_cast<uint32_t>(eid));   // [dev] the pile deferral drill's count
    UE_LOGI("[PILE] trash_mirror: MATERIALIZED eid=%u native=%p class='%ls' chipType=%u form=%s "
            "(rooted, tick-off, kinematic, Movable) -- the game's own hover GUI, trace and rotation",
            eid, native, className.c_str(), static_cast<unsigned>(chipType),
            isClump ? "clump (collision off while carried)" : "pile (native collision)");
    return native;
}

void Unpin(void* actor) {
    UE_ASSERT_GAME_THREAD("trash_mirror::Unpin");
    if (!actor) return;
    // This releases BEFORE the caller's destroy, the opposite order to DestroyIfOurs, and both are
    // correct: the game thread runs the two statements with no collection between them, so the
    // order only matters to a reader. What must not happen is a destroy with the pin still held by
    // a map nobody clears, which is what this exists to prevent.
    g_made.erase(actor);  // no-op when we never made it (save-loaded / game-native)
}

bool WeMade(void* actor) {
    UE_ASSERT_GAME_THREAD("trash_mirror::WeMade");
    if (!actor) return false;
    auto it = g_made.find(actor);
    return it != g_made.end() && R::IsLiveByIndex(actor, it->second.idx);
}

void Retire(coop::element::ElementId eid, bool authoritative) {
    UE_ASSERT_GAME_THREAD("trash_mirror::Retire");
    void* actor = nullptr;
    if (auto* el = coop::element::Registry::Get().Get(eid)) {
        void* a = el->GetActor();
        if (a && R::IsLiveByIndex(a, el->GetInternalIdx())) actor = a;
    }
    bool destroyed = false;
    if (actor) {
        // Evict the drives first, so neither the drive tick nor a force release touches an actor
        // being destroyed.
        coop::remote_prop::ClearAnyDriveFor(actor);
        coop::trash_clump_pose_stream::ClearDriveForEid(eid);
        // Ours to destroy always; the client's own save-loaded pile only on the host's word.
        destroyed = DestroyIfOurs(actor);
        if (!destroyed && authoritative) {
            // The host says the entity is gone. Marked as local bookkeeping first, so our own
            // destroy observer does not broadcast it back as this peer's news.
            coop::prop_element_tracker::UnmarkKnownKeyedProp(actor);
            coop::prop_echo_suppress::MarkIncomingDestroy(actor);
            E::DestroyActor(actor);
            destroyed = true;
        }
    }
    // The binding goes either way (a deferred destructor outside the manager mutex, the documented
    // teardown pattern; the element destructor unregisters the mirror).
    coop::element::ElementDeleter::Get().Enqueue(
        coop::element::MirrorManager<coop::element::Prop>::Instance().Take(eid));
    coop::pile_look::Forget(static_cast<uint32_t>(eid));
    UE_LOGI("[PILE] trash_mirror: RETIRE eid=%u actor=%p %s (%s; unbound)",
            eid, actor, authoritative ? "[the host's word]" : "[local teardown]",
            destroyed ? "destroyed" : "the client's own actor -- kept alive");
}

void OnDisconnectForSlot(int slot) {
    UE_ASSERT_GAME_THREAD("trash_mirror::OnDisconnectForSlot");
    // A single peer dropping while the session stays up: the generic per-slot mirror drain unbinds
    // the elements, which takes away the only route from an eid back to its actor, so the mirrors
    // this slot's expressions made are retired here first or they stay rooted for the session.
    std::vector<coop::element::ElementId> eids;
    for (const auto& kv : g_made)
        if (kv.second.ownerSlot == slot) eids.push_back(kv.second.eid);
    if (eids.empty()) return;
    for (auto eid : eids) Retire(eid, /*authoritative=*/false);
    UE_LOGI("[PILE] trash_mirror: OnDisconnectForSlot(%d) retired %zu mirror(s)", slot, eids.size());
}

void OnDisconnect() {
    UE_ASSERT_GAME_THREAD("trash_mirror::OnDisconnect");
    if (g_made.empty()) return;
    const size_t n = g_made.size();
    // Destroy what we made: with no session there is no remote entity for a mirror to express, and
    // a parked actor nobody drives is a ghost. The client's own save-loaded piles are not in this
    // map and are untouched, so its world keeps its trash.
    std::vector<void*> actors;
    actors.reserve(n);
    for (const auto& kv : g_made) actors.push_back(kv.first);
    for (void* a : actors) {
        coop::remote_prop::ClearAnyDriveFor(a);
        DestroyIfOurs(a);
    }
    g_made.clear();
    UE_LOGI("[PILE] trash_mirror: OnDisconnect retired %zu mirror(s) we made (the client's own piles kept)", n);
}

void RepositionBoundNative(void* native, uint8_t chipType, const ue_wrap::FVector& loc,
                           const ue_wrap::FRotator& rot, const ue_wrap::FVector& scale) {
    if (!native) return;
    UE_ASSERT_GAME_THREAD("trash_mirror::RepositionBoundNative");
    // Reuse an already-bound native as the LAND mirror: reposition and re-skin it to the host's
    // landed transform. No spawn and no bind, since it is already the element's bound mirror. Movable-force comes first because a save-loaded native may be Static, and
    // SetActorLocation on a Static root silently no-ops. This is the create-edge CLAIM -- the local
    // result is taken as the answer -- which is what suppresses a parallel spawn.
    E::SetActorRootMovable(native);
    E::SetActorLocation(native, loc);
    SkinTrashNative(native, chipType, rot, scale);
}

}  // namespace coop::trash_mirror
