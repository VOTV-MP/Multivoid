#include "ue_wrap/log.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <share.h>

namespace ue_wrap::log {
namespace {

FILE* g_file = nullptr;
CRITICAL_SECTION g_lock;
std::once_flag g_lockOnce;
bool g_opened = false;

// Optional log sink (the in-game console). Atomic so SetSink is lock-free vs Write.
std::atomic<Sink> g_sink{nullptr};

// Build "<dir of this DLL>\<logfile>". The filename is VOTVCOOP_LOG if set, else
// votv-coop.log -- so the two-instance LAN test can give each process its own log
// (both instances load the SAME DLL from the SAME dir; without this they would
// clobber one shared file).
void LogPath(wchar_t (&out)[MAX_PATH]) {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&LogPath), &self);
    wchar_t dll[MAX_PATH] = {};
    ::GetModuleFileNameW(self, dll, MAX_PATH);
    wchar_t* lastSep = nullptr;
    for (wchar_t* p = dll; *p; ++p) {
        if (*p == L'\\' || *p == L'/') lastSep = p;
    }
    out[0] = L'\0';
    if (lastSep) {
        const size_t dirLen = static_cast<size_t>(lastSep - dll) + 1;
        wcsncpy_s(out, MAX_PATH, dll, dirLen);
    }
    wchar_t name[64] = {};
    if (::GetEnvironmentVariableW(L"VOTVCOOP_LOG", name, 64) == 0 || name[0] == L'\0')
        wcscpy_s(name, L"votv-coop.log");
    wcscat_s(out, name);
}

void EnsureOpen() {
    // Initialize the lock exactly once, even if Write() is called from several
    // threads before Init() (a plain-bool double-check would let two threads
    // init the CRITICAL_SECTION concurrently -- UB).
    std::call_once(g_lockOnce, [] { ::InitializeCriticalSection(&g_lock); });
    ::EnterCriticalSection(&g_lock);
    if (!g_opened) {
        wchar_t path[MAX_PATH] = {};
        LogPath(path);
        // Preserve the PREVIOUS session's log before the open below truncates it. Real
        // users hit a problem then often relaunch before sending the log; one level of
        // history (votv-coop.prev.log) means the bug session survives that relaunch. The
        // prior process has exited (each launch is a fresh process), so the rename is safe.
        {
            wchar_t prev[MAX_PATH] = {};
            wcscpy_s(prev, path);
            const size_t plen = wcslen(prev);
            if (plen > 4 && _wcsicmp(prev + plen - 4, L".log") == 0) {
                prev[plen - 4] = L'\0';
                wcscat_s(prev, L".prev.log");
            } else {
                wcscat_s(prev, L".prev");
            }
            ::MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING);  // best-effort; ignore failure
        }
        // Open with read-sharing (_SH_DENYWR: others may READ, not write) so the
        // log can be tailed live while the game runs -- without this the file is
        // locked exclusively and diagnostics can't be read until the game exits.
        g_file = _wfsopen(path, L"w", _SH_DENYWR);
        g_opened = true;
    }
    ::LeaveCriticalSection(&g_lock);
}

const char* Tag(Level l) {
    switch (l) {
        case Level::Warn: return "WARN";
        case Level::Error: return "ERROR";
        default: return "INFO";
    }
}

}  // namespace

void Init() {
    EnsureOpen();
    if (!g_file) return;
    ::EnterCriticalSection(&g_lock);
    std::fprintf(g_file, "==== votv-coop log ====\n");
    std::fflush(g_file);
    ::LeaveCriticalSection(&g_lock);
}

void Shutdown() {
    std::call_once(g_lockOnce, [] { ::InitializeCriticalSection(&g_lock); });
    ::EnterCriticalSection(&g_lock);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
        g_opened = false;
    }
    ::LeaveCriticalSection(&g_lock);
}

void Flush() {
    EnsureOpen();
    if (!g_file) return;
    ::EnterCriticalSection(&g_lock);
    std::fflush(g_file);
    ::LeaveCriticalSection(&g_lock);
}

void SetSink(Sink sink) { g_sink.store(sink, std::memory_order_release); }

void Write(Level level, const char* fmt, ...) {
    EnsureOpen();
    if (!g_file) return;

    // Format the message body ONCE into a local buffer so we can write it to the file AND
    // hand it to the sink (the console) without re-running printf. Truncates at 1 KB.
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    char ts[32] = {};
    {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    }

    ::EnterCriticalSection(&g_lock);
    std::fprintf(g_file, "[%s] [%-5s] %s\n", ts, Tag(level), msg);
    // Audit 2026-05-27 (post-v2 anim ship): per-INFO fflush was eating
    // game-thread time -- a spam burst of ~2000 dedup INFO lines / ~40 s
    // (host re-broadcasting known props) translated to ~50 synchronous
    // disk syncs per second on the client, visibly tanking FPS. Flush
    // only on WARN/ERROR (critical messages stay visible immediately);
    // INFO lines ride the CRT stdio buffer (~4 KB) and land on disk in
    // bursts. Live-tailing INFO is slightly delayed but the perf cost
    // disappears entirely on the hot path.
    if (level != Level::Info) {
        std::fflush(g_file);
    }
    ::LeaveCriticalSection(&g_lock);

    // Mirror to the sink OUTSIDE our critical section so the console's own lock can never
    // be held under g_lock (no lock-order inversion). The sink must not log (no recursion).
    if (Sink s = g_sink.load(std::memory_order_acquire)) s(level, msg);
}

}  // namespace ue_wrap::log
