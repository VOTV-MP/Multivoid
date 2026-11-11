// ue_wrap/engine/save_capture.cpp -- see the header.

#include "ue_wrap/engine/save_capture.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/save_folder.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_singleton.h"  // the gamemode, of the CURRENT world

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ue_wrap::save_capture {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

UE_FACT_FN(kRowGetEvent, "save_capture", L"lib_C", L"getEvent", Declared);
UE_FACT_FN(kRowSaveObjects, "save_capture", L"mainGamemode_C", L"saveObjects", Declared);

namespace {

// ---- when saveObjects gathers -----------------------------------------------------------------
// mainGamemode::saveObjects(quicksave, skipEventBlock) gathers the world -- the object, primitive and
// trigger records -- at once when skipEventBlock is set; otherwise it asks lib_C::getEvent first and
// gathers only on false, leaving the records as the LAST gather wrote them while an event runs
// (getEvent is `activeEvents > 0` OR the local camera outside the box around the origin). Both
// branches are watched: the call's own skipEventBlock at its entry, and the answer getEvent gives
// it.
void* g_saveObjectsFn = nullptr;
int32_t g_offSkipEventBlock = -1;
int32_t g_offIsEventActive = -1;
void* g_getEventFn = nullptr;
std::atomic<WorldGatherFn> g_gatherHook{nullptr};

void RaiseGather() {
    if (const WorldGatherFn fn = g_gatherHook.load(std::memory_order_acquire)) fn();
}

script_gate::Verdict OnSaveObjectsPre(const script_gate::Call& c) {
    if (g_offSkipEventBlock >= 0 && c.locals && c.locals[g_offSkipEventBlock] != 0) RaiseGather();
    return script_gate::Verdict::Run;
}

void OnGetEventPost(const script_gate::Call& c) {
    if (!c.callerFunction || c.callerFunction != g_saveObjectsFn || g_offIsEventActive < 0) return;
    const uint8_t* out = script_gate::OutParamPtr(c, g_offIsEventActive);
    if (out && *out == 0) RaiseGather();
}

// A field of `obj` holding an object pointer, by name; null when undeclared.
void* ObjectField(void* obj, const wchar_t* prop) {
    void* cls = obj ? R::ClassOf(obj) : nullptr;
    const int32_t off = cls ? R::FindPropertyOffset(cls, prop) : -1;
    return off >= 0 ? *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(obj) + off) : nullptr;
}

// A string field of `obj`, by name; empty when undeclared or unset.
std::wstring StringField(void* obj, const wchar_t* prop) {
    void* cls = obj ? R::ClassOf(obj) : nullptr;
    const int32_t off = cls ? R::FindPropertyOffset(cls, prop) : -1;
    if (off < 0) return {};
    const auto& s = *reinterpret_cast<const R::FString*>(static_cast<const uint8_t*>(obj) + off);
    return (s.Data && s.Num > 1) ? std::wstring(s.Data, s.Data + (s.Num - 1)) : std::wstring();
}

// The live world's save file in `path`, where the game itself keeps it. Outside an area that is
// main.sav. Inside one the live saveSlot_C is the area's (lib_C::createAreaSlot swapped it in for the
// main world's) and the game saves it as areas/<area>.sav, leaving main.sav as the last save outside
// the area wrote it: so main.sav is the host's own copy of that, which the joiner's load reads and
// the area's slot replaces.
bool WriteWorldFile(void* saveSlot, void* gameInstance, const std::wstring& path, const std::wstring& area) {
    if (area.empty()) return save_folder::SaveCompressed(saveSlot, path, L"main", L".sav");
    namespace fs = std::filesystem;
    const std::wstring areaDir = save_folder::AreaFolder(path);
    std::error_code ec;
    fs::create_directories(fs::path(areaDir), ec);
    if (!save_folder::SaveCompressed(saveSlot, areaDir, area, L".sav")) return false;
    const std::wstring hostFolder = StringField(gameInstance, L"folderSlotName");
    if (hostFolder.empty()) {
        UE_LOGW("save_capture: the host is in area '%ls' and its GameInstance names no save folder -- there is "
                "no main.sav to send with the area", area.c_str());
        return false;
    }
    fs::copy_file(fs::path(save_folder::FolderPath(hostFolder)) / L"main.sav", fs::path(path) / L"main.sav",
                  fs::copy_options::overwrite_existing, ec);
    if (ec)
        UE_LOGW("save_capture: copying the host's main.sav of '%ls' failed (%s)", hostFolder.c_str(),
                ec.message().c_str());
    return !ec;
}

}  // namespace

void SetWorldGatherHook(WorldGatherFn fn) { g_gatherHook.store(fn, std::memory_order_release); }

