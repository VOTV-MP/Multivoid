// coop/save_block.cpp -- see coop/save_block.h.

#include "coop/save/save_block.h"

#include "coop/net/session.h"
#include "ue_wrap/hook.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/sig_scan.h"

#include <windows.h>  // SEH (__try/__except) -- the detour's crash firewall

#include <atomic>
#include <chrono>
#include <cstdint>

namespace coop::save_block {

namespace R = ue_wrap::reflection;
namespace prof = ue_wrap::profile;

namespace {

// Signature of the hooked engine fn:
//   bool UGameplayStatics::SaveGameToSlot(USaveGame* obj, const FString& slot, int32 idx)
// x64: obj=RCX, &slot=RDX (FString*), idx=R8. Returns bool in AL.
using SaveGameToSlotFn = bool(__fastcall*)(void* saveGameObject, void* slotNameFStr,
                                           int32_t userIndex);

// Trampoline to the un-hooked SaveGameToSlot (call to perform a real save).
SaveGameToSlotFn g_original = nullptr;

// The world-save container UClass (saveSlot_C), resolved once at install. Only
// saves whose USaveGame IS-A this class are blocked; save_main_C (meta save:
// keybinds/achievements/store) passes through so client settings still persist.
void* g_saveSlotClass = nullptr;

// Install decision made (client: hooked / host: deliberately skipped). Stops the
// per-tick install pump from re-evaluating once we've acted.
std::atomic<bool> g_done{false};

// --- Part 3 state: the native save-cycle gate (see the header) ---
// The gamemode instance is world-lifetime: IsLiveByIndex-revalidated per tick,
// re-walked (throttled) after a world change. disableSave is a BP BoolProperty --
// resolved via FindBoolProperty (real byte+mask; never a raw whole-byte guess).
void* g_gmCls = nullptr;              // mainGamemode_C UClass (never moves once loaded)
void* g_gm = nullptr;                 // live gamemode instance (this world)
int32_t g_gmIdx = -1;                 // its GUObjectArray index (liveness guard)
int32_t g_disableSaveOff = -1;        // disableSave byte offset within the gamemode
uint8_t g_disableSaveMask = 0;        // ...and its bit mask inside that byte
std::chrono::steady_clock::time_point g_nextGmResolve{};  // re-walk throttle (2 s)

// UE4 FString layout (TArray<TCHAR>): {TCHAR* Data; int32 Num; int32 Max}. Read
// (not written) for the blocked-save log line. Data is null-terminated.
struct FStringView {
    const wchar_t* Data;
    int32_t Num;
    int32_t Max;
};

void LogBlockedSave(void* slotNameFStr, int32_t userIndex) {
    // Throttle: the first few blocks + every 20th. Autosave fires every few
    // minutes so this is naturally sparse, but a forced-save loop shouldn't
    // be able to flood the log.
    static std::atomic<uint64_t> sCount{0};
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 3 && (n % 20) != 0) return;

