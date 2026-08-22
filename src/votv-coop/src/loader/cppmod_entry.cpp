// loader/cppmod_entry.cpp -- the UE4SS C-ABI loading contract (D-3 SLIM CONTRACT).
//
// Design of record: research/findings/tooling/votv-ue4ss-f2-migration-DESIGN-2026-08-21.md
// SS2, + the 10-round spike impl audit (2026-08-21, qf_thread.md). The contract,
// measured in ALL THREE live era sources (v3.0.1 / e31aaaa6 2026-02-03 / main
// 7f7cc36 2026-05, UE4SS/src/Mod/CppMod.cpp each):
//   - UE4SS LoadLibrary's Mods/<name>/dlls/main.dll at mod-scan, GetProcAddress's
//     "start_mod" + "uninstall_mod" BY NAME, and starts enabled mods by calling
//     start_mod(). m_is_started = (returned ptr != nullptr); EVERY fire_* is
//     null-guarded -> returning nullptr is the clean, era-safe REFUSE.
//   - The host NEVER deletes the returned object, never reads or writes its
//     fields (GUITabs is touched only inside their own base-class member
//     functions a non-deriving object never invokes) -> the object is a bare
//     vptr onto no-op stubs; layout beyond the vptr is free.
//   - Every virtual the host can fire returns VOID in all three eras (slot 0 is
//     the MSVC scalar-deleting dtor, never invoked). The stubs still zero
//     RAX+XMM0 so a FUTURE scalar-returning slot reads deterministic 0/0.0/
//     false/nullptr. A future sret (aggregate-return) slot has NO universal
//     safe stub -- that coupling is WATCHED (tripwire wire-e), not solved.
//   - "Restart All Mods" (main-era GUI) = uninstall_mod(ptr) -> CppMod dtor
//     FreeLibrary -> re-LoadLibrary same path -> start_mod again. The self-PIN
//     makes the FreeLibrary a refcount no-op (same module instance, statics
//     survive), and bootstrap::StartOnce's latch turns the second start_mod
//     into a logged no-op returning a fresh dummy: a live multiplayer session
//     does not quiesce because a debug button was clicked.

#include "loader/cppmod_entry.h"

#include "bootstrap/boot.h"
#include "ue_wrap/core/log.h"

#include <windows.h>
#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <string>

// ---- MASM stub surface (src/loader/cppmod_stubs.asm) -----------------------
extern "C" {
// 256 identical-shape stubs at a uniform stride; begin/end bracket them so the
// stride is DERIVED, not assumed (the assembler owns the encoding widths).
extern const unsigned char multivoid_cppmod_stubs_begin[];
extern const unsigned char multivoid_cppmod_stubs_end[];
// Per-slot call counters + first-hit bits, written by the stubs themselves.
extern volatile long long multivoid_cppmod_slot_counters[256];

// First-hit reporter, called BY the stub (aligned, shadow-spaced) exactly once
// per slot: the attribution line is on disk immediately, so a crash milliseconds
// after an unexpected dispatch still has its evidence flushed.
void MultivoidCppmodSlotFirstHit(uint64_t slot);
}

namespace loader::cppmod {
namespace {

// Highest vtable slot any KNOWN era can dispatch (v3.0.1 = slots 0..9,
// e31aaaa6/main = 0..15; MSVC gives the virtual dtor ONE slot). A call at or
// beyond this index means upstream widened the dispatch surface -> WARN
// (tripwire wire-e's runtime backstop).
constexpr uint64_t kClassifiedSlots = 16;

// The returned objects: a bare vptr + magic. Static ring so restart re-entries
// hand out distinct, forever-valid pointers with no allocation.
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

// Build the vtable once; stride derived from the assembler's real encoding.
// FALSE = the stub block is malformed (REPT emitted uneven stubs) -- callers
// must REFUSE (return nullptr) rather than hand UE4SS a garbage vtable.
// Compile-time-constant behavior; the built object was disasm-verified at a
// uniform 48-byte stride (audit 2026-08-21), so this is a tripwire, not a path.
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

// ---- predecessor detection (the upgrader path) ------------------------------
// The OLD standalone install = xinput proxy + multivoid-*.dll BESIDE THE EXE
// (or a legacy votv-coop.dll). Direction is fixed: the folder-mod DEFERS to a
// standalone install always -- the old build boots normally and keeps the
// session playable; WE refuse with a removal dialog. Boot-order-independent
// because the DISK predicate does not care who booted first.

bool NameIsPredecessor(const wchar_t* base) {
    const size_t len = ::wcslen(base);
    if (len > 14 && _wcsnicmp(base, L"multivoid-", 10) == 0 &&
        _wcsicmp(base + len - 4, L".dll") == 0)
        return true;
    return _wcsicmp(base, L"votv-coop.dll") == 0;
}

// Disk leg: multivoid-*.dll / votv-coop.dll / our xinput proxy beside the exe.
// THE refuse predicate (deterministic, load-order-independent).
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
    // xinput1_3.dll is AMBIGUOUS (another mod's proxy, an old UE4SS 2.5.2 --
    // anyone can ship one) and with no multivoid payload beside it OUR proxy
    // has nothing to load: never a refuse trigger by itself. It joins the
    // removal list only when a payload hit above already refused.
    if (found &&
        ::GetFileAttributesW((dir + L"xinput1_3.dll").c_str()) != INVALID_FILE_ATTRIBUTES) {
        outList += L";xinput1_3.dll (the multivoid loader proxy)";
    }
    return found;
}

// Live leg: a predecessor MODULE already mapped (kernel module list -- immune
// to any filesystem virtualization a mod-manager shim may do). Evidence line;
// also refuses on its own if the disk leg somehow missed.
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

// Refusal dialog on its own detached thread: start_mod must return promptly
// (no UE4SS thread ever blocks on the modal), and the PIN taken before any of
// this guarantees the thread's code cannot be unloaded under it. NOT the
// in-game boot_warning_dialog -- that renders from our overlay, which a
// refused instance must never install.
DWORD WINAPI RefuseDialogThread(LPVOID raw) {
    std::wstring* msg = static_cast<std::wstring*>(raw);
    // The up/dismissed lines are the headless drills' evidence that the dialog
    // REALLY showed (a window-probe from outside proved unreliable; the log is
    // authoritative and kill-safe because both lines flush).
    UE_LOGI("cppmod: refuse dialog up (tid=%lu)", ::GetCurrentThreadId());
    ue_wrap::log::Flush();
    ::MessageBoxW(nullptr, msg->c_str(), L"Multivoid -- old install found",
                  MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    UE_LOGI("cppmod: refuse dialog dismissed");
    ue_wrap::log::Flush();
    delete msg;
    return 0;
}

void ShowRefuseDialog(const std::wstring& body) {
    auto* msg = new std::wstring(body);
    if (HANDLE t = ::CreateThread(nullptr, 0, RefuseDialogThread, msg, 0, nullptr)) {
        ::CloseHandle(t);
    } else {
        delete msg;
    }
}

// ---- dispatch census watcher ------------------------------------------------
// 1 Hz; prints the census line when the SET of nonzero slots changes (bounded:
// at most one reprint per new slot) and exits on process teardown with the
// thread. The first-hit reporter already flushed each slot's attribution line
// the moment it happened; this line is the SET view the smoke gate asserts.

volatile LONG g_watcherStarted = 0;

// Two-phase on purpose (audit F2): the 1 Hz tick computes only the nonzero-set
// MASK (256 volatile reads, no alloc); the string is built ONLY when the set
// changed (bounded: at most 257 builds process-life).
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
    // No-op unless the cppmod lane actually ran (counters written or start_mod
    // called): the proxy lane's DETACH must not add a confusing line.
    if (g_startModCalls == 0) return;
    const std::string census = CensusString();
    UE_LOGI("cppmod: final dispatch tally [%s] (start_mod x%ld, uninstall_mod x%ld)",
            census.empty() ? "none" : census.c_str(), g_startModCalls, g_uninstallCalls);
    ue_wrap::log::Flush();
}

}  // namespace loader::cppmod

