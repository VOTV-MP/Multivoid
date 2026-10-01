// bootstrap/boot.cpp -- see bootstrap/boot.h.
//
// The BootThread body and the StartOnce guards. dllmain boots nothing: it disables thread
// notifications on ATTACH and keeps the last-resort teardown backstop on DETACH, and
// src/loader/cppmod_entry.cpp is the only caller of StartOnce.

#include "bootstrap/boot.h"

#include "bootstrap/refuse_dialog.h"  // the stand-down modal (never the overlay's dialog)
#include "ui/native_text_field.h"   // its un-gated editing selftest
#include "coop/net/protocol.h"  // kProtocolVersion -- the b<N> build rev in the banner
#include "coop/version.h"
#include "coop/text/i18n.h"   // the language pack, settled before any surface draws
#include "harness/harness.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hook.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/paths.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/actor_end_play.h"

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
// Set only when the attempt reached kStarted: a duplicate-mutex REFUSED
// instance has attempted but never started, and its restart re-entry must
// not read as a live session.
volatile LONG g_started = 0;
// The load moment, taken as the boot begins (StartOnce, after start_mod's predecessor checks): the boot
// thread starts only after the engine patches, so its own reading would add their time.
unsigned long long g_loadMs = 0;

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

// Support telemetry: which UE4SS host shares the process. Reads the
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

// The stand-down: this build of the game is not the one the mod targets, found before anything was
// hooked. Said on the loader's own Win32 modal, never an overlay this path must not install.
void StandDownUnsupportedBuild() {
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
        L"in multivoid.log (look for 'STANDING DOWN').");
}

DWORD WINAPI BootThread(LPVOID rawTag) {
    const char* entryTag = static_cast<const char*>(rawTag);
    WriteMarker(entryTag);
    UE_LOGI("==== %s ====", coop::version::kDisplayLabel);
    // The Paper-pair identity line: game target + build number (= kProtocolVersion).
    UE_LOGI("boot: Multivoid %s b%u", coop::version::kGameTarget,
            static_cast<unsigned>(coop::net::kProtocolVersion));
    // Build triage line: discriminates same-proto rebuilds in bug reports
    // (banner-only -- never announced, never gated; the DLL hash stays the deploy
    // truth). The exe identity beside kGameTarget makes an install-skew report
    // (mod built for cook X running on exe Y) one-look diagnosable from the log.
    UE_LOGI("boot: compiled %s %s", __DATE__, __TIME__);
    // Entry + load-moment marker: which entry point brought us in (start_mod, entry=cppmod, is
    // the only one this binary can print) and how late relative to process creation. UE4SS starts
    // its C++ mods from its constructor, before any scan of its own.
    UE_LOGI("boot: entry=%s since-process-start=%llums pid=%lu", entryTag, g_loadMs,
            ::GetCurrentProcessId());
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
    // THE LANGUAGE, SETTLED BEFORE ANY SURFACE DRAWS. The pack is a JSON file a translator drops
    // in the install's i18n folder (path anchor: ue_wrap/core/paths), so the only way a player
    // learns why their translation did not apply is this call's own log line -- which prints the
    // tag, the file, the entry count and every directory searched. Idempotent and read-only: a
    // pack that is absent, malformed or half-finished leaves the English the source carries.
    coop::i18n::Init();

    // THE NATIVE TEXT FIELD'S EDITING RULES, checked at BOOT and not at session start. The field
    // lives at the MENU -- the server browser's address box -- so a player can use it without a
    // session ever existing, and a check gated on a session start would never run at all.
    // Un-gated, pure logic, microseconds.
    ui::native_text_field::RunSelftest();

    // THE VERDICT IS A DECISION, NOT A LOG LINE.
    //
    // This is the one place that knows whether our offsets match the running game, so a failed
    // check STOPS the boot instead of arming the patched detours and driving VOTV's UFunctions
    // through offsets the check has just called wrong. The hazard is an AOB that matched the WRONG
    // SITE: non-null, caught only by the functional round-trips, and writing through wrong offsets
    // into a live game corrupts the save. The patches made at the loader's call stay unarmed: each
    // guard forwards to the function it covers and runs nothing of ours. So we stand down and SAY
    // SO on a surface that exists -- the Win32 modal shared with the loader, never
    // `ui::boot_warning_dialog`, which renders from an overlay this path must not install.
    int healthFails = ue_wrap::reflection::RunHealthCheck();
    {
        // DRILL ARM. A refusal path that never executes is a claim, not a behaviour, and this one
        // can only fire on a game build we do not have. `VOTVCOOP_FORCE_HEALTH_FAIL=<n>` makes boot
        // react as if the check had failed n times WITHOUT touching the check itself, so the
        // stand-down and its modal can be shown on a healthy install. Inert unless set.
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
                "(docs/versioning.md).", healthFails, coop::version::kGameTarget);
        StandDownUnsupportedBuild();
        return 0;
    }

    // The engine's create and delete notifications, registered as early as the profile is proven
    // so nothing born after this point escapes the object index; the index itself seeds on the
    // first game-thread drain. The game instance, the gamemode and the world context are found
    // through it, so a build whose notification lists do not read as the profile says stands down.
    if (!ue_wrap::object_index::Install()) {
        UE_LOGE("boot: STANDING DOWN -- the object index could not register with the engine's object "
                "notifications (the uobject_listeners lines above say why). The game instance, the "
                "gamemode and the world context are found through it, so no session can start.");
        StandDownUnsupportedBuild();
        return 0;
    }

    // Establish a game-thread execution context: arm the ProcessEvent detour so we have a
    // guaranteed game-thread callback to drive UFunction calls from (ProcessEvent
    // must NOT be called from this boot thread). Then post a self-test task to
    // prove it: the task runs on the game thread (a different thread than this
    // one) and reads engine state safely from there.
    const unsigned long bootTid = ::GetCurrentThreadId();
    UE_LOGI("boot: BootThread tid=%lu", bootTid);
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
        // The script-body gate beside it: the second detour, on the VM's own body loop, so a
        // Blueprint-internal call can be watched and refused per call.
        if (!ue_wrap::script_gate::Install())
            UE_LOGE("boot: the script-body gate did not arm; Blueprint-internal calls are invisible "
                    "and every watch will be refused");
        // Every actor's end of play, by any route, from one detour on the engine's own AActor::EndPlay.
        ue_wrap::actor_end_play::Install();  // logs its own failure
        // The entries UE4SS also patches still hold our jumps over the functions' own bytes.
        ue_wrap::hook::VerifyEntries("at the arm");

        // Autonomous test harness (ported from the UE4SS Lua coopTestHarness):
        // skip the menus into gameplay, screenshot, report -- standalone.
        harness::Start();
    } else {
        UE_LOGE("boot: failed to install game-thread dispatcher");
    }
    return 0;
}

