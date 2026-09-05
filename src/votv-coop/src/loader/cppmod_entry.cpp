// loader/cppmod_entry.cpp -- the UE4SS C-ABI loading contract, measured in the three live
// UE4SS sources (the 3.0.1 release, the shimloader build, main): UE4SS loads
// Mods/<name>/dlls/main.dll at mod scan, looks up start_mod and uninstall_mod by name, and
// starts enabled mods by calling start_mod; a mod counts as started when the returned pointer
// is non-null and every callback is null-guarded, so returning null is the clean, era-safe
// refuse. The host never deletes the returned object and never reads or writes its fields, so
// the object is a bare vptr onto no-op stubs, and the layout beyond the vptr is free. Every
// virtual the host can fire returns void in all three eras (slot 0 is the MSVC deleting
// destructor, never invoked); the stubs still zero RAX and XMM0 so a future scalar-returning
// slot reads a deterministic zero, while a future aggregate-returning slot has no universal
// safe stub and is only watched. "Restart All Mods" is uninstall_mod, FreeLibrary, LoadLibrary
// of the same path and start_mod again: the self-pin makes the FreeLibrary a refcount no-op
// (the same module instance, statics surviving) and the boot latch turns the second start_mod
// into a logged no-op returning a fresh dummy, so a live session does not quiesce because a
// debug button was clicked.

#include "loader/cppmod_entry.h"

#include "bootstrap/boot.h"
#include "bootstrap/refuse_dialog.h"
#include "ue_wrap/core/log.h"

#include <windows.h>
#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <string>

// The MASM stub surface (src/loader/cppmod_stubs.asm).
extern "C" {
// 256 identical stubs at a uniform stride; begin and end bracket them so the stride is
// derived, not assumed (the assembler owns the encoding widths).
extern const unsigned char multivoid_cppmod_stubs_begin[];
extern const unsigned char multivoid_cppmod_stubs_end[];
// Per-slot call counters, written by the stubs themselves.
extern volatile long long multivoid_cppmod_slot_counters[256];

// The first-hit reporter, called by the stub (aligned, shadow-spaced) exactly once per slot:
// the attribution line is on disk immediately, so a crash milliseconds after an unexpected
// dispatch still has its evidence flushed.
void MultivoidCppmodSlotFirstHit(uint64_t slot);
}