// ---- the C-ABI exports ------------------------------------------------------

extern "C" void MultivoidCppmodSlotFirstHit(uint64_t slot) {
    // Called by the stub, once per slot, on whatever thread UE4SS dispatched
    // from. Logger is CS-locked, fixed-buffer, no allocation.
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

    // PIN FIRST, before any other logic or thread spawn: once pinned, the
    // CppMod dtor's FreeLibrary (restart path AND the refused-instance path)
    // is a refcount no-op, so nothing we start below can be unloaded under us.
    {
        HMODULE self = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_PIN,
                             reinterpret_cast<LPCWSTR>(&start_mod), &self);
    }
    ::InterlockedIncrement(&loader::cppmod::g_startModCalls);

    // The stub vtable must exist before ANY dummy is handed out; a malformed
    // stub block (tripwire, disasm-verified unreachable today) means REFUSE.
    if (!loader::cppmod::EnsureVtable()) {
        ue_wrap::log::Flush();
        return nullptr;
    }

    // Re-entry short-circuit BEFORE the scans: an already-attempted module's
    // start_mod is never a fresh boot, and re-scanning cannot change a running
    // session's reality (a predecessor appearing mid-session must not flip a
    // live mod to refused in UE4SS's bookkeeping). Audit F3: an instance that
    // attempted but was REFUSED stays refused -- it has no session to claim.
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

    // Predecessor scan -- ONLY on this lane (the proxy lane IS the standalone
    // install and must never self-refuse). Both legs always log their verdict
    // so the evidence attributes the exact leg.
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
        loader::cppmod::ShowRefuseDialog(body);
        return nullptr;  // measured era-safe: m_is_started=false, all fire_* null-guard
    }

    const bootstrap::StartResult r = bootstrap::StartOnce("cppmod");
    if (r == bootstrap::StartResult::kRefusedDupMutex) {
        ue_wrap::log::Flush();
        loader::cppmod::ShowRefuseDialog(
            L"Multivoid is installed twice (two mod copies in this game's mod "
            L"folders).\n\nOnly the first copy started. Remove the duplicate "
            L"Multivoid mod folder, then restart the game.");
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
    // Drain the boot evidence (the entry= line and the leg verdicts are INFO,
    // which log.cpp deliberately leaves buffered): every autonomous teardown is
    // TerminateProcess, and only the refuse legs flushed -- a clean boot's
    // evidence must not depend on a later WARN or a vtable first-hit happening
    // to drain it (lesson-kill-teardown-discards-buffered-info-log-lines).
    ue_wrap::log::Flush();
    // kStarted and kAlreadyBooted both hand UE4SS a live dummy: the restart
    // re-entry must read as STARTED in their bookkeeping (m_is_started) --
    // our subsystems never stopped.
    return loader::cppmod::NextDummy();
}

extern "C" __declspec(dllexport) void uninstall_mod(void* mod) {
    // Log-only, touches no state -> idempotent for the double-fire the host
    // performs (restart dtor + final shutdown). A live multiplayer session
    // does not quiesce because a debug button was clicked; real teardown stays
    // WM_CLOSE/DETACH-driven exactly as on the proxy lane.
    const LONG n = ::InterlockedIncrement(&loader::cppmod::g_uninstallCalls);
    UE_LOGI("cppmod: uninstall_mod called (ptr=%p, call #%ld) -- ignored by design", mod, n);
    ue_wrap::log::Flush();
}
