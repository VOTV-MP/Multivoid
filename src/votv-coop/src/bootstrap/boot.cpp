// bootstrap/boot.cpp -- see bootstrap/boot.h.
//
// BootThread body moved VERBATIM from bootstrap/dllmain.cpp (2026-08-21, the
// D-3 spike): dllmain keeps only the lane discriminator + DETACH backstop, and
// src/loader/cppmod_entry.cpp is the second caller.

#include "bootstrap/boot.h"

#include "coop/net/protocol.h"  // kProtocolVersion -- the b<N> build rev in the banner
#include "coop/version.h"
#include "harness/harness.h"
#include "ui/boot_warning_dialog.h"  // v122: the duplicate-DLL install popup
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
// for the proxy-vs-UE4SS load-moment comparison. Wall-clock filetimes, 100ns.
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

// Support telemetry (D-3): which UE4SS host, if any, shares the process. Reads
// the version RESOURCE of whichever UE4SS module is loaded (the official builds
// ship "UE4SS.dll"; shimloader loads a lowercase "ue4ss.dll"). Boot-time
// snapshot only -- on the proxy lane UE4SS may legitimately load later.
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
    // D-3 lane + load-moment marker: which loader brought us in, and how late
    // relative to process creation (proxy = process-init; UE4SS = its mod-scan,
    // after the sig-scan phase). The spike's timing cells diff this line.
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
    // Duplicate-install detection (v122 multivoid rename): the xinput proxy scanned
    // for multivoid-*.dll; if it found MORE than one version file (or a stale legacy
    // votv-coop.dll), it loaded the highest build and left the leftovers in
    // MULTIVOID_DUP_FILES. Surface that as an in-game popup (the user asked for a
    // dialog, not a log line) + a WARN for the log-based triage.
    {
        char dup[1024] = {};
        char loaded[256] = {};
        const DWORD n = ::GetEnvironmentVariableA("MULTIVOID_DUP_FILES", dup, sizeof(dup));
        ::GetEnvironmentVariableA("MULTIVOID_LOADED", loaded, sizeof(loaded));
        if (n > 0 && n < sizeof(dup)) {
            UE_LOGW("boot: MULTIPLE mod DLL versions found beside the exe -- loaded '%s', "
                    "leftover(s): %s", loaded, dup);
            std::string msg =
                "Several versions of the multivoid mod DLL are installed next to the game.\n\n"
                "Loaded (newest): " + std::string(loaded[0] ? loaded : "?") + "\n"
                "Also found: " + std::string(dup) + "\n\n"
                "Delete the other file(s) from VotV\\Binaries\\Win64 to avoid running "
                "a mixed install.";
            ui::boot_warning_dialog::Arm(msg);
        }
    }
    ue_wrap::reflection::RunHealthCheck();

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

    ::InterlockedExchange(&g_started, 1);
    if (HANDLE t = ::CreateThread(nullptr, 0, BootThread,
                                  const_cast<char*>(entryTag), 0, nullptr)) {
        ::CloseHandle(t);
    }
    return StartResult::kStarted;
}

}  // namespace bootstrap