namespace loader::cppmod {
namespace {

// The highest vtable slot any known era can dispatch (3.0.1 uses slots 0 to 9, the later
// builds 0 to 15; MSVC gives the virtual destructor one slot). A call at or beyond it means
// upstream widened the dispatch surface: a warning, the runtime backstop.
constexpr uint64_t kClassifiedSlots = 16;

// The returned objects: a bare vptr and a magic. A static ring, so restart re-entries hand out
// distinct, forever-valid pointers with no allocation.
struct DummyMod {
    void** vptr;
    uint64_t magic;
};
constexpr uint64_t kDummyMagic = 0x4D756C7469566F69ull;  // "MultiVoi"

void* g_vtable[256];
DummyMod g_dummies[8];
volatile LONG g_dummyIdx = -1;
volatile LONG g_startModCalls = 0;
volatile LONG g_uninstallCalls = 0;

// Build the vtable once, the stride derived from the assembler's real encoding. False means
// the stub block is malformed (uneven stubs) and the caller must refuse rather than hand
// UE4SS a garbage vtable; the built object was verified at a uniform stride by disassembly,
// so this is a tripwire, not a path.
bool EnsureVtable() {
    static LONG state = 0;  // 0 unbuilt, 1 ok, -1 malformed
    if (state == 0) {
        const size_t span = static_cast<size_t>(multivoid_cppmod_stubs_end -
                                                multivoid_cppmod_stubs_begin);
        if (span % 256 != 0) {
            UE_LOGE("cppmod: stub span %zu not divisible by 256 -- vtable NOT built, "
                    "start_mod will refuse", span);
            ::InterlockedExchange(&state, -1);
            return false;
        }
        const size_t stride = span / 256;
        for (size_t i = 0; i < 256; ++i) {
            g_vtable[i] = const_cast<unsigned char*>(multivoid_cppmod_stubs_begin) + i * stride;
        }
        ::InterlockedExchange(&state, 1);
    }
    return state == 1;
}

DummyMod* NextDummy() {
    const LONG idx = ::InterlockedIncrement(&g_dummyIdx) & 7;
    g_dummies[idx].vptr = g_vtable;
    g_dummies[idx].magic = kDummyMagic;
    return &g_dummies[idx];
}

// Predecessor detection, the upgrader path. The old standalone install is an xinput proxy plus
// a multivoid-*.dll beside the exe (or a legacy votv-coop.dll). The direction is fixed: the
// folder mod always defers to a standalone install, so the old build boots normally and keeps
// the session playable while this one refuses with a removal dialog; boot-order-independent,
// since the disk predicate does not care who booted first.

bool NameIsPredecessor(const wchar_t* base) {
    const size_t len = ::wcslen(base);
    if (len > 14 && _wcsnicmp(base, L"multivoid-", 10) == 0 &&
        _wcsicmp(base + len - 4, L".dll") == 0)
        return true;
    return _wcsicmp(base, L"votv-coop.dll") == 0;
}

// The disk leg: multivoid-*.dll, votv-coop.dll or our xinput proxy beside the exe. The refuse
// predicate (deterministic, load-order-independent).
bool ScanDiskPredecessors(std::wstring& outList) {
    wchar_t exePath[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSep = nullptr;
    for (wchar_t* p = exePath; *p; ++p) {
        if (*p == L'\\' || *p == L'/') lastSep = p;
    }
    if (!lastSep) return false;
    lastSep[1] = L'\0';
    const std::wstring dir(exePath);

    bool found = false;
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW((dir + L"multivoid-*.dll").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                if (!outList.empty()) outList += L";";
                outList += fd.cFileName;
                found = true;
            }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    if (::GetFileAttributesW((dir + L"votv-coop.dll").c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (!outList.empty()) outList += L";";
        outList += L"votv-coop.dll";
        found = true;
    }
    // xinput1_3.dll is ambiguous (another mod's proxy, an old UE4SS; anyone can ship one) and with
    // no multivoid payload beside it our proxy has nothing to load, so it is never a refuse
    // trigger by itself. It joins the removal list only when a payload hit above already refused.
    if (found &&
        ::GetFileAttributesW((dir + L"xinput1_3.dll").c_str()) != INVALID_FILE_ATTRIBUTES) {
        outList += L";xinput1_3.dll (the multivoid loader proxy)";
    }
    return found;
}

// The live leg: a predecessor module already mapped (the kernel module list, immune to any
// filesystem virtualisation a mod-manager shim may do). An evidence line; it also refuses on
// its own if the disk leg somehow missed.
bool ScanLivePredecessors(std::wstring& outList) {
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!::K32EnumProcessModules(::GetCurrentProcess(), mods, sizeof(mods), &needed))
        return false;
    const DWORD count = needed / sizeof(HMODULE);
    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ScanLivePredecessors), &self);
    bool found = false;
    for (DWORD i = 0; i < count && i < 1024; ++i) {
        if (mods[i] == self) continue;
        wchar_t path[MAX_PATH] = {};
        if (!::K32GetModuleFileNameExW(::GetCurrentProcess(), mods[i], path, MAX_PATH)) continue;
        const wchar_t* base = path;
        for (const wchar_t* p = path; *p; ++p) {
            if (*p == L'\\' || *p == L'/') base = p + 1;
        }
        if (NameIsPredecessor(base)) {
            if (!outList.empty()) outList += L";";
            outList += base;
            found = true;
        }
    }
    return found;
}

// The refusal dialog lives in bootstrap/refuse_dialog.h: it has a second caller, boot's
// SDK-health refusal, which needs the same property (the overlay must never come up, so a
// Win32 modal is the only surface); the title is the caller's to name.

// The dispatch census watcher: 1 Hz, prints the census line when the set of nonzero slots
// changes (bounded to one reprint per new slot) and exits on process teardown with the
// thread. The first-hit reporter already flushed each slot's attribution line the moment it
// happened; this line is the set view the smoke gate asserts.

volatile LONG g_watcherStarted = 0;

// Two-phase on purpose: the 1 Hz tick computes only the nonzero-set mask (256 volatile reads,
// no allocation); the string is built only when the set changed, at most 257 times in the
// process's life.
void BuildMask(uint64_t (&mask)[4]) {
    for (int i = 0; i < 4; ++i) mask[i] = 0;
    for (int i = 0; i < 256; ++i) {
        if (multivoid_cppmod_slot_counters[i] > 0) mask[i >> 6] |= 1ull << (i & 63);
    }
}

std::string CensusString() {
    std::string s;
    char buf[32];
    for (int i = 0; i < 256; ++i) {
        const long long c = multivoid_cppmod_slot_counters[i];
        if (c <= 0) continue;
        if (!s.empty()) s += " ";
        std::snprintf(buf, sizeof(buf), "%d:%lld", i, c);
        s += buf;
    }
    return s;
}

DWORD WINAPI WatcherThread(LPVOID) {
    uint64_t lastMask[4] = {};
    bool warned = false;
    for (;;) {
        ::Sleep(1000);
        uint64_t mask[4] = {};
        BuildMask(mask);
        bool changed = false;
        for (int i = 0; i < 4; ++i) changed |= (mask[i] != lastMask[i]);
        if (changed) {
            const std::string census = CensusString();
            for (int i = 0; i < 4; ++i) lastMask[i] = mask[i];
            UE_LOGI("cppmod: dispatch census [%s]", census.empty() ? "none" : census.c_str());
            if (!warned) {
                for (size_t slot = kClassifiedSlots; slot < 256; ++slot) {
                    if (multivoid_cppmod_slot_counters[slot] > 0) {
                        UE_LOGW("cppmod: WARN unknown vtable slot %zu dispatched -- upstream "
                                "widened the dispatch surface (wire-e)", slot);
                        warned = true;
                        break;
                    }
                }
            }
            ue_wrap::log::Flush();
        }
    }
}

}  // namespace