    const wchar_t* slot = L"<null>";
    const auto* fstr = static_cast<const FStringView*>(slotNameFStr);
    if (fstr && fstr->Data && fstr->Num > 0) slot = fstr->Data;
    UE_LOGI("save_block: BLOCKED client world-save to slot '%ls' (userIndex=%d, block #%llu) "
            "-- host-only persistence; client save left untouched",
            slot, userIndex, static_cast<unsigned long long>(n));
}

// Decision body (has C++ objects via the logging path) -- kept OUT of the SEH
// frame below per the RunDetourSEH pattern (game_thread.cpp): a function holding
// the __try must not require C++ object unwinding.
bool DetourImpl(void* saveGameObject, void* slotNameFStr, int32_t userIndex) {
    void* cls = saveGameObject ? R::ClassOf(saveGameObject) : nullptr;
    const bool isWorldSave =
        cls && g_saveSlotClass && R::IsDescendantOfAny(cls, &g_saveSlotClass, 1);
    if (!isWorldSave) {
        // Meta/settings save (save_main_C) or an unrecognized container -> allow.
        return g_original(saveGameObject, slotNameFStr, userIndex);
    }
    // World save on a coop client -> cancel the write. Return false = "the save
    // did not happen" (honest; the BP save flow treats it as a failed save,
    // which it is). The on-disk .sav is never opened, so the client's pre-coop
    // world save is preserved byte-for-byte.
    LogBlockedSave(slotNameFStr, userIndex);
    return false;
}

// MinHook detour entry. SEH-only (no C++ objects in this frame) so the __try is
// legal -- it is the crash firewall for the decision logic. On ANY fault we fail
// SAFE for a client: block (no disk write), matching the install-side guarantee.
bool __fastcall SaveGameToSlotDetour(void* saveGameObject, void* slotNameFStr,
                                     int32_t userIndex) {
    __try {
        return DetourImpl(saveGameObject, slotNameFStr, userIndex);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;  // faulted deciding -> block (client must not write)
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    if (g_done.load(std::memory_order_acquire)) return;
    if (!session) return;  // session not ready yet -- retry next pump tick

    // Host: never install. The host's save IS the canonical one written during
    // coop; leaving its path un-hooked guarantees zero risk to it (and zero
    // per-save overhead). Latch so we stop re-checking every tick.
    if (session->role() != coop::net::Role::Client) {
        g_done.store(true, std::memory_order_release);
        return;
    }

    // Client: gate the hook on saveSlot_C being loaded so the detour's
    // world-vs-meta discrimination is reliable from its very first fire (a
    // null g_saveSlotClass would mis-classify EVERY save as "not world" and
    // let it through -- the exact hole this module closes). saveSlot_C loads
    // with the save subsystem early; retry quietly until then.
    void* saveSlotCls = R::FindClass(L"saveSlot_C");
    if (!saveSlotCls) return;  // not loaded yet -- retry next tick

    const uintptr_t addr = ue_wrap::FindPattern(prof::kSigSaveGameToSlot);
    if (!addr) {
        // Signature is in the .exe image; a miss means it's stale for this
        // build (version mismatch). Retrying won't help -- log once + give up.
        UE_LOGE("save_block: SaveGameToSlot signature not found -- client world-save "
                "block NOT installed (sdk_profile.h::kSigSaveGameToSlot stale for this build?)");
        g_done.store(true, std::memory_order_release);
        return;
    }

    // Publish the discriminator class BEFORE arming the hook: a save firing in
    // the window between Install() and this assignment would otherwise see a
    // null g_saveSlotClass and slip through. (Saves are rare so the race is
    // near-impossible, but ordering it correctly costs nothing.)
    g_saveSlotClass = saveSlotCls;

    ue_wrap::hook::Init();  // idempotent
    if (!ue_wrap::hook::Install(reinterpret_cast<void*>(addr),
                                reinterpret_cast<void*>(&SaveGameToSlotDetour),
                                reinterpret_cast<void**>(&g_original))) {
        UE_LOGE("save_block: MinHook install on SaveGameToSlot@%p FAILED -- client "
                "world-save block NOT active", reinterpret_cast<void*>(addr));
        g_saveSlotClass = nullptr;
        g_done.store(true, std::memory_order_release);
        return;
    }

    g_done.store(true, std::memory_order_release);
    UE_LOGI("save_block: client world-save block INSTALLED "
            "(SaveGameToSlot@%p, saveSlot_C UClass=%p; save_main_C meta saves pass through)",
            reinterpret_cast<void*>(addr), saveSlotCls);
}

void Tick(coop::net::Session* session) {
    if (!session || session->role() != coop::net::Role::Client) return;

    // Steady state: cached live gamemode -> one masked-bit read, write only on the
    // false->true edge (the bit is ours to hold: no bytecode ever writes it).
    if (g_gm && !R::IsLiveByIndex(g_gm, g_gmIdx)) { g_gm = nullptr; g_gmIdx = -1; }
    if (!g_gm) {
        // No gamemode (menu / join window / world change): the GUObjectArray walk is
        // throttled -- never per tick (the perf rule's forbidden pattern).
        const auto now = std::chrono::steady_clock::now();
        if (now < g_nextGmResolve) return;
        g_nextGmResolve = now + std::chrono::seconds(2);
        if (!g_gmCls) g_gmCls = R::FindClass(prof::name::GamemodeClass);
        if (!g_gmCls) return;
        if (g_disableSaveOff < 0 &&
            !R::FindBoolProperty(g_gmCls, L"disableSave", g_disableSaveOff, g_disableSaveMask)) {
            static bool sWarned = false;
            if (!sWarned) {
                sWarned = true;
                UE_LOGE("save_block: mainGamemode_C.disableSave did not resolve -- native "
                        "save-cycle gate NOT held (disk write-block still active)");
            }
            return;
        }
        for (void* obj : R::FindObjectsByClass(prof::name::GamemodeClass)) {
            if (obj && R::IsLive(obj)) {
                g_gm = obj;
                g_gmIdx = R::InternalIndexOf(obj);
                break;
            }
        }
        if (!g_gm) return;
    }

    auto* p = reinterpret_cast<uint8_t*>(g_gm) + g_disableSaveOff;
    if ((*p & g_disableSaveMask) == 0) {
        *p |= g_disableSaveMask;
        UE_LOGI("save_block: client native save cycle OFF -- disableSave=true on gamemode %p "
                "(+0x%X mask 0x%02X; saveSlot_C::save gates gather+write on it, disk hook "
                "stays as the belt)",
                g_gm, static_cast<unsigned>(g_disableSaveOff), g_disableSaveMask);
    }
}

}  // namespace coop::save_block
