// ue_wrap/engine/save_to_slot_hook.cpp -- see the header.

#include "ue_wrap/engine/save_to_slot_hook.h"

#include "ue_wrap/core/hook.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/scan_ledger.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sig_scan.h"

#include <windows.h>  // SEH (__try/__except) -- the firewall around the two seams

#include <atomic>
#include <chrono>
#include <cstdint>

namespace ue_wrap::save_to_slot_hook {

namespace R = ue_wrap::reflection;
namespace prof = ue_wrap::profile;

namespace {

// Signature of the hooked engine fn:
//   bool UGameplayStatics::SaveGameToSlot(USaveGame* obj, const FString& slot, int32 idx)
// x64: obj=RCX, &slot=RDX (FString*), idx=R8. Returns bool in AL.
using SaveGameToSlotFn = bool(__fastcall*)(void* saveGameObject, void* slotNameFStr,
                                           int32_t userIndex);

// Trampoline to the un-hooked SaveGameToSlot; call it to perform a real save. The name says
// trampoline because that is what this points at -- MinHook's own 64-byte slot, not the engine's
// function -- and freeing the slot corrupts it in place; see hook.h, "Retirement".
SaveGameToSlotFn g_saveGameToSlotTrampoline = nullptr;

// The world-save container UClass (saveSlot_C), published before the detour is armed.
void* g_saveSlotClass = nullptr;

std::atomic<Gate>    g_gate{nullptr};
std::atomic<Written> g_written[kMaxWritten] = {};

// Install answered for good: armed, or the signature is stale for this build.
std::atomic<bool> g_done{false};
bool g_armed = false;

// UE4 FString layout (TArray<TCHAR>): {TCHAR* Data; int32 Num; int32 Max}. Data is
// null-terminated.
struct FStringView {
    const wchar_t* Data;
    int32_t Num;
    int32_t Max;
};

const wchar_t* SlotOf(void* slotNameFStr) {
    const auto* fstr = static_cast<const FStringView*>(slotNameFStr);
    return (fstr && fstr->Data && fstr->Num > 0) ? fstr->Data : L"";
}

enum class Verdict { NotWorld, Write, Cancel };

Verdict Decide(void* saveGameObject, void* slotNameFStr) {
    void* cls = saveGameObject ? R::ClassOf(saveGameObject) : nullptr;
    if (!cls || !g_saveSlotClass || !R::IsDescendantOfAny(cls, &g_saveSlotClass, 1))
        return Verdict::NotWorld;
    const Gate gate = g_gate.load(std::memory_order_acquire);
    return (!gate || gate(saveGameObject, SlotOf(slotNameFStr))) ? Verdict::Write : Verdict::Cancel;
}

// A fault absorbed by a seam is said, the first few times: a gate that faults cancels every world
// save of a host, and nobody would know why it stopped saving.
void LogSeamFault(const char* seam, unsigned long code) {
    static std::atomic<int> s_said{0};
    if (s_said.fetch_add(1, std::memory_order_relaxed) < 5)
        UE_LOGE("save_to_slot_hook: the %s seam FAULTED (exception 0x%08lX) -- %s", seam, code,
                seam[0] == 'g' ? "this world save is cancelled" : "the rest of the notice did not run");
}

// The two seams run inside SEH-only frames (no C++ objects to unwind, so the __try is legal). The
// engine's own write is NOT inside one: a fault in the engine's save is the engine's to raise,
// not ours to turn into a save that quietly failed.
Verdict DecideSEH(void* saveGameObject, void* slotNameFStr) {
    __try {
        return Decide(saveGameObject, slotNameFStr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogSeamFault("gate", GetExceptionCode());
        return Verdict::Cancel;  // faulted deciding -> do not write
    }
}

void NotifySEH(void* saveGameObject, void* slotNameFStr) {
    __try {
        for (const std::atomic<Written>& slot : g_written)
            if (const Written w = slot.load(std::memory_order_acquire)) w(saveGameObject, SlotOf(slotNameFStr));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogSeamFault("written", GetExceptionCode());
    }
}

bool __fastcall SaveGameToSlotDetour(void* saveGameObject, void* slotNameFStr, int32_t userIndex) {
    const Verdict v = DecideSEH(saveGameObject, slotNameFStr);
    if (v == Verdict::Cancel) return false;
    const bool ok = g_saveGameToSlotTrampoline(saveGameObject, slotNameFStr, userIndex);
    if (ok && v == Verdict::Write) NotifySEH(saveGameObject, slotNameFStr);
    return ok;
}

}  // namespace

bool Install() {
    if (g_done.load(std::memory_order_acquire)) return g_armed;

    // Gate the hook on saveSlot_C being loaded, so the world-vs-meta test is reliable from the
    // detour's very first fire. saveSlot_C loads with the save subsystem early; retry until then.
    // The miss is a walk of the object array, so it is retried once a second, not per call.
    static std::chrono::steady_clock::time_point s_nextTry{};
    const auto now = std::chrono::steady_clock::now();
    if (now < s_nextTry) return false;
    void* saveSlotCls = R::FindClass(L"saveSlot_C");
    if (!saveSlotCls) { s_nextTry = now + std::chrono::seconds(1); return false; }

    const uintptr_t addr = ue_wrap::FindPattern(prof::kSigSaveGameToSlot);
    ue_wrap::scan_ledger::Record(ue_wrap::scan_ledger::Scan::SaveGameToSlot, addr != 0);
    if (!addr) {
        // The signature is in the .exe image; a miss means it is stale for this build, and
        // retrying will not help.
        UE_LOGE("save_to_slot_hook: SaveGameToSlot signature not found -- NOT installed "
                "(sdk_profile.h::kSigSaveGameToSlot stale for this build?)");
        g_done.store(true, std::memory_order_release);
        return false;
    }

    // Publish the class BEFORE arming: a save firing between the two would otherwise see a null
    // class and read as "not a world save".
    g_saveSlotClass = saveSlotCls;

    ue_wrap::hook::Init();  // idempotent
    if (!ue_wrap::hook::Install(reinterpret_cast<void*>(addr),
                                reinterpret_cast<void*>(&SaveGameToSlotDetour),
                                reinterpret_cast<void**>(&g_saveGameToSlotTrampoline))) {
        UE_LOGE("save_to_slot_hook: MinHook install on SaveGameToSlot@%p FAILED",
                reinterpret_cast<void*>(addr));
        g_saveSlotClass = nullptr;
        g_done.store(true, std::memory_order_release);
        return false;
    }

    g_armed = true;
    g_done.store(true, std::memory_order_release);
    UE_LOGI("save_to_slot_hook: INSTALLED (SaveGameToSlot@%p, saveSlot_C UClass=%p; save_main_C "
            "meta saves pass through)", reinterpret_cast<void*>(addr), saveSlotCls);
    return true;
}

void SetGate(Gate gate) { g_gate.store(gate, std::memory_order_release); }
bool AddWritten(Written written) {
    if (!written) return false;
    for (std::atomic<Written>& slot : g_written) {
        Written cur = slot.load(std::memory_order_acquire);
        if (cur == written) return true;
        if (!cur && slot.compare_exchange_strong(cur, written, std::memory_order_acq_rel)) return true;
        if (cur == written) return true;
    }
    return false;
}

}  // namespace ue_wrap::save_to_slot_hook