void FinalDump() {
    // A no-op unless start_mod was called: the proxy lane's detach must not add a confusing line.
    if (g_startModCalls == 0) return;
    const std::string census = CensusString();
    UE_LOGI("cppmod: final dispatch tally [%s] (start_mod x%ld, uninstall_mod x%ld)",
            census.empty() ? "none" : census.c_str(), g_startModCalls, g_uninstallCalls);
    ue_wrap::log::Flush();
}

}  // namespace loader::cppmod

// The C-ABI exports.

extern "C" void MultivoidCppmodSlotFirstHit(uint64_t slot) {
    // Called by the stub, once per slot, on whatever thread UE4SS dispatched from. The logger is
    // lock-guarded and fixed-buffer, no allocation.
    if (slot >= loader::cppmod::kClassifiedSlots) {
        UE_LOGW("cppmod: WARN unknown vtable slot %llu dispatched (first hit) -- upstream "
                "widened the dispatch surface (wire-e)", (unsigned long long)slot);
    } else {
        UE_LOGI("cppmod: vtable slot %llu first dispatch", (unsigned long long)slot);
    }
    ue_wrap::log::Flush();
}

extern "C" __declspec(dllexport) void* start_mod() {
    using loader::cppmod::NextDummy;
    using loader::cppmod::ScanDiskPredecessors;
    using loader::cppmod::ScanLivePredecessors;

    // Pin first, before any other logic or thread spawn: once pinned, the CppMod destructor's
    // FreeLibrary (the restart path and the refused-instance path) is a refcount no-op, so
    // nothing started below can be unloaded under us.
    {
        HMODULE self = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_PIN,
                             reinterpret_cast<LPCWSTR>(&start_mod), &self);
    }
    ::InterlockedIncrement(&loader::cppmod::g_startModCalls);

    // The stub vtable must exist before any dummy is handed out; a malformed stub block (a
    // tripwire, unreachable by disassembly today) means refuse.
    if (!loader::cppmod::EnsureVtable()) {
        ue_wrap::log::Flush();
        return nullptr;
    }

    // The re-entry short-circuit before the scans: an already-attempted module's start_mod is
    // never a fresh boot, and re-scanning cannot change a running session's reality (a
    // predecessor appearing mid-session must not flip a live mod to refused in UE4SS's
    // bookkeeping). An instance that attempted and was refused stays refused: it has no session
    // to claim.
    if (bootstrap::AlreadyBooted()) {
        if (bootstrap::Started()) {
            bootstrap::StartOnce("cppmod");  // logs the already-booted line
            ue_wrap::log::Flush();
            return loader::cppmod::NextDummy();
        }
        UE_LOGE("cppmod: REFUSE reason=previously-refused -- restart re-entry on a "
                "refused instance stays refused");
        ue_wrap::log::Flush();
        return nullptr;
    }

    // The predecessor scan, only on this lane (the proxy lane is the standalone install and must
    // never refuse itself). Both legs always log their verdict, so the evidence attributes the
    // exact leg.
    std::wstring disk, live;
    const bool diskHit = ScanDiskPredecessors(disk);
    const bool liveHit = ScanLivePredecessors(live);
    if (diskHit)
        UE_LOGE("cppmod: REFUSE reason=predecessor-disk files=[%ls]", disk.c_str());
    else
        UE_LOGI("cppmod: predecessor-disk leg clean");
    if (liveHit)
        UE_LOGE("cppmod: REFUSE reason=predecessor-live modules=[%ls]", live.c_str());
    else
        UE_LOGI("cppmod: predecessor-live leg clean");
    if (diskHit || liveHit) {
        ue_wrap::log::Flush();
        std::wstring body =
            L"Multivoid is installed twice:\n\n"
            L"  - the OLD standalone install (next to the game exe)\n"
            L"  - the NEW UE4SS mod folder (Mods\\Multivoid)\n\n"
            L"The old install stays active this run; the new one refused to start.\n\n"
            L"To finish updating, delete from VotV\\Binaries\\Win64:\n  " +
            (disk.empty() ? live : disk) +
            L"\n\nThen restart the game.";
        bootstrap::ShowRefuseDialog(L"Multivoid -- old install found", body);
        return nullptr;  // measured era-safe: m_is_started=false, all fire_* null-guard
    }

    const bootstrap::StartResult r = bootstrap::StartOnce("cppmod");
    if (r == bootstrap::StartResult::kRefusedDupMutex) {
        ue_wrap::log::Flush();
        bootstrap::ShowRefuseDialog(
            L"Multivoid -- old install found",
            L"Multivoid is installed twice (two mod copies in this game's mod "
            L"folders).\n\nOnly the first copy started. Remove the duplicate "
            L"Multivoid mod folder, then restart the game.");
        return nullptr;
    }
    if (r == bootstrap::StartResult::kRefusedThreadSpawn) {
        // Nothing is running. Refusing is the honest answer to UE4SS, since a dummy would leave it
        // firing callbacks into a mod that never booted. No modal: an OS that cannot spawn one
        // thread will not do better with a second, and the log line carries the reason.
        ue_wrap::log::Flush();
        return nullptr;
    }
    if (r == bootstrap::StartResult::kStarted) {
        if (::InterlockedCompareExchange(&loader::cppmod::g_watcherStarted, 1, 0) == 0) {
            if (HANDLE t = ::CreateThread(nullptr, 0, loader::cppmod::WatcherThread,
                                          nullptr, 0, nullptr)) {
                ::CloseHandle(t);
            }
        }
    }
    // Drain the boot evidence (the entry line and the leg verdicts are info, which the log
    // deliberately leaves buffered): every autonomous teardown is a process kill, and only the
    // refuse legs flushed, so a clean boot's evidence must not depend on a later warning or a
    // vtable first hit happening to drain it.
    ue_wrap::log::Flush();
    // Started and already-booted both hand UE4SS a live dummy: the restart re-entry must read as
    // started in its bookkeeping, since our subsystems never stopped.
    return loader::cppmod::NextDummy();
}

extern "C" __declspec(dllexport) void uninstall_mod(void* mod) {
    // Log only, touching no state, so idempotent for the double fire the host performs (the
    // restart destructor and the final shutdown). A live session does not quiesce because a debug
    // button was clicked; real teardown stays window-close and detach driven, as on the proxy
    // lane.
    const LONG n = ::InterlockedIncrement(&loader::cppmod::g_uninstallCalls);
    UE_LOGI("cppmod: uninstall_mod called (ptr=%p, call #%ld) -- ignored by design", mod, n);
    ue_wrap::log::Flush();
}
