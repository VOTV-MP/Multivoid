#include "ue_wrap/log.h"

#include <windows.h>

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

void Write(Level level, const char* fmt, ...) {
    EnsureOpen();
    if (!g_file) return;

    ::EnterCriticalSection(&g_lock);

    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char ts[32] = {};
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    std::fprintf(g_file, "[%s] [%-5s] ", ts, Tag(level));

    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_file, fmt, args);
    va_end(args);

    std::fprintf(g_file, "\n");
    std::fflush(g_file);

    ::LeaveCriticalSection(&g_lock);
}

}  // namespace ue_wrap::log
