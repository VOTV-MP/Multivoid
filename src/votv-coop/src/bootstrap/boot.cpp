// bootstrap/boot.cpp -- see bootstrap/boot.h.
//
// BootThread body moved VERBATIM from bootstrap/dllmain.cpp (2026-08-21, the
// D-3 spike): dllmain keeps only the lane discriminator + DETACH backstop, and
// src/loader/cppmod_entry.cpp is the second caller.

#include "bootstrap/boot.h"

#include "bootstrap/refuse_dialog.h"  // the stand-down modal (never the overlay's dialog)
#include "coop/net/protocol.h"  // kProtocolVersion -- the b<N> build rev in the banner
#include "coop/version.h"
#include "harness/harness.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"
#include "ue_wrap/core/reflection.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

namespace bootstrap {
namespace {

// UNBOOTED=0 -> BOOTING=1 at StartOnce entry (one attempt per module instance,
// ever -- a failed half-boot must NOT be retried into half-installed hooks).
volatile LONG g_bootLatch = 0;
// Set only when the attempt reached kStarted (audit F3: a duplicate-mutex
// REFUSED instance has attempted but never started -- its restart re-entry
// must not read as a live session).
volatile LONG g_started = 0;

// Milliseconds since THIS process was created (GetProcessTimes creation time),
// for the load-moment marker: how late UE4SS's mod-scan started us relative to
// process creation. Wall-clock filetimes, 100ns.
unsigned long long MsSinceProcessStart() {
    FILETIME create{}, exit_{}, kernel{}, user{}, now{};
    if (!::GetProcessTimes(::GetCurrentProcess(), &create, &exit_, &kernel, &user)) return 0;
    ::GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = create.dwLowDateTime;
    a.HighPart = create.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;
    return (b.QuadPart - a.QuadPart) / 10000ull;
}

void WriteMarker(const char* entryTag) {
    // The marker lands beside the game exe (the install-dir anchor,
    // ue_wrap/core/paths) -- same home as multivoid.log / multivoid.ini.
    const std::wstring dir = ue_wrap::paths::ExeDir();
    if (dir.empty()) return;
    wchar_t markerPath[MAX_PATH] = {};
    wcscpy_s(markerPath, dir.c_str());
    wcscat_s(markerPath, L"\\multivoid-loaded.txt");

    FILE* f = nullptr;
    if (_wfopen_s(&f, markerPath, L"a") == 0 && f) {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        char ts[32] = {};
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
        std::fprintf(f, "[%s] multivoid bootstrap loaded into PID %lu (entry=%s)\n",
                     ts, ::GetCurrentProcessId(), entryTag);
        std::fclose(f);
    }
}

// Support telemetry (D-3): which UE4SS host shares the process. Reads the
// version RESOURCE of whichever UE4SS module is loaded (the official builds
// ship "UE4SS.dll"; shimloader loads a lowercase "ue4ss.dll"). Boot-time
// snapshot only.
void LogUe4ssPresence() {
    // One lookup: GetModuleHandleW is case-insensitive, so this matches the
    // official "UE4SS.dll" and shimloader's lowercase "ue4ss.dll" alike.
    HMODULE h = ::GetModuleHandleW(L"UE4SS.dll");
    if (!h) {
        UE_LOGI("boot: UE4SS host: not loaded at boot time");
        return;
    }
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(h, path, MAX_PATH);
    char ver[64] = "unknown";
    DWORD dummy = 0;
    if (const DWORD sz = ::GetFileVersionInfoSizeW(path, &dummy)) {
        std::string buf(sz, '\0');
        VS_FIXEDFILEINFO* ffi = nullptr;
        UINT ffiLen = 0;
        if (::GetFileVersionInfoW(path, 0, sz, buf.data()) &&
            ::VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &ffiLen) && ffi) {
            std::snprintf(ver, sizeof(ver), "%u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS),
                          LOWORD(ffi->dwFileVersionMS), HIWORD(ffi->dwFileVersionLS),
                          LOWORD(ffi->dwFileVersionLS));
        }
    }
    UE_LOGI("boot: UE4SS host: '%ls' version %s", path, ver);
}

DWORD WINAPI BootThread(LPVOID rawTag) {
    const char* entryTag = static_cast<const char*>(rawTag);
    WriteMarker(entryTag);
    // Standalone SDK health check (resolves GUObjectArray / FName::ToString /
    // ProcessEvent via AOB, then functionally validates them). Logs a PASS/FAIL
    // report to multivoid.log -- our own SDK access, no UE4SS.
    ue_wrap::log::Init();
    UE_LOGI("==== %s ====", coop::version::kDisplayLabel);
    // The Paper-pair identity line: game target + build number (= kProtocolVersion).
    UE_LOGI("boot: Multivoid %s b%u", coop::version::kGameTarget,
            static_cast<unsigned>(coop::net::kProtocolVersion));
    // Build triage line (v122): discriminates same-proto rebuilds in bug reports
    // (banner-only -- never announced, never gated; the DLL hash stays the deploy
    // truth). The exe identity beside kGameTarget makes an install-skew report
    // (mod built for cook X running on exe Y) one-look diagnosable from the log.
    UE_LOGI("boot: compiled %s %s", __DATE__, __TIME__);
    // D-3 entry + load-moment marker: which entry point brought us in (the one
    // live lane is start_mod, entry=cppmod -- mp.py's _lane_check greps it, and
    // entry=proxy-dllmain appearing here means a PREDECESSOR binary booted),
    // and how late relative to process creation (UE4SS's mod-scan runs after
    // its sig-scan phase). The spike's timing cells diff this line.
    UE_LOGI("boot: entry=%s since-process-start=%llums pid=%lu", entryTag,
            MsSinceProcessStart(), ::GetCurrentProcessId());
    LogUe4ssPresence();
    {
        char exePath[MAX_PATH] = {};
        ::GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (exePath[0] && ::GetFileAttributesExA(exePath, GetFileExInfoStandard, &fad)) {
            const unsigned long long exeSize =
                (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            UE_LOGI("boot: game exe '%s' size=%llu (mod targets VOTV %s)",
                    exePath, exeSize, coop::version::kGameTarget);
        }
    }
    // THE VERDICT IS A DECISION, NOT A LOG LINE.
    //
    // This is the one place in the process that knows whether our offsets match
    // the running game. Until 2026-08-30 it returned void and boot continued
    // unconditionally -- installing the ProcessEvent detour and driving VOTV's
    // UFunctions through offsets the check had just called wrong. The hazard is
    // NOT a null pointer (`game_thread::Install` refuses one at pe_detour.cpp:645);
    // it is an AOB that matched the WRONG SITE, which is non-null and only the
    // functional round-trips can catch. Continuing past that means writing through
    // wrong offsets into a live game -- i.e. a corrupted save, which is a far worse
    // outcome for a player than "the mod did not load".
    //
    // So we stand down, and we SAY SO on a surface that exists: the Win32 modal,
    // never `ui::boot_warning_dialog`, because that renders from an overlay this
    // path must not install (same reasoning as the loader's duplicate-install
    // refusal -- they share bootstrap::ShowRefuseDialog).
    int healthFails = ue_wrap::reflection::RunHealthCheck();
    {
        // DRILL ARM (probe; RULE 2 exempt). A refusal path that never executes is
        // a claim, not a behaviour -- and this one can only fire on a game build
        // we do not have. `VOTVCOOP_FORCE_HEALTH_FAIL=<n>` makes boot react as if
        // the check had failed n times, WITHOUT touching the check itself, so the
        // stand-down and its modal can be shown RED on a healthy install. Inert
        // unless set.
        char v[16] = {};
        if (::GetEnvironmentVariableA("VOTVCOOP_FORCE_HEALTH_FAIL", v, sizeof(v)) > 0) {
            const int forced = ::atoi(v);
            if (forced > 0) {
                UE_LOGW("boot: HEALTH-FAIL drill armed -- reacting as if %d check(s) failed "
                        "(the real verdict was %d)", forced, healthFails);
                healthFails = forced;
            }
        }
    }
    if (healthFails > 0) {
        UE_LOGE("boot: STANDING DOWN -- %d SDK health check(s) failed. The mod targets "
                "VOTV %s; this game build does not match it, so ProcessEvent will NOT "
                "be hooked and no session can start. Re-derive sdk_profile.h "
                "(docs/VERSION_MIGRATION.md).", healthFails, coop::version::kGameTarget);
        ue_wrap::log::Flush();
        bootstrap::ShowRefuseDialog(
            L"Multivoid -- unsupported game build",
            L"Multivoid did not start.\n\n"
            L"It could not find the parts of the game it needs, which means this "
            L"version of Voices of the Void is not the one this build of Multivoid "
            L"targets.\n\n"
            L"The game itself is unaffected and will keep running normally -- "
            L"Multivoid simply stood down instead of guessing.\n\n"
            L"Check for a Multivoid update built for your game version. Details are "
            L"in multivoid.log (look for 'HEALTH: FAIL').");
        return 0;
    }

    // Establish a game-thread execution context: hook ProcessEvent so we have a
    // guaranteed game-thread callback to drive UFunction calls from (ProcessEvent
    // must NOT be called from this boot thread). Then post a self-test task to
    // prove it: the task runs on the game thread (a different thread than this
    // one) and reads engine state safely from there.
    const unsigned long bootTid = ::GetCurrentThreadId();
    UE_LOGI("boot: BootThread tid=%lu", bootTid);
    {
        // DIAGNOSTIC AMPLIFIER (probe; RULE 2 exempt; WP-2 2026-08-22): delay the
        // PE MinHook install so the patch lands while the game thread is deep in
        // live ProcessEvent traffic. Used to force the ~20% boot AV into a
        // deterministic repro (hypothesis: a thread mid-PE at patch time).
        // Inert unless VOTVCOOP_PE_INSTALL_DELAY_MS is set.
        char v[16] = {};
        if (::GetEnvironmentVariableA("VOTVCOOP_PE_INSTALL_DELAY_MS", v, sizeof(v)) > 0) {
            const unsigned long ms = ::strtoul(v, nullptr, 10);
            if (ms > 0) {
                UE_LOGW("boot: PE-install DELAY diagnostic armed: sleeping %lu ms before hook", ms);
                ue_wrap::log::Flush();
                ::Sleep(ms);
            }
        }
    }
    if (ue_wrap::game_thread::Install()) {
        ue_wrap::game_thread::Post([bootTid] {
            const unsigned long tid = ::GetCurrentThreadId();
            const int32_t n = ue_wrap::reflection::NumObjects();
            UE_LOGI("game-thread self-test: task ran on tid=%lu (boot tid=%lu, %s); "
                    "NumObjects()=%d read from game thread",
                    tid, bootTid, tid != bootTid ? "DIFFERENT thread -- OK" : "SAME -- WRONG",
                    n);
            UE_LOGI("==== GAME-THREAD CONTEXT: LIVE ====");
        });
        UE_LOGI("boot: game-thread dispatcher installed; self-test task posted");

        // Autonomous test harness (ported from the UE4SS Lua coopTestHarness):
        // skip the menus into gameplay, screenshot, report -- standalone.
        harness::Start();
    } else {
        UE_LOGE("boot: failed to install game-thread dispatcher");
    }
    return 0;
}

}  // namespace

bool AlreadyBooted() {
    return ::InterlockedCompareExchange(&g_bootLatch, 0, 0) != 0;
}

bool Started() {
    return ::InterlockedCompareExchange(&g_started, 0, 0) != 0;
}

StartResult StartOnce(const char* entryTag) {
    // Latch FIRST: a second call on this module instance is the UE4SS restart
    // re-entry (or a double-fire of the proxy lane) -- never re-bootstrap, and
    // never re-take the mutex (CreateMutex on our own held name would report
    // ERROR_ALREADY_EXISTS and mislabel the benign restart as a duplicate).
    if (::InterlockedCompareExchange(&g_bootLatch, 1, 0) != 0) {
        UE_LOGI("boot: entry=%s already-booted (re-entry ignored; session keeps running)",
                entryTag);
        return StartResult::kAlreadyBooted;
    }

    // Per-PROCESS duplicate guard (lane-symmetric): first boot in this process
    // owns the name; a SECOND module instance of the mod in the SAME process
    // (two mod-folder copies; a folder-mod beside a live standalone install)
    // collides here. PID suffix on purpose -- several game processes on one
    // box (the standard LAN workflow) must never see each other.
    wchar_t mutexName[64] = {};
    ::swprintf_s(mutexName, L"Local\\MultivoidLoaded_%lu", ::GetCurrentProcessId());
    const HANDLE mutex = ::CreateMutexW(nullptr, FALSE, mutexName);  // held for process life
    if (mutex && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        UE_LOGE("boot: REFUSE reason=duplicate-mutex entry=%s -- another Multivoid instance "
                "already booted this process", entryTag);
        ue_wrap::log::Flush();
        return StartResult::kRefusedDupMutex;
    }

    // LATCH AFTER THE SPAWN, NOT BEFORE. `Started()` means "the boot thread
    // exists", not "we intended to make one" -- and the old order made a failed
    // CreateThread indistinguishable from a successful boot, at every caller and
    // in the log. `Started()` is only ever read on RE-ENTRY (cppmod_entry.cpp:320),
    // long after this call returns, so there is no window to race here.
    const HANDLE t = ::CreateThread(nullptr, 0, BootThread,
                                    const_cast<char*>(entryTag), 0, nullptr);
    if (!t) {
        UE_LOGE("boot: REFUSE reason=thread-spawn-failed entry=%s (CreateThread gle=%lu) -- "
                "nothing is running; this instance stays refused", entryTag, ::GetLastError());
        ue_wrap::log::Flush();
        return StartResult::kRefusedThreadSpawn;
    }
    ::CloseHandle(t);
    ::InterlockedExchange(&g_started, 1);
    return StartResult::kStarted;
}

}  // namespace bootstrap
