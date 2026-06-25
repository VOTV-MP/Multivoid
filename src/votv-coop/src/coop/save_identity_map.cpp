// coop/save_identity_map.cpp -- see header. Phase 1B: HOST-side build + log, NO wire.

#include "coop/save_identity_map.h"

#include "coop/element/element.h"          // ElementId, kInvalidId
#include "coop/kerfur_entity.h"            // IsKerfurPropClass
#include "coop/prop_element_tracker.h"     // GetPropElementIdForActor, MarkPropElement
#include "ue_wrap/call.h"                  // ParamFrame, Call
#include "ue_wrap/hot_path_guard.h"        // UE_ASSERT_GAME_THREAD
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"                  // IsChipPile, GetInteractableKeyString
#include "ue_wrap/reflection.h"            // FindObjectByClass, FindClass, FindClassDefaultObject, FindFunction, EngineFree
#include "ue_wrap/sdk_profile.h"

#include <cstdint>
#include <string>

namespace coop::save_identity_map {

namespace R  = ue_wrap::reflection;
namespace P  = ue_wrap::profile;
namespace PT = coop::prop_element_tracker;

namespace {

// A read-only view over the engine's TArray<AActor*> {Data@0x0, Num@0x8, Max@0xC} out-param.
struct FScriptArrayView {
    void*   Data = nullptr;
    int32_t Num  = 0;
    int32_t Max  = 0;
};

// Resolve (self-seed if needed) the host eid for a keyless native, mirroring CollectTracked{Pile,Kerfur}
// Transforms: chipPile mints with an empty key (keyless), an off-kerfur with its real Aprop key. Returns
// kInvalidId only if the mint declined (registry exhausted). Idempotent -- a no-op when already seeded
// (the capture's Collect walks ran just before this, so most are already tracked).
coop::element::ElementId ResolveOrSeedEid_(void* actor, Family fam, const std::wstring& cls) {
    coop::element::ElementId eid = PT::GetPropElementIdForActor(actor);
    if (eid != coop::element::kInvalidId && eid != 0u) return eid;
    if (fam == Family::ChipPile) {
        PT::MarkPropElement(actor, L"", cls);                      // keyless mint
    } else {
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
        PT::MarkPropElement(actor, (key == L"None") ? std::wstring() : key, cls);
    }
    eid = PT::GetPropElementIdForActor(actor);
    return eid;
}

}  // namespace

int BuildHostMap(IdMap& outMap) {
    UE_ASSERT_GAME_THREAD("save_identity_map::BuildHostMap");
    outMap.clear();

    // WorldContext == saveObjects' `self` (the gamemode); the gather is by int_save_C, the same call +
    // order saveObjects uses (bytecode [32]) so the keyless entries land in objectsData order.
    void* gm = R::FindObjectByClass(P::name::GamemodeClass);
    void* gsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    void* gsCls = gsCdo ? R::ClassOf(gsCdo) : nullptr;
    void* gaawiFn = gsCls ? R::FindFunction(gsCls, L"GetAllActorsWithInterface") : nullptr;
    void* intSaveCls = R::FindClass(L"int_save_C");
    if (!gm || !gsCdo || !gaawiFn || !intSaveCls) {
        UE_LOGW("save_identity_map: cannot build -- gm=%p gsCdo=%p GetAllActorsWithInterface=%p int_save_C=%p",
                gm, gsCdo, gaawiFn, intSaveCls);
        return 0;
    }

    ue_wrap::ParamFrame f(gaawiFn);
    if (!f.valid()) {
        UE_LOGW("save_identity_map: GetAllActorsWithInterface frame invalid");
        return 0;
    }
    f.Set<void*>(L"WorldContextObject", gm);
    f.Set<void*>(L"Interface", intSaveCls);  // TSubclassOf<UInterface> == the int_save_C UClass*
    if (!ue_wrap::Call(gsCdo, f)) {
        UE_LOGW("save_identity_map: GetAllActorsWithInterface dispatch failed");
        return 0;
    }
    FScriptArrayView arr{};
    f.GetRaw(L"OutActors", &arr, static_cast<int32_t>(sizeof(arr)));
    if (!arr.Data || arr.Num <= 0) {
        UE_LOGW("save_identity_map: GetAllActorsWithInterface returned %d actor(s) -- empty world?", arr.Num);
        if (arr.Data) R::EngineFree(arr.Data);  // free even an empty-but-allocated buffer
        return 0;
    }

    auto* actors = reinterpret_cast<void**>(arr.Data);
    int chip = 0, kerfur = 0, seeded = 0;
    // The gather ordinal is the entry's index (== objectsData order for the keyless subsequence; the 1B
    // header explains why ignoreSave is not applied here). Only KEYLESS families become entries.
    for (int32_t i = 0; i < arr.Num; ++i) {
        void* a = actors[i];
        if (!a || !R::IsLive(a)) continue;
        void* cls = R::ClassOf(a);
        Family fam;
        if (ue_wrap::prop::IsChipPile(a)) {
            fam = Family::ChipPile;
        } else if (cls && coop::kerfur_entity::IsKerfurPropClass(cls)) {
            fam = Family::KerfurOff;
        } else {
            continue;  // keyed / non-keyless -> binds by key, not in the eid map
        }
        const std::wstring clsName = R::ClassNameOf(a);
        const coop::element::ElementId before = PT::GetPropElementIdForActor(a);
        const coop::element::ElementId eid = ResolveOrSeedEid_(a, fam, clsName);
        if (eid == coop::element::kInvalidId || eid == 0u) continue;  // mint declined (registry full) -> skip
        if (before == coop::element::kInvalidId || before == 0u) ++seeded;
        outMap.push_back(IdEntry{static_cast<uint32_t>(i), static_cast<uint32_t>(eid),
                                 static_cast<uint8_t>(fam)});
        if (fam == Family::ChipPile) ++chip; else ++kerfur;
    }
    R::EngineFree(arr.Data);  // free the engine-allocated OutActors buffer (no leak)

    UE_LOGI("save_identity_map: HOST map built (Phase 1B, NO wire) -- %zu entries (%d chipPile + %d kerfurOff) "
            "from %d int_save actors, %d self-seeded at build. index = int_save gather ordinal (objectsData "
            "order for the keyless subsequence). eids should match the S8.2 capture-eids.",
            outMap.size(), chip, kerfur, arr.Num, seeded);
    // Sample the first + last few entries so the host log shows the index->eid->family pairs concretely.
    const size_t n = outMap.size();
    for (size_t j = 0; j < n; ++j) {
        if (j < 5 || j + 5 >= n) {
            const IdEntry& e = outMap[j];
            UE_LOGI("save_identity_map:   [%zu] index=%u eid=%u family=%s", j, e.index, e.eid,
                    e.family == static_cast<uint8_t>(Family::ChipPile) ? "chipPile" : "kerfurOff");
        } else if (j == 5 && n > 10) {
            UE_LOGI("save_identity_map:   ... %zu more ...", n - 10);
        }
    }
    return static_cast<int>(outMap.size());
}

}  // namespace coop::save_identity_map
