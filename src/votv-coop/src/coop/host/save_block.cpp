// coop/save_block.cpp -- see coop/save_block.h.

#include "coop/host/save_block.h"

#include "coop/net/session.h"
#include "ue_wrap/hook.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/sig_scan.h"

#include <windows.h>  // SEH (__try/__except) -- the detour's crash firewall

#include <atomic>
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

}  // namespace coop::save_block
