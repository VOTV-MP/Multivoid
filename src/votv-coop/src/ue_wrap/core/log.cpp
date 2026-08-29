#include "ue_wrap/core/log.h"

#include "ue_wrap/core/paths.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <ctime>
#include <locale.h>
#include <mutex>
#include <share.h>

namespace ue_wrap::log {
namespace {

// A UTF-8 locale used for FORMATTING ONLY, never installed process-wide.
//
// MEASURED 2026-07-28, and it had been silently eating log lines since the first
// Cyrillic nickname: `%ls` in vsnprintf converts wide -> narrow through the C
// locale, and the default "C" locale can encode nothing above U+007F. The call
// returns -1 and MSVC leaves the buffer EMPTY, so the whole line degenerates to
// a bare "[21:04:18] [INFO ] " with no message at all -- not truncated, GONE.
// Every line naming a Cyrillic, CJK or emoji peer vanished, which is precisely
// the population the international-names work exists for, and it is why an
// arc-D2 drill looked like a relay failure: the evidence lines were missing, not
// the behaviour. (Probe: `%ls` of "Пел" -> n=-1; with this locale -> n=17.)
//
// _create_locale + the _l formatter keep this to OUR call. setlocale() would be
// the two-line version and is wrong here: we are injected into someone else's
// process, and LC_CTYPE is shared CRT state the game also reads.
//
// LC_CTYPE, **NOT** LC_ALL -- one token, and the audits caught it independently.
// ".UTF-8" leaves the language/country to the OS user default, and LC_ALL drags
// LC_NUMERIC along with it: on a ru-RU machine every `%f` in the log turned into
// `1,50`. Measured side by side -- plain `1.50`, LC_ALL `1,50`, LC_CTYPE `1.50`,
// and LC_CTYPE converts `%ls` exactly as well (`n=9` for a Cyrillic+CJK+emoji
// string, against `n=-1` plain). 301 log call sites carry a float and mp.py
// parses numbers out of them, so LC_ALL would have made every peer's log
// machine-dependent to fix a character-conversion bug.
_locale_t Utf8Locale() {
    static _locale_t loc = ::_create_locale(LC_CTYPE, ".UTF-8");
    return loc;
}

FILE* g_file = nullptr;
CRITICAL_SECTION g_lock;
std::once_flag g_lockOnce;
bool g_opened = false;

// STALENESS BOUND for buffered INFO. Guarded by g_lock (every reader and writer
// already holds it), so a plain integer is correct here -- no atomic needed.
//
// MEASURED 2026-08-29, and it cost a whole diagnosis: a real r2modman session ran
// for four minutes and left a 65-line log ending mid-boot. Nothing was wrong with
// the mod -- `multivoid.ini.example` was regenerated at 20:03:38, the very step
// whose INFO line is missing -- the game simply closed without reaching Shutdown()
// (no `shutdown: END cleanup` block), and the CRT's ~4 KB buffer died with it.
// Every INFO line since the last WARN was lost, which is precisely the window a
// post-mortem needs.
//
// The pre-existing answer was ~20 explicit Flush() calls at hand-picked milestones.
// That is a SITE LIST, and it fails the way site lists fail: it covers the paths
// someone anticipated, and the session that actually broke was not one of them.
// The invariant replaces the list -- the log on disk is never more than
// kFlushIntervalMs behind the process, whatever happens next and whoever writes.
//
// It does NOT reintroduce what the 2026-05-27 audit removed. That measured ~50
// synchronous disk syncs/sec from per-INFO flush (a ~2000-line dedup burst over
// ~40 s) visibly tanking FPS. This caps the same work at ONE sync/sec regardless
// of line rate -- 1/50th of the cost that was rejected -- and a quiet log flushes
// nothing at all, because the check rides an existing write rather than a timer.
// The per-line cost added is one GetTickCount64(), which reads KUSER_SHARED_DATA
// with no syscall; its ~15.6 ms granularity is irrelevant against a 1 s interval.
//
// RESIDUAL, stated rather than discovered later: a process that dies during a
// QUIET period still loses the tail written since the last flush, because there is
// no later write to carry the check. Bounded by "lines written in the final second
// of activity", not by "everything since the last WARN".
constexpr ULONGLONG kFlushIntervalMs = 1000;
ULONGLONG g_lastFlushMs = 0;

// Optional log sink (the in-game console). Atomic so SetSink is lock-free vs Write.
std::atomic<Sink> g_sink{nullptr};

// Build "<game exe dir>\<logfile>". The filename is VOTVCOOP_LOG if set, else
// multivoid.log (per-process log names for multi-instance tests). Anchored on
// the EXE dir (ue_wrap::paths::ExeDir, the install-dir anchor) -- under UE4SS
// the DLL itself lives in Mods\Multivoid\dlls\, which is loader-dependent and
// (under shimloader) virtualized; the exe dir is the install's one real home.
void LogPath(wchar_t (&out)[MAX_PATH]) {
    out[0] = L'\0';
    const std::wstring dir = paths::ExeDir();
    if (!dir.empty()) {
        wcscpy_s(out, dir.c_str());
        wcscat_s(out, L"\\");
    }
    wchar_t name[64] = {};
    if (::GetEnvironmentVariableW(L"VOTVCOOP_LOG", name, 64) == 0 || name[0] == L'\0')
        wcscpy_s(name, L"multivoid.log");
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
        // history (multivoid.prev.log) means the bug session survives that relaunch. The
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
    std::fprintf(g_file, "==== Multivoid log ====\n");
    std::fflush(g_file);
    g_lastFlushMs = ::GetTickCount64();
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
    // Keep the staleness stamp coherent: an explicit flush IS a flush, so the
    // next INFO line must not immediately re-sync as if none had happened.
    g_lastFlushMs = ::GetTickCount64();
    ::LeaveCriticalSection(&g_lock);
}

void SetSink(Sink sink) { g_sink.store(sink, std::memory_order_release); }

void Write(Level level, const char* fmt, ...) {
    EnsureOpen();
    if (!g_file) return;

    // Format the message body ONCE into a local buffer so we can write it to the file AND
    // hand it to the sink (the console) without re-running printf. Truncates at 1 KB.
    char msg[1024];
    // Not `= {}`: that memsets a kilobyte on every log line. One byte is all the
    // failure paths below need, and without it `msg[0]` and the strlen scan read
    // uninitialized stack whenever the formatter returns without writing.
    msg[0] = '\0';
    va_list args;
    va_start(args, fmt);
    int wrote = -1;
    if (_locale_t loc = Utf8Locale()) {
        // The NON-SECURE _l variant, deliberately. `_vsnprintf_s_l` routes a
        // malformed conversion specifier to the CRT invalid-parameter handler,
        // which raises __fastfail -- measured: a `%q` typo terminated the probe
        // process outright, and __fastfail bypasses SEH, so RenderFrameGuarded's
        // __try and every per-callback wrapper in the mod are useless against it.
        // A logging typo must never be able to kill the game. The non-secure
        // variant printed `bad q here` and carried on, which is what
        // std::vsnprintf did before this change. It does not NUL-terminate on
        // truncation, so we reserve the last byte and terminate ourselves.
#pragma warning(suppress : 4996)  // "_vsnprintf_s_l is safer" -- see above: it is
        wrote = ::_vsnprintf_l(msg, sizeof(msg) - 1, fmt, loc, args);  // not, it FASTFAILS
    } else {
        wrote = std::vsnprintf(msg, sizeof(msg), fmt, args);
    }
    msg[sizeof(msg) - 1] = '\0';
    va_end(args);
    // A LINE MUST NEVER DISAPPEAR BECAUSE OF ITS ARGUMENTS. A conversion failure
    // can leave the buffer empty, and an empty message is indistinguishable from
    // a bug that never logged. Fall back to the format string: it names the site,
    // which is the half worth keeping.
    if (wrote < 0 && msg[0] == '\0') {
        std::snprintf(msg, sizeof(msg), "%s [args unformattable]", fmt);
    } else if (wrote < 0) {
        // Truncated (or stopped mid-string). Drop a trailing UTF-8 sequence ONLY
        // if it is INCOMPLETE -- the obvious "walk back past continuations, then
        // drop the lead" loses a whole valid character every time, and truncation
        // is the common case for exactly the long name/roster lines this arc
        // exists to serve.
        size_t n = std::strlen(msg);
        size_t lead = n;
        while (lead > 0 && (static_cast<unsigned char>(msg[lead - 1]) & 0xC0) == 0x80) --lead;
        if (lead > 0) {
            const unsigned char c = static_cast<unsigned char>(msg[lead - 1]);
            const size_t need = (c < 0x80)          ? 1
                              : ((c & 0xE0) == 0xC0) ? 2
                              : ((c & 0xF0) == 0xE0) ? 3
                              : ((c & 0xF8) == 0xF0) ? 4
                                                     : 1;   // stray continuation
            if (n - (lead - 1) < need) n = lead - 1;
        }
        msg[n] = '\0';
    }

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
    // bursts.
    //
    // ...and INFO is flushed anyway once kFlushIntervalMs has passed, so the
    // buffer can never outlive the process by more than that. See the constant
    // for why the site list of explicit Flush() calls was not enough, and why
    // this is 1/50th of the cost the 2026-05-27 audit rejected.
    if (level != Level::Info) {
        std::fflush(g_file);
        g_lastFlushMs = ::GetTickCount64();
    } else {
        const ULONGLONG now = ::GetTickCount64();
        if (now - g_lastFlushMs >= kFlushIntervalMs) {
            std::fflush(g_file);
            g_lastFlushMs = now;
        }
    }
    ::LeaveCriticalSection(&g_lock);

    // Mirror to the sink OUTSIDE our critical section so the console's own lock can never
    // be held under g_lock (no lock-order inversion). The sink must not log (no recursion).
    if (Sink s = g_sink.load(std::memory_order_acquire)) s(level, msg);
}

}  // namespace ue_wrap::log
