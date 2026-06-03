// coop/trash_collect_sync.cpp -- see coop/trash_collect_sync.h.
//
// One function: EnsureHeldItemBroadcast. net_pump's held-prop send calls it on
// the new-held edge. The collect itself (trashBitsPile_C::playerTryToCollect)
// is BP-internal (ProcessInternal) so it can't be observed; we instead act on
// the grabbing_actor the send already resolves -- a freshly-spawned, auto-
// grabbed Aprop_C with Key=None gets a stable Key + a PropSpawn here, then the
// existing pose stream mirrors it into the collector's hands.

#include "coop/trash_collect_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/prop_element_tracker.h"
#include "coop/prop_synth_key.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

#include <string>

namespace coop::trash_collect_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace PT = coop::prop_element_tracker;

}  // namespace

bool EnsureHeldItemBroadcast(void* heldActor, coop::net::Session* s) {
    if (!heldActor || !s || !s->connected()) return false;
    if (!R::IsLive(heldActor)) return false;
    // Any KEYED interactable: Aprop_C trash items AND the non-Aprop_C
    // garbageClump/chipPile (the actual trash the player carries). CRASH-SAFETY
    // is enforced on the RECEIVER, not by excluding them here: GetStaticMesh
    // returns null for non-Aprop_C, so the peer never runs physics on the clump
    // mirror -- it spawns physics-free and is driven KINEMATICALLY (the inverse
    // of the reverted-2a UAF). [[project-bug-trash-chippile-uaf-crash]]
    if (!ue_wrap::prop::IsKeyedInteractable(heldActor)) return false;

    // Only freshly-spawned UNKEYED items need a mint + spawn broadcast. An
    // already-keyed held actor is a normal world prop the peer already has (the
    // pose stream alone mirrors it) -- AND after our own mint the Key is
    // non-None, so this is the idempotency guard against re-broadcasting.
    std::wstring keyStr = ue_wrap::prop::GetInteractableKeyString(heldActor);
    if (!keyStr.empty() && keyStr != L"None") return false;

    const std::wstring cls = R::ClassNameOf(heldActor);
    // Aprop_C trash items get a force-minted Key (their UCS holds it). The non-Aprop_C
    // trash CLUMP (prop_garbageClump_C) is NON-KEYABLE -- setKey doesn't stick (the
    // source re-reads None, autotest 33e7f25) -- so it rides the SAME prop pipeline but
    // is identified by our EID instead: broadcast with key=None + the eid, and the
    // receiver resolves its mirror by eid (PropPoseSnapshot.elementId, v26). The clump
    // renders on its own (bare spawn = the 'dirtball' mesh), so no mesh transfer needed.
    // [[project-bug-trash-chippile-uaf-crash]]
    const bool isAprop = ue_wrap::prop::IsDescendantOfProp(heldActor);
    keyStr = coop::prop_synth_key::EnsureKeyForBroadcast(heldActor, keyStr, /*mintForAprop=*/isAprop);
    if (keyStr.empty() || keyStr == L"None") {
        if (isAprop) {
            UE_LOGW("trash_collect: Aprop item %p cls='%ls' Key still None after force-mint -- cannot mirror",
                    heldActor, cls.c_str());
            return false;
        }
        keyStr.clear();  // clump: wire key stays None; the eid is the cross-peer identity
    }
    // Create the Prop Element shadow + dedupe latch so the item's eventual
    // K2_DestroyActor unwinds it through the normal destroy path.
    PT::MarkProcessedInit(heldActor);
    PT::MarkPropElement(heldActor, keyStr, cls);

    coop::net::PropSpawnPayload p{};
    p.className.len = 0;
    for (size_t i = 0; i < cls.size() && i < 63; ++i)
        p.className.data[p.className.len++] = static_cast<char>(cls[i]);
    p.key.len = 0;
    for (size_t i = 0; i < keyStr.size() && i < 31; ++i)
        p.key.data[p.key.len++] = static_cast<char>(keyStr[i]);

    const ue_wrap::FVector  loc = ue_wrap::engine::GetActorLocation(heldActor);
    const ue_wrap::FRotator rot = ue_wrap::engine::GetActorRotation(heldActor);
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
    p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
    p.scaleX = p.scaleY = p.scaleZ = 1.f;
    p.physFlags = coop::net::propspawn_flags::kSimulatePhysics;
    // IsHeavy/IsFrozen read Aprop_C struct offsets -- only meaningful on Aprop_C.
    // For a non-Aprop_C clump the receiver ignores physFlags (kinematic mirror),
    // so don't read stray bytes off it.
    if (ue_wrap::prop::IsDescendantOfProp(heldActor)) {
        if (ue_wrap::prop::IsHeavy(heldActor))  p.physFlags |= coop::net::propspawn_flags::kIsHeavy;
        if (ue_wrap::prop::IsFrozen(heldActor)) p.physFlags |= coop::net::propspawn_flags::kFrozen;
    }
    p.initLinVelX = p.initLinVelY = p.initLinVelZ = 0.f;
    p.initAngVelX = p.initAngVelY = p.initAngVelZ = 0.f;
    {
        const coop::element::ElementId eid = PT::GetPropElementIdForActor(heldActor);
        p.elementId = (eid == coop::element::kInvalidId) ? 0u : eid;
    }
    UE_LOGI("trash_collect: BROADCAST held unkeyed item cls='%ls' key='%ls' loc=(%.1f,%.1f,%.1f) "
            "-- held-pose stream now mirrors it into the collector's hands",
            cls.c_str(), keyStr.c_str(), p.locX, p.locY, p.locZ);
    s->SendPropSpawn(p);
    return true;
}

}  // namespace coop::trash_collect_sync
