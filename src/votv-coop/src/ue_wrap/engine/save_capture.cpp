// ue_wrap/engine/save_capture.cpp -- see the header.

#include "ue_wrap/engine/save_capture.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/world_identity.h"  // the gamemode must belong to the CURRENT world

#include <cstdint>

namespace ue_wrap::save_capture {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

namespace {

// Dispatch a no-argument mainGamemode UFunction by name. Returns false (logged) if
// the function is unresolved or the ProcessEvent dispatch fails.
bool CallGmVoid(void* gm, void* gmCls, const wchar_t* fnName) {
    void* fn = R::FindFunction(gmCls, fnName);
    if (!fn) {
        UE_LOGW("save_capture: mainGamemode.%ls unresolved", fnName);
        return false;
    }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    return ue_wrap::Call(gm, f);
}

}  // namespace

bool CaptureLiveWorldToScratchSlot(const std::wstring& scratchSlotName) {
    UE_ASSERT_GAME_THREAD("save_capture::CaptureLiveWorldToScratchSlot");
    if (scratchSlotName.empty()) return false;

    // 1. The host gamemode owns the live world and the save container.
    //
    // IT MUST BE THIS WORLD'S GAMEMODE. FindObjectsByClass skips only nulls and the CDO -- no
    // liveness test, no world filter -- and answers in GUObjectArray index order. A dying world's
    // actors are not kill-flagged until the GC purge, which can run tens of seconds behind, and
    // after a menu-to-game cycle the OLD mainGamemode sits at a LOWER index than the new one. So
    // the scan reads candidates until one names this world, rather than judging the first and
    // stopping: judging only the first turns the very case this guard exists for into a refusal to
    // capture, since the stale gamemode fails the world test and the live one is never reached.
    //
    // Serializing the stale one produces no torn write but a structurally complete save that
    // describes nothing -- a saveSlot whose object arrays are empty, terminator intact, a kilobyte
    // or so long. A joiner handed that keeps its own world, and the two then disagree on every door
    // and vehicle.
    void* gm = nullptr;
    void* const nowWorld = ::ue_wrap::world_identity::CurrentWorld();
    for (void* cand : R::FindObjectsByClass(P::name::GamemodeClass)) {
        // With no current world there is nothing to judge against -- boot, mid-travel, or a
        // recook that broke the world lookup, in which case WorldOf answers null for everything
        // too -- so take the first candidate, which is what an unfiltered scan would return.
        if (!nowWorld) { gm = cand; break; }
        // Otherwise the stamp must name THIS world. A null stamp is a rejection here, not a
        // shrug: elsewhere null means "not world-scoped", but that answer belongs to classes,
        // CDOs and assets, whose outer chain reaches a package. A gamemode INSTANCE is outered
        // to its level in one hop, so the only way it stamps null is that the level's owning
        // world has already been nulled -- which names a torn-down world, the very thing being
        // excluded.
        if (::ue_wrap::world_identity::WorldOf(cand) == nowWorld) { gm = cand; break; }
    }
    void* gmCls = gm ? R::ClassOf(gm) : nullptr;
    if (!gm || !gmCls) {
        UE_LOGW("save_capture: no mainGamemode belonging to the CURRENT world -- refusing to "
                "capture (a stale one would serialize an empty world)");
        return false;
    }

    // 2. The world save container the populate writes into. Read it BEFORE the populate so we
    //    can probe objectsData's count delta. That delta is also the one safety check this design
    //    needs: saveObjects MUST rebuild objectsData rather than append for a standalone call to
    //    be correct. It does rebuild -- it fills a local struct_save array and then assigns that
    //    to the member, the build-local-then-replace shape -- and a single-player save/reload
    //    never duplicates the world, which it would if the array only ever grew. The probe below
    //    confirms it live; a >1.5x growth would mean it appends, and then we would have to clear
    //    first. The failure mode is host-safe either way, since a doubled scratch blob
    //    over-populates only the JOINER and the host's slot is never written.
    void* saveSlot = *reinterpret_cast<void* const*>(
        reinterpret_cast<const uint8_t*>(gm) + P::off::AmainGamemode_saveSlot);
    if (!saveSlot) {
        UE_LOGW("save_capture: mainGamemode.saveSlot is null -- nothing to serialize");
        return false;
    }
    // objectsData is a UE TArray {Data@0x0, Num@0x8, Max@0xC}; read its Num.
    auto objectsDataNum = [saveSlot]() -> int32_t {
        return *reinterpret_cast<const int32_t*>(
            reinterpret_cast<const uint8_t*>(saveSlot) + P::off::UsaveSlot_objectsData + 0x8);
    };
    const int32_t objCountBefore = objectsDataNum();

    // 3. Repopulate the in-memory world save from LIVE actors. saveObjects is the critical
    //    step: it walks every int_save_C world actor (props + NPCs, including a turned-on kerfur,
    //    which serializes as its live NPC state -- exactly what a single-player save/reload
    //    restores). saveTriggers refreshes door/light/keypad states. We deliberately SKIP the
    //    player-state populates (playerTransform/inventory/heldObj): the joiner overrides those
    //    with its own per-player state and each has a live coop sync channel. These are pure
    //    read-into-array populates -- no actor mutation, no disk write, no save event -- so
    //    nothing "real" happens to the host's session here.
    {
        void* fn = R::FindFunction(gmCls, P::name::MainGamemodeSaveObjectsFn);
        if (!fn) {
            UE_LOGW("save_capture: mainGamemode.saveObjects unresolved -- abort "
                    "(serializing now would ship a stale object list)");
            return false;
        }
        ue_wrap::ParamFrame f(fn);
        if (!f.valid()) return false;
        // saveObjects(bool quicksave): the zeroed frame leaves quicksave=false (the
        // full populate, matching a normal save) -- exactly what we want.
        if (!ue_wrap::Call(gm, f)) {
            UE_LOGW("save_capture: mainGamemode.saveObjects dispatch failed -- abort");
            return false;
        }
    }
    CallGmVoid(gm, gmCls, P::name::MainGamemodeSaveTriggersFn);  // best-effort; objects are the critical half

    // Safety probe: confirm saveObjects rebuilt (didn't append) objectsData.
    const int32_t objCountAfter = objectsDataNum();
    if (objCountBefore > 0 && objCountAfter > objCountBefore + objCountBefore / 2) {
        UE_LOGW("save_capture: objectsData grew %d -> %d (>1.5x) -- saveObjects appears to APPEND, "
                "not rebuild; the scratch blob may carry DUPLICATE objects. Investigate before "
                "trusting this build's live capture.", objCountBefore, objCountAfter);
    } else {
        UE_LOGI("save_capture: objectsData repopulated %d -> %d live world object(s)",
                objCountBefore, objCountAfter);
    }
    // AN EMPTY CONTAINER IS NOT A WORLD, and this count is the exact discriminator: an integer
    // read at the producer, rather than the byte-size ratio a consumer would be left to guess
    // with. A healthy capture repopulates a few thousand objects; the stale-gamemode stub reads
    // zero. Refusing here means the caller falls back to the canonical slot instead of streaming
    // a world nobody lives in.
    if (objCountAfter == 0) {
        UE_LOGW("save_capture: saveObjects produced an EMPTY objectsData (%d -> 0) -- refusing "
                "this capture. The container serialized fine; it just describes no world.",
                objCountBefore);
        return false;
    }

    // 4. Serialize it to a SCRATCH slot. GameplayStatics::SaveGameToSlot(obj, slot,
    //    idx): the slot NAME is our parameter, so the host's canonical slot is never
    //    named or touched. On the host SaveGameToSlot is un-hooked (coop::save_block
    //    installs on clients only), so this runs as the stock engine serializer.
    void* gsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    void* gsCls = gsCdo ? R::ClassOf(gsCdo) : nullptr;
    void* saveFn = gsCls ? R::FindFunction(gsCls, P::name::SaveGameToSlotFn) : nullptr;
    if (!gsCdo || !saveFn) {
        UE_LOGW("save_capture: GameplayStatics::SaveGameToSlot unresolved -- cannot serialize");
        return false;
    }

    std::wstring slotBuf = scratchSlotName;  // kept alive across the Call (the FString aliases it)
    R::FString fs{};
    fs.Data = slotBuf.data();
    fs.Num  = static_cast<int32_t>(slotBuf.size()) + 1;  // FString::Num counts the null terminator
    fs.Max  = fs.Num;

    ue_wrap::ParamFrame f(saveFn);
    if (!f.valid()) return false;
    f.Set<void*>(L"SaveGameObject", saveSlot);
    f.SetRaw(L"SlotName", &fs, sizeof(fs));
    f.Set<int32_t>(L"UserIndex", 0);
    if (!ue_wrap::Call(gsCdo, f)) {
        UE_LOGW("save_capture: SaveGameToSlot dispatch failed");
        return false;
    }
    const bool ok = f.Get<uint8_t>(L"ReturnValue") != 0;
    if (ok) {
        UE_LOGI("save_capture: host world serialized LIVE to scratch slot '%ls' "
                "(objects+triggers repopulated; canonical slot untouched)",
                scratchSlotName.c_str());
    } else {
        UE_LOGW("save_capture: SaveGameToSlot returned false for scratch slot '%ls'",
                scratchSlotName.c_str());
    }
    return ok;
}

}  // namespace ue_wrap::save_capture
