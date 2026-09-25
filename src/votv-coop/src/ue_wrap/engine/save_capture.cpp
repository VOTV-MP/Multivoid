// ue_wrap/engine/save_capture.cpp -- see the header.

#include "ue_wrap/engine/save_capture.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/world/world_singleton.h"  // the gamemode, of the CURRENT world

#include <atomic>
#include <cstdint>

namespace ue_wrap::save_capture {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

namespace {

// ---- saveObjects gathers NOTHING while the game says an event is on ---------------------------
// mainGamemode::saveObjects asks lib_C::getEvent before it walks the world, and on true it runs
// the save animation and the held-object save and returns: `objectsData`, `playerTransform`, the
// drone record and the rest are left as the LAST gather wrote them. It runs `Save Primitives` and
// `saveTriggers` on every path, and each opens with the same test and skips ITS gather: three
// gatherers, one gate each. getEvent is `activeEvents > 0` OR the local camera outside the
// 90 000-unit box around the origin. For the game that is "you cannot save during an event"; for
// a capture it is a world from the past handed over as the live one (measured with a badSun event
// running: a joiner's world held three props the host had destroyed two minutes earlier).
// A capture is not a save, so for the span of OUR saveObjects call, and only for the getEvent
// those three functions make, the body is refused at the VM's script loop and its out parameter
// left false. What that lets through (docs/join.md, the save transfer): the gather writes the
// host's LIVE save object mid-event, which the game never does, and event actors that are
// int_save ride the blob; a lane that owns such a class refuses its local spawn on a client.
bool  g_capturing = false;          // game thread: inside our saveObjects call
void* g_saveObjectsFn = nullptr;    // the world gather: the one the gather hook speaks for
void* g_saveTriggersFn = nullptr;
void* g_savePrimitivesFn = nullptr;
void* g_getEventFn = nullptr;
int32_t g_offIsEventActive = -1;
int   g_worldGateRefused = 0;       // per capture: saveObjects' own test
int   g_otherGatesRefused = 0;      // per capture: the two it calls
std::atomic<WorldGatherFn> g_gatherHook{nullptr};

void RaiseGather() {
    if (const WorldGatherFn fn = g_gatherHook.load(std::memory_order_acquire)) fn();
}

script_gate::Verdict OnGetEventPre(const script_gate::Call& c) {
    if (!g_capturing) return script_gate::Verdict::Run;
    // A call with no Blueprint caller (ProcessEvent) names nobody, and an unresolved gatherer is
    // null too: null never matches.
    if (!c.callerFunction) return script_gate::Verdict::Run;
    const bool world = c.callerFunction == g_saveObjectsFn;
    if (!world && c.callerFunction != g_saveTriggersFn && c.callerFunction != g_savePrimitivesFn)
        return script_gate::Verdict::Run;
    if (g_offIsEventActive >= 0)
        if (uint8_t* out = script_gate::OutParamPtr(c, g_offIsEventActive)) *out = 0;
    if (world) { ++g_worldGateRefused; RaiseGather(); }
    else ++g_otherGatesRefused;
    return script_gate::Verdict::Cancel;
}

// The game's own saveObjects (a refused body has no post): the answer getEvent just gave is the
// branch saveObjects takes next.
void OnGetEventPost(const script_gate::Call& c) {
    if (!c.callerFunction || c.callerFunction != g_saveObjectsFn || g_offIsEventActive < 0) return;
    const uint8_t* out = script_gate::OutParamPtr(c, g_offIsEventActive);
    if (out && *out == 0) RaiseGather();
}

}  // namespace

void SetWorldGatherHook(WorldGatherFn fn) { g_gatherHook.store(fn, std::memory_order_release); }

bool InstallGatherWatch() {
    if (g_getEventFn) return true;
    // The cheap preconditions first, and a failure that cannot heal is final: every lookup below
    // walks the object array, and this is retried from a 1 Hz tick for as long as it answers false.
    static bool s_final = false;
    if (s_final || !script_gate::IsInstalled()) return false;
    void* libCls = R::FindClass(L"lib_C");
    void* gmCls = R::FindClass(P::name::GamemodeClass);
    if (!libCls || !gmCls) return false;  // Blueprint classes load with the world: ask again later
    void* fn = R::FindFunction(libCls, L"getEvent");
    void* saveObjects = R::FindFunction(gmCls, P::name::MainGamemodeSaveObjectsFn);
    if (!fn || !saveObjects) {
        // Both classes are loaded and a function is not on them: a recook renamed it.
        s_final = true;
        UE_LOGE("save_capture: lib_C::getEvent=%p mainGamemode::saveObjects=%p -- the gather watch "
                "cannot stand on this build", fn, saveObjects);
        return false;
    }
    g_offIsEventActive = R::FindParamOffset(fn, L"isEventActive");
    g_saveObjectsFn = saveObjects;
    // Either of these missing is a recook's rename: the capture's proof below then reports the
    // gate it could not hold instead of passing a stale half.
    g_saveTriggersFn = R::FindFunction(gmCls, P::name::MainGamemodeSaveTriggersFn);
    g_savePrimitivesFn = R::FindFunction(gmCls, L"Save Primitives");
    if (!script_gate::Watch(fn, /*tag=*/0, &OnGetEventPre, &OnGetEventPost)) {
        s_final = true;  // a full table or a refused function: Watch has said which
        return false;
    }
    g_getEventFn = fn;
    return true;
}

// Asked through the game's own function, and NOT through the watch: "the watch does not stand" is
// exactly when this answer is needed, so it resolves getEvent by itself. Our call arrives with no
// Blueprint caller, so a standing watch lets it run.
EventState GameEventState() {
    // The class default object never dies with a world; the lookup is a name-rendering walk.
    static ue_wrap::CachedObjRef s_libCdo;
    static void* s_getEventFn = nullptr;
    if (!s_libCdo.Alive()) s_libCdo.Set(R::FindClassDefaultObject(L"lib_C"));
    void* libCdo = s_libCdo.Raw();
    if (libCdo && !s_getEventFn) s_getEventFn = R::FindFunction(R::ClassOf(libCdo), L"getEvent");
    void* gm = world_singleton::Gamemode();
    if (!libCdo || !s_getEventFn || !gm) return EventState::Unknown;
    ue_wrap::ParamFrame f(s_getEventFn);
    if (!f.valid()) return EventState::Unknown;
    f.Set<void*>(L"__WorldContext", gm);
    if (!ue_wrap::Call(libCdo, f)) return EventState::Unknown;
    return f.Get<uint8_t>(L"isEventActive") != 0 ? EventState::On : EventState::Off;
}

bool CaptureLiveWorldToScratchSlot(const std::wstring& scratchSlotName) {
    UE_ASSERT_GAME_THREAD("save_capture::CaptureLiveWorldToScratchSlot");
    if (scratchSlotName.empty()) return false;

    // 1. The host gamemode owns the live world and the save container.
    //
    // IT MUST BE THIS WORLD'S GAMEMODE. A dying world's actors are not kill-flagged until the GC
    // purge, which can run tens of seconds behind, so a gamemode found by liveness alone can be the
    // world the game just left; the world singleton hands out only one of the running world.
    //
    // Serializing the stale one produces no torn write but a structurally complete save that
    // describes nothing -- a saveSlot whose object arrays are empty, terminator intact, a kilobyte
    // or so long. A joiner handed that keeps its own world, and the two then disagree on every door
    // and vehicle.
    void* gm = world_singleton::Gamemode();
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
    //    restores). It runs `Save Primitives` and `saveTriggers` itself, on every path. The capture therefore
    //    carries the HOST's player state too: saveObjects writes playerTransform and
    //    inventoryData itself, and the carried items, equipment and hold live in this same
    //    save object. Nothing is stripped here; the joiner replaces what is per-player on its
    //    own side, before its world is built (coop/items/player_inventory_sync). These are pure
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
        //
        // The gather must run whatever the game thinks of events (see OnGetEvent), so this call
        // holds the script gate for its length, beside the session's own hold if there is one.
        const bool watching = InstallGatherWatch();
        bool called = false;
        {
            // Both scoped, so an unwind past the call still lets go: a flag left set would go on
            // refusing the game's event test for the three gatherers.
            struct Capturing {
                Capturing() { g_capturing = true; }
                ~Capturing() { g_capturing = false; }
            };
            script_gate::Hold gate("the save capture");
            g_worldGateRefused = 0;
            g_otherGatesRefused = 0;
            Capturing capturing;
            called = ue_wrap::Call(gm, f);
        }
        if (!called) {
            UE_LOGW("save_capture: mainGamemode.saveObjects dispatch failed -- abort");
            return false;
        }
        // What ran is read off the refusals, not assumed. saveObjects asks its event test on the
        // way into the world gather and nowhere else, so a refusal there IS the gather; the two it
        // calls each ask once.
        if (g_worldGateRefused >= 1 && g_otherGatesRefused >= 2) {
            UE_LOGI("save_capture: the world, trigger and primitive gathers ran with the game's event "
                    "test refused (getEvent: saveObjects %d, the two it calls %d)",
                    g_worldGateRefused, g_otherGatesRefused);
        } else if (g_worldGateRefused >= 1) {
            // The world is current; doors, lights and keypads reach a joiner on their own lanes too.
            UE_LOGE("save_capture: the world gather ran, but saveTriggers / Save Primitives were held "
                    "open %d time(s) of 2 -- a function was renamed by a recook; during an event "
                    "their half of this capture is a previous gather's", g_otherGatesRefused);
        } else if (watching) {
            // Watched, and never asked. By the bytecode that is saveObjects' map test refusing the
            // level before anything else runs (`isSublevelAllowed(level) || subArea == None`, cfg
            // @5 -> @668, "Save error: invalid map"); a fault absorbed in our own callback lands
            // here too. Either way no gather can be shown, and a container that holds a previous
            // gather is not streamed as the live world: the caller has a path that KNOWS it is
            // stale.
            UE_LOGE("save_capture: saveObjects never asked its event test -- it returned before its "
                    "world gather (the game's map test refuses this level?). Refusing this capture.");
            return false;
        } else {
            // No watch, so nothing could be refused and nothing forced: the game decided.
            const EventState ev = GameEventState();
            if (ev != EventState::Off) {
                UE_LOGE("save_capture: the event test could not be watched and %s -- the container "
                        "may hold a PREVIOUS gather. Refusing this capture.",
                        ev == EventState::On ? "an event IS active: saveObjects skipped its gather"
                                             : "the game cannot be asked whether an event is active");
                return false;
            }
            UE_LOGW("save_capture: the event test could not be watched; the game says no event is "
                    "active, so the gathers ran -- a capture during an event will be refused");
        }
    }

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
    //    named or touched. The call passes through ue_wrap/engine/save_to_slot_hook like any
    //    other world save; a listener there tells it from the host's own save by this slot name.
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