bool InstallGatherWatch() {
    if (g_getEventFn) return true;
    // The cheap preconditions first, and a failure that cannot heal is final: every lookup below
    // walks the object array, and this is retried from a 1 Hz tick for as long as it answers false.
    static bool s_final = false;
    if (s_final || !script_gate::IsInstalled()) return false;
    if (!R::FindClass(L"lib_C") || !R::FindClass(P::name::GamemodeClass))
        return false;  // Blueprint classes load with the world: ask again later
    const facts::Resolved getEvent = facts::Resolve(kRowGetEvent);
    const facts::Resolved saveObjects = facts::Resolve(kRowSaveObjects);
    if (!getEvent.Fn() || !saveObjects.Fn()) {
        // Both classes are loaded and a function is not on them: a recook renamed it.
        s_final = true;
        UE_LOGE("save_capture: lib_C::getEvent=%p mainGamemode::saveObjects=%p -- the gather watch "
                "cannot stand on this build", getEvent.Fn(), saveObjects.Fn());
        return false;
    }
    g_offIsEventActive = R::FindParamOffset(getEvent.Fn(), L"isEventActive");
    g_offSkipEventBlock = R::FindParamOffset(saveObjects.Fn(), L"skipEventBlock");
    g_saveObjectsFn = saveObjects.Fn();
    if (!script_gate::Watch(saveObjects, /*tag=*/0, &OnSaveObjectsPre, nullptr) ||
        !script_gate::Watch(getEvent, /*tag=*/0, nullptr, &OnGetEventPost)) {
        s_final = true;  // a full table or a refused function: Watch has said which
        return false;
    }
    g_getEventFn = getEvent.Fn();
    return true;
}

// Asked through the game's own function, never through the watch: "the watch does not stand" is
// exactly when this answer is needed. Our call arrives with no Blueprint caller.
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

bool CaptureLiveWorldToFolder(const std::wstring& folderName, std::wstring& area) {
    area.clear();
    UE_ASSERT_GAME_THREAD("save_capture::CaptureLiveWorldToFolder");
    if (folderName.empty()) return false;

    // 1. The host gamemode owns the live world and the save objects.
    //
    // IT MUST BE THIS WORLD'S GAMEMODE. A dying world's actors are not kill-flagged until the GC
    // purge, which can run tens of seconds behind, so a gamemode found by liveness alone can be the
    // world the game just left; the world singleton hands out only one of the running world. The
    // stale one serializes a structurally complete save that describes nothing, and a joiner handed
    // it disagrees with the host on every door and vehicle.
    void* gm = world_singleton::Gamemode();
    void* gi = world_singleton::GameInstance();
    if (!gm || !gi) {
        UE_LOGW("save_capture: no mainGamemode belonging to the CURRENT world -- refusing to "
                "capture (a stale one would serialize an empty world)");
        return false;
    }
    void* saveSlot = ObjectField(gm, L"save_slot");
    void* persistent = ObjectField(gm, L"save_persistent");
    void* metadata = ObjectField(gi, L"metadataObject");
    if (!saveSlot || !persistent || !metadata) {
        UE_LOGW("save_capture: a save object is missing (save_slot=%p save_persistent=%p metadataObject=%p) -- "
                "nothing to serialize", saveSlot, persistent, metadata);
        return false;
    }
    const int32_t recordsOff = R::FindPropertyOffset(R::ClassOf(saveSlot), L"betterObjectsData");
    auto recordCount = [&]() -> int32_t {
        if (recordsOff < 0) return -1;
        return *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(saveSlot) + recordsOff + 0x8);
    };
    const int32_t before = recordCount();

    // 2. Repopulate the in-memory world save from LIVE actors: saveObjects(quicksave=false,
    //    skipEventBlock=true), the game's own gather with its own event test skipped, so a capture
    //    during an event carries the world as it is. It clears and refills the object, primitive and
    //    trigger records and writes the host's player transform; the joiner replaces what is
    //    per-player on its own side, before its world is built (coop/items/player_inventory_sync).
    //    No actor is touched and nothing is written to disk by it.
    void* fn = R::FindFunction(R::ClassOf(gm), P::name::MainGamemodeSaveObjectsFn);
    if (!fn) {
        UE_LOGW("save_capture: mainGamemode.saveObjects unresolved -- abort (serializing now would ship a "
                "stale object list)");
        return false;
    }
    InstallGatherWatch();  // the watch raises the gather hook for this call too
    {
        ue_wrap::ParamFrame f(fn);
        if (!f.valid() || !f.Set<bool>(L"skipEventBlock", true) || !ue_wrap::Call(gm, f)) {
            UE_LOGW("save_capture: mainGamemode.saveObjects dispatch failed -- abort");
            return false;
        }
    }
    const int32_t after = recordCount();
    UE_LOGI("save_capture: betterObjectsData repopulated %d -> %d live world record(s)", before, after);
    // AN EMPTY CONTAINER IS NOT A WORLD: a healthy capture holds thousands of records, the
    // stale-gamemode stub none. Refusing here means the caller falls back to the folder on disk.
    if (after <= 0) {
        UE_LOGW("save_capture: saveObjects produced no object records -- refusing this capture");
        return false;
    }

    // 3. Serialize the files a load needs into the scratch folder, through the game's own
    //    compressed writer. The folder is OURS; the player's own save folder is never written.
    const std::wstring path = save_folder::FolderPath(folderName);
    if (path.empty()) {
        UE_LOGW("save_capture: the saved directory is unresolved -- cannot serialize");
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path), ec);
    area = StringField(metadata, L"area");
    const bool ok = WriteWorldFile(saveSlot, gi, path, area) &&
                    save_folder::SaveCompressed(persistent, path, L"persistent", L".sav") &&
                    save_folder::SaveCompressed(metadata, path, L"main", L".meta");
    if (ok)
        UE_LOGI("save_capture: host world serialized LIVE to '%ls' (area '%ls')", path.c_str(), area.c_str());
    else
        UE_LOGW("save_capture: writing the capture to '%ls' failed", path.c_str());
    return ok;
}

}  // namespace ue_wrap::save_capture