// The engine code patches, made on the loader's own call before it returns. UE4SS makes that call while
// it constructs its program, and creates the threads that resolve and patch ProcessEvent, the script
// loop and AActor::EndPlay themselves only after, on both of its load paths: under its proxy the call
// runs on the game's main thread from an APC queued in DllMain, before the exe's entry point, and under
// a manual injection on the injecting thread. Its resolves then always find our jumps and compose on
// our relays. Made later, from the boot thread, both sides patched the same entries at once, and a
// patch that landed between our read of an entry and our write left our trampoline returning into it:
// every Blueprint call faulted. The detours stay inert until the boot thread's health check arms them.
// Under the proxy the game's start waits for this, so the line reports how long it took.
void PatchEngine() {
    LARGE_INTEGER freq{}, t0{}, t1{}, t2{}, t3{};
    ::QueryPerformanceFrequency(&freq);
    ::QueryPerformanceCounter(&t0);
    ue_wrap::game_thread::Patch();
    ::QueryPerformanceCounter(&t1);
    ue_wrap::script_gate::Patch();
    ::QueryPerformanceCounter(&t2);
    ue_wrap::actor_end_play::Patch();
    ::QueryPerformanceCounter(&t3);
    const auto ms = [&freq](const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
        return static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
    };
    UE_LOGI("boot: the engine patches took %.1f ms on the loader's call (ProcessEvent %.1f, the script "
            "loop %.1f, EndPlay %.1f)", ms(t0, t3), ms(t0, t1), ms(t1, t2), ms(t2, t3));
}

}  // namespace

bool AlreadyBooted() {
    return ::InterlockedCompareExchange(&g_bootLatch, 0, 0) != 0;
}

bool Started() {
    return ::InterlockedCompareExchange(&g_started, 0, 0) != 0;
}

StartResult StartOnce(const char* entryTag) {
    // Latch FIRST: a second call on this module instance is the UE4SS restart re-entry --
    // never re-bootstrap, and never re-take the mutex (CreateMutex on our own held name
    // would report ERROR_ALREADY_EXISTS and mislabel the benign restart as a duplicate).
    if (::InterlockedCompareExchange(&g_bootLatch, 1, 0) != 0) {
        UE_LOGI("boot: entry=%s already-booted (re-entry ignored; session keeps running)",
                entryTag);
        return StartResult::kAlreadyBooted;
    }
    g_loadMs = MsSinceProcessStart();

    // Per-PROCESS duplicate guard: the first boot in this process owns the name, and a
    // SECOND module instance of the mod in the SAME process (two mod-folder copies; a mod
    // folder beside a live standalone install) collides here. PID suffix on purpose --
    // several game processes on one box must never see each other.
    wchar_t mutexName[64] = {};
    ::swprintf_s(mutexName, L"Local\\MultivoidLoaded_%lu", ::GetCurrentProcessId());
    const HANDLE mutex = ::CreateMutexW(nullptr, FALSE, mutexName);  // held for process life
    if (mutex && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        UE_LOGE("boot: REFUSE reason=duplicate-mutex entry=%s -- another Multivoid instance "
                "already booted this process", entryTag);
        ue_wrap::log::Flush();
        return StartResult::kRefusedDupMutex;
    }

    PatchEngine();

    // LATCH AFTER THE SPAWN, NOT BEFORE. `Started()` means "the boot thread exists", not
    // "we intended to make one": latching first makes a failed CreateThread
    // indistinguishable from a successful boot, at every caller and in the log. `Started()`
    // is only ever read on RE-ENTRY, in the loader's restart short-circuit, long after this
    // call returns, so there is no window to race here.
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
