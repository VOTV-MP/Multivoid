#include "dev/restore_vitals.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "dev/common.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <windows.h>

#include <atomic>

namespace dev::restore_vitals {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

namespace {

// Cached Session pointer for the broadcast. nullptr-safe at every callsite
// (a press before Session start is logged + no-op'd, NOT crashed).
std::atomic<coop::net::Session*> g_session{nullptr};

// VOTV's vitals top out at 100.0 (% units). Writing >100 doesn't visually
// over-fill the meter -- the BP graph clamps display at 100. Picking the
// exact UI cap rather than something larger so subsequent food consumption
// behaves identically to a player who reached max naturally (no hidden
// over-stored value that would gate hunger draining differently).
constexpr float kMaxVital = 100.0f;

// One-time resolution cache. Set lazily on first press. Cleared / re-resolved
// only if the cached UObject pointer fails IsLive (level transition / hot
// reload). Pre-fix every press walked GUObjectArray TWICE (FindObjectByClass +
// FindClass) plus four FindPropertyOffset calls -- ~100-300 ms of game-thread
// blocking per press = the half-second FPS hitch the user reported. With these
// cached, the hot path is one pointer deref + four float writes.
struct Cache {
    void* mainGameInstance = nullptr;   // live UmainGameInstance_C*
    int32_t saveGameInstOff = -1;       // mainGameInstance_C::save_gameInst (UsaveSlot_C*)
    void* saveSlotClass = nullptr;      // UClass* for UsaveSlot_C (offset lookup target)
    int32_t foodOff = -1;
    int32_t sleepOff = -1;
    int32_t healthOff = -1;
    int32_t coffeePowerOff = -1;
};
Cache g_cache;

bool KeyDown(int vk) { return (::GetAsyncKeyState(vk) & 0x8000) != 0; }

// Lazily resolve the cache. Returns true once every field is filled. Logs a
// one-line warning on the first failure so the user knows which step blocked.
// Called from the game thread (the UObject lookups must be game-thread).
bool EnsureResolved() {
    // (1) GameInstance: live singleton, never destroyed after world boot. Cache
    //     it once; refresh only if the slot was reused for another object.
    if (g_cache.mainGameInstance && !R::IsLive(g_cache.mainGameInstance)) {
        g_cache.mainGameInstance = nullptr;
    }
    if (!g_cache.mainGameInstance) {
        g_cache.mainGameInstance = R::FindObjectByClass(P::name::GameInstanceClass);
        if (!g_cache.mainGameInstance) {
            UE_LOGW("restore_vitals: mainGameInstance_C not yet alive (still booting?)");
            return false;
        }
    }
    // (2) save_gameInst offset on mainGameInstance_C. BP-cooked offsets shift
    //     across recooks (per [[project-adaptation-strategy]]); resolve via
    //     reflection rather than hardcoding 0x01A8 from the SDK dump.
    if (g_cache.saveGameInstOff < 0) {
        void* giClass = R::ClassOf(g_cache.mainGameInstance);
        if (!giClass) {
            UE_LOGW("restore_vitals: mainGameInstance has no UClass");
            return false;
        }
        g_cache.saveGameInstOff = R::FindPropertyOffset(giClass, L"save_gameInst");
        if (g_cache.saveGameInstOff < 0) {
            UE_LOGW("restore_vitals: save_gameInst field not found on mainGameInstance_C");
            return false;
        }
    }
    // (3) saveSlot_C UClass (target for offset lookups).
    if (!g_cache.saveSlotClass) {
        g_cache.saveSlotClass = R::FindClass(P::name::SaveSlotClass);
        if (!g_cache.saveSlotClass) {
            UE_LOGW("restore_vitals: saveSlot_C class unresolvable");
            return false;
        }
    }
    // (4) Four vital field offsets on saveSlot_C. Same BP-cooked rule as (2).
    if (g_cache.foodOff        < 0) g_cache.foodOff        = R::FindPropertyOffset(g_cache.saveSlotClass, L"food");
    if (g_cache.sleepOff       < 0) g_cache.sleepOff       = R::FindPropertyOffset(g_cache.saveSlotClass, L"sleep");
    if (g_cache.healthOff      < 0) g_cache.healthOff      = R::FindPropertyOffset(g_cache.saveSlotClass, L"health");
    if (g_cache.coffeePowerOff < 0) g_cache.coffeePowerOff = R::FindPropertyOffset(g_cache.saveSlotClass, L"coffeePower");
    if (g_cache.foodOff < 0 || g_cache.sleepOff < 0 || g_cache.healthOff < 0 || g_cache.coffeePowerOff < 0) {
        UE_LOGW("restore_vitals: vital offsets unresolved (food=%d sleep=%d health=%d coffeePower=%d)",
                g_cache.foodOff, g_cache.sleepOff, g_cache.healthOff, g_cache.coffeePowerOff);
        return false;
    }
    return true;
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void ApplyLocally() {
    if (!EnsureResolved()) return;
    // Dereference mainGameInstance.save_gameInst -- the CANONICAL live saveSlot
    // pointer used by the rest of the BP graph. Pre-fix used
    // FindObjectByClass(saveSlot_C) which walks GUObjectArray and returns the
    // first non-CDO; in gameplay there's only one saveSlot_C so the value found
    // matched, BUT ui_saveSlots.hpp keeps several UsaveSlot_C* arrays
    // (saves / saves_infront / saves_subslots) that can leave additional
    // instances live after the menu, and the walk would return whichever
    // landed at the lower GUObjectArray index. Pointing through GameInstance
    // bypasses the ambiguity.
    void* slot = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(g_cache.mainGameInstance) + g_cache.saveGameInstOff);
    if (!slot) {
        UE_LOGW("restore_vitals: mainGameInstance.save_gameInst is null (save not registered yet)");
        return;
    }
    auto write = [slot](int32_t off, float v) {
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(slot) + off) = v;
    };
    write(g_cache.foodOff,        kMaxVital);
    write(g_cache.sleepOff,       kMaxVital);
    write(g_cache.healthOff,      kMaxVital);
    write(g_cache.coffeePowerOff, kMaxVital);
    UE_LOGI("restore_vitals: vitals refilled (slot=%p, food/sleep/health/coffeePower = %.1f)",
            slot, kMaxVital);
}

namespace {

// Hotkey + dispatch. Polls F3 (rising edge), applies locally on the game
// thread, and broadcasts a no-payload RestoreVitals reliable. The broadcast
// is best-effort -- if Session isn't connected, the local apply still works
// (the user still gets their refill in solo play).
DWORD WINAPI HotkeyThread(LPVOID) {
    bool prevF3 = false;
    for (;;) {
        // Foreground-window gate per [[feedback-deliver-results-fast]] pattern
        // (also documented in dev::common::IsOurWindowForeground): a same-box
        // host+client test would otherwise fire F3 in BOTH processes from a
        // single keypress because GetAsyncKeyState is global. We want the F3
        // press to be peer-attributed to whichever window has focus.
        const bool f3 = ::dev::IsOurWindowForeground() && KeyDown(VK_F3);
        if (f3 && !prevF3) {
            // Local apply runs on the game thread (saveSlot writes touch BP
            // state). Broadcast is wire-thread-safe.
            GT::Post([] { ApplyLocally(); });
            auto* s = g_session.load(std::memory_order_acquire);
            if (s) {
                // No payload: the action is fixed (max-out the 4 fields).
                const bool sent = s->SendReliable(coop::net::ReliableKind::RestoreVitals, nullptr, 0);
                if (!sent) UE_LOGW("restore_vitals: broadcast failed (channel busy or not connected)");
            }
        }
        prevF3 = f3;
        ::Sleep(8);
    }
}

}  // namespace

void Init() {
    if (!::dev::MasterEnabled()) {
        UE_LOGI("restore_vitals: disabled by master switch ([dev] enabled=0)");
        return;
    }
    if (!::dev::IsIniKeyTrue("devkeys")) {
        UE_LOGI("restore_vitals: disabled (set [dev] devkeys=1 in votv-coop.ini to enable F3)");
        return;
    }
    if (HANDLE t = ::CreateThread(nullptr, 0, &HotkeyThread, nullptr, 0, nullptr)) {
        ::CloseHandle(t);  // detached
    }
    UE_LOGI("restore_vitals: ENABLED (F3 refills food/sleep/health/coffeePower on both peers)");
}

}  // namespace dev::restore_vitals
