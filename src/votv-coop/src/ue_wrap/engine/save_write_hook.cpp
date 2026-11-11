// ue_wrap/engine/save_write_hook.cpp -- see the header.

#include "ue_wrap/engine/save_write_hook.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/save_folder.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>  // SEH (__try/__except) -- the firewall around the two seams

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <string>

namespace ue_wrap::save_write_hook {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

namespace {

UE_FACT_FN(kRowSaveGameCompressed, "save_write", L"CompressedSaveLoadLib", L"SaveGameCompressed", Declared);

// The world's saves beyond the saveSlot_C container: the area containers, the persistent save and
// the metadata.
constexpr const wchar_t* kCompanionClasses[] = {L"save_container_C", L"save_persistent_C", L"save_metadata_C"};

enum class Kind { Other, World, Companion };

std::atomic<Gate>    g_gate{nullptr};
std::atomic<Written> g_written[kMaxWritten] = {};

save_folder::SaveFrame g_frame;  // where the parameters sit; set before the hook is armed

// Install answered for good: armed, or the function cannot be patched on this build.
std::atomic<bool> g_done{false};
bool g_armed = false;

Kind KindOf(void* saveObject) {
    if (!saveObject) return Kind::Other;
    const std::wstring cls = R::ClassNameOf(saveObject);
    if (_wcsicmp(cls.c_str(), P::name::SaveSlotClass) == 0) return Kind::World;
    for (const wchar_t* companion : kCompanionClasses)
        if (_wcsicmp(cls.c_str(), companion) == 0) return Kind::Companion;
    return Kind::Other;
}

// The string parameter at `offset` of the frame, empty when it holds nothing.
const wchar_t* StringAt(const uint8_t* parms, int32_t offset) {
    const auto* s = reinterpret_cast<const R::FString*>(parms + offset);
    return (s->Data && s->Num > 0) ? s->Data : L"";
}

Write ReadWrite(const uint8_t* parms, Kind kind) {
    Write w{};
    w.object = *reinterpret_cast<void* const*>(parms + g_frame.object);
    w.folder = StringAt(parms, g_frame.folder);
    w.slot = StringAt(parms, g_frame.slot);
    w.extension = StringAt(parms, g_frame.extension);
    w.world = kind == Kind::World;
    return w;
}

bool Decide(const uint8_t* parms) {
    const Kind kind = KindOf(*reinterpret_cast<void* const*>(parms + g_frame.object));
    if (kind == Kind::Other) return true;
    const Gate gate = g_gate.load(std::memory_order_acquire);
    return !gate || gate(ReadWrite(parms, kind));
}

// A fault absorbed by a seam is said, the first few times: a gate that faults cancels every world
// write of a host, and nobody would know why it stopped saving.
void LogSeamFault(const char* seam, unsigned long code, int subscriber = -1) {
    static std::atomic<int> s_said{0};
    if (s_said.fetch_add(1, std::memory_order_relaxed) < 5)
        UE_LOGE("save_write_hook: the %s seam FAULTED (exception 0x%08lX, subscriber %d) -- %s", seam, code,
                subscriber, seam[0] == 'g' ? "this write is cancelled" : "the other subscribers still run");
}

// The two seams run inside SEH-only frames (no C++ objects to unwind, so the __try is legal). The
// engine's own write is NOT inside one: a fault in the engine's save is the engine's to raise, not
// ours to turn into a save that quietly failed.
bool DecideSEH(const uint8_t* parms) {
    __try {
        return Decide(parms);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogSeamFault("gate", GetExceptionCode());
        return false;  // faulted deciding -> do not write
    }
}

// One subscriber per frame, so a fault in one is named and the later ones still hear the write.
void NotifyOneSEH(Written w, int index, const Write& write) {
    __try {
        w(write);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogSeamFault("written", GetExceptionCode(), index);
    }
}

void Notify(const uint8_t* parms) {
    const Write write = ReadWrite(parms, Kind::World);
    for (int i = 0; i < kMaxWritten; ++i)
        if (const Written w = g_written[i].load(std::memory_order_acquire)) NotifyOneSEH(w, i, write);
}

script_gate::Verdict OnPre(void* /*context*/, void* /*source*/, const uint8_t* parms) {
    return DecideSEH(parms) ? script_gate::Verdict::Run : script_gate::Verdict::Cancel;
}

// The engine's answer is the function's bool. A write that reported failure left the file as it was,
// so it is not a moment the world on disk changed.
void OnDone(void* /*context*/, void* /*source*/, const uint8_t* parms, const void* result) {
    const Kind kind = KindOf(*reinterpret_cast<void* const*>(parms + g_frame.object));
    if (kind == Kind::Other) return;
    const bool ok = result && *static_cast<const uint8_t*>(result) != 0;
    const Write write = ReadWrite(parms, kind);
    // The saves are rare (a save is a handful of writes), so each is said: the line shows the seam is standing.
    UE_LOGI("save_write_hook: %ls%ls %ls in '%ls' (%ls)", write.slot, write.extension, ok ? L"written" : L"FAILED",
            write.folder, write.world ? L"the world container" : L"a companion file");
    if (ok && write.world) Notify(parms);
}

}  // namespace

bool Install() {
    if (g_done.load(std::memory_order_acquire)) return g_armed;

    // The library is native and loads with the engine; until it does, the miss is a walk of the object
    // array, retried once a second, not per call.
    static std::chrono::steady_clock::time_point s_nextTry{};
    const auto now = std::chrono::steady_clock::now();
    if (now < s_nextTry) return false;
    if (!R::FindClass(P::name::CompressedSaveLoadLibClass)) { s_nextTry = now + std::chrono::seconds(1); return false; }

    const facts::Resolved fn = facts::Resolve(kRowSaveGameCompressed);
    if (!fn || !save_folder::ResolveSaveFrame(g_frame) || g_frame.function != fn.Fn()) {
        // The library is loaded and the function or its frame is not as expected: a recook, and
        // retrying will not help. The gate is the only thing between a client and a write of the
        // host's world, so this is said loudly.
        UE_LOGE("save_write_hook: CompressedSaveLoadLib::SaveGameCompressed did not resolve to a function of the "
                "expected frame -- NOT installed (a client's world saves are not gated)");
        g_done.store(true, std::memory_order_release);
        return false;
    }
    g_armed = ue_wrap::ufunction_hook::InstallPreHook(fn, &OnPre, /*armed=*/true, &OnDone);
    g_done.store(true, std::memory_order_release);
    if (g_armed)
        UE_LOGI("save_write_hook: INSTALLED (SaveGameCompressed=%p; keybinds and settings saves pass through)",
                fn.Fn());
    else
        UE_LOGE("save_write_hook: the pre hook on SaveGameCompressed=%p was refused -- NOT installed (a client's "
                "world saves are not gated)", fn.Fn());
    return g_armed;
}

void SetGate(Gate gate) { g_gate.store(gate, std::memory_order_release); }

bool AddWritten(Written written) {
    if (!written) return false;
    for (std::atomic<Written>& slot : g_written) {
        Written cur = slot.load(std::memory_order_acquire);
        if (cur == written) return true;
        if (!cur && slot.compare_exchange_strong(cur, written, std::memory_order_acq_rel)) return true;
    }
    // Every slot is taken: a subscriber that never hears a write. Said once, since a caller may retry.
    static std::atomic<bool> s_full{false};
    if (!s_full.exchange(true, std::memory_order_relaxed))
        UE_LOGE("save_write_hook: the written-subscriber table is full (%d) -- a subscriber was refused and "
                "will never hear a world save", kMaxWritten);
    return false;
}

}  // namespace ue_wrap::save_write_hook
