#include "ue_wrap/core/log.h"

#include "ue_wrap/core/paths.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <cstring>
#include <string>
#include <ctime>
#include <locale.h>
#include <memory>
#include <mutex>
#include <new>
#include <share.h>

namespace ue_wrap::log {
namespace {

// A UTF-8 locale used for formatting only, never installed process-wide. The formatter's
// wide-to-narrow conversion goes through the C locale, whose default encodes nothing above
// ASCII: the call returns -1 and the runtime leaves the buffer empty, so a line naming a
// non-ASCII peer would vanish whole, not truncate. A created locale with the locale-taking
// formatter keeps the fix to this call; the global setter is wrong here, since we are injected
// into someone else's process and the character-type category is runtime state the game also
// reads. Character type only: the all-categories form drags the numeric category along, so on
// a Russian-locale machine every float in the log would print with a comma decimal, and
// hundreds of log sites carry a float the smoke driver parses.
_locale_t Utf8Locale() {
    static _locale_t loc = ::_create_locale(LC_CTYPE, ".UTF-8");
    return loc;
}

FILE* g_file = nullptr;
CRITICAL_SECTION g_lock;
std::once_flag g_lockOnce;
bool g_opened = false;

// The paths EnsureOpen settled on: the live log, and the file the previous run's log was kept as.
// Guarded by g_lock. Fixed arrays, not strings: EnsureOpen runs inside Write, which never throws,
// and holds the lock while it copies.
wchar_t g_livePath[MAX_PATH] = {};
wchar_t g_prevPath[MAX_PATH] = {};

// The staleness bound for buffered info lines; guarded by the lock (every reader and writer
// holds it), so a plain integer is correct. The runtime buffers info lines, and a game closed
// without reaching the shutdown loses every line since the last warning, precisely the window
// a post-mortem needs; a site list of explicit flushes fails the way site lists fail, the
// session that breaks is never one of them. The invariant instead: the log on disk is never
// more than the interval behind the process, whoever writes. This is not the per-line flush
// (tens of synchronous disk syncs per second during a burst, visibly tanking the frame rate):
// it caps the work at one sync per interval regardless of line rate, a quiet log flushes
// nothing since the check rides a write rather than a timer, and the per-line cost is one
// tick-count read. A process that dies during a quiet period still loses the tail since the
// last flush.
constexpr ULONGLONG kFlushIntervalMs = 1000;
ULONGLONG g_lastFlushMs = 0;

// The optional log sink, the in-game console; atomic, so setting it is lock-free against a
// write.
std::atomic<Sink> g_sink{nullptr};

// Build the log path in the game exe's directory: the filename is the VOTVCOOP_LOG env var if
// set, else multivoid.log (per-process names for multi-instance tests). Anchored on the exe
// directory, the install's one real home; the DLL's own directory is loader-dependent and
// virtualised under the shim loader.

// Readers may tail the live log; other processes must not rename or truncate it.
FILE* OpenShared(const wchar_t* path) {
    HANDLE h = ::CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_WRONLY | _O_TEXT);
    if (fd < 0) { ::CloseHandle(h); return nullptr; }
    FILE* f = _fdopen(fd, "w");
    if (!f) _close(fd);
    return f;
}

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
    // Initialise the lock exactly once, even if Write is called from several threads before
    // Init; a plain-bool double check would let two threads initialise the critical section
    // concurrently.
    std::call_once(g_lockOnce, [] { ::InitializeCriticalSection(&g_lock); });
    ::EnterCriticalSection(&g_lock);
    if (!g_opened) {
        wchar_t path[MAX_PATH] = {};
        LogPath(path);
        // Preserve a closed run's log. A live writer denies rename and forces the PID fallback.
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
            if (::MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING)) wcscpy_s(g_prevPath, prev);
            g_file = OpenShared(path);
            if (g_file) wcscpy_s(g_livePath, path);
        }
        // This also covers an overlapping older build that does not share delete access.
        if (!g_file) {
            g_prevPath[0] = L'\0';
            wchar_t alt[MAX_PATH];
            wcscpy_s(alt, path);
            const size_t alen = wcslen(alt);
            if (alen > 4 && _wcsicmp(alt + alen - 4, L".log") == 0) alt[alen - 4] = L'\0';
            wchar_t suffix[32];
            swprintf_s(suffix, L".%lu.log", static_cast<unsigned long>(::GetCurrentProcessId()));
            wcscat_s(alt, suffix);
            g_file = OpenShared(alt);
            if (g_file) {
                wcscpy_s(g_livePath, alt);
            }
        }
        if (g_file) {
            std::fprintf(g_file, "==== Multivoid log ====\n");
            std::fprintf(g_file, "%s\n", kLogFormatLine);
        }
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

void Flush() {
    EnsureOpen();
    if (!g_file) return;
    ::EnterCriticalSection(&g_lock);
    std::fflush(g_file);
    // Keep the staleness stamp coherent: an explicit flush is a flush, so the next info line must
    // not immediately re-sync as if none had happened.
    g_lastFlushMs = ::GetTickCount64();
    ::LeaveCriticalSection(&g_lock);
}

void SetSink(Sink sink) { g_sink.store(sink, std::memory_order_release); }

std::wstring CurrentPath() {
    EnsureOpen();
    wchar_t copy[MAX_PATH];
    ::EnterCriticalSection(&g_lock);
    wcscpy_s(copy, g_livePath);
    ::LeaveCriticalSection(&g_lock);
    return copy;
}

std::wstring PreviousPath() {
    EnsureOpen();
    wchar_t copy[MAX_PATH];
    ::EnterCriticalSection(&g_lock);
    wcscpy_s(copy, g_prevPath);
    ::LeaveCriticalSection(&g_lock);
    return copy;
}

// The mark is for the bug-report redactor, which replaces every marked span with a token; the
// callers are the log sites that print a peer's address, a dial text or a non-project endpoint.
// The delimiters are replaced inside the value so no value can close its own mark early.
std::string Addr(std::string_view value) {
    std::string out;
    out.reserve(sizeof(kAddrOpen) + value.size() + sizeof(kAddrClose));
    out += kAddrOpen;
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        const bool bracket = c == 0xE2 && i + 2 < value.size() &&
                             static_cast<unsigned char>(value[i + 1]) == 0x9F &&
                             (static_cast<unsigned char>(value[i + 2]) == 0xA8 ||
                              static_cast<unsigned char>(value[i + 2]) == 0xA9);
        if (bracket) {
            out += '?';
            i += 2;
        } else if (c == '\r' || c == '\n') {
            out += '?';
        } else {
            out += static_cast<char>(c);
        }
    }
    out += kAddrClose;
    return out;
}

void Write(Level level, const char* fmt, ...) {
    EnsureOpen();
    if (!g_file) return;

    // Format the message body once into a local buffer, so it can go to the file and to the sink
    // without re-running the formatter. A line under 1 KB (the common case) takes one stack
    // format; a longer one the formatter can finish is measured and written whole from `whole`,
    // so its address mark is never split. A line the formatter cannot finish (a conversion
    // stop), or whose whole buffer the heap refuses, is still cut to the stack buffer by the
    // trim below.
    char msg[1024];
    // Write never throws, so the whole-line buffer is a nothrow allocation: a refusal leaves
    // `whole` empty and the line takes the stack buffer and the trim.
    std::unique_ptr<char[]> whole;
    // Not a full zero-initialisation: that clears a kilobyte on every log line. One byte is all
    // the failure paths below need, and without it the first byte and the length scan read
    // uninitialised stack whenever the formatter returns without writing.
    msg[0] = '\0';
    va_list args;
    va_start(args, fmt);
    // Two copies, taken before the first format consumes the list: one to measure a line that did
    // not fit, one to format it again at its real length.
    va_list measure;
    va_copy(measure, args);
    va_list again;
    va_copy(again, args);
    int wrote = -1;
    if (_locale_t loc = Utf8Locale()) {
        // The non-secure locale variant, deliberately: the secure one routes a malformed conversion
        // specifier to the runtime's invalid-parameter handler, a fast-fail that bypasses SEH, so
        // no frame guard or per-callback wrapper in the mod could catch it and a logging typo would
        // kill the game. The non-secure variant prints the bad specifier and carries on; it does
        // not terminate on truncation, so the last byte is reserved and terminated here.
#pragma warning(suppress : 4996)  // "_vsnprintf_s_l is safer" -- see above: it is
        wrote = ::_vsnprintf_l(msg, sizeof(msg) - 1, fmt, loc, args);  // not, it FASTFAILS
        if (wrote < 0 && msg[0] != '\0') {
            // Truncated, or stopped at a conversion. The measure tells them apart: a length means
            // the line is formattable whole; -1 means a conversion stop, which the trim below
            // keeps as it was.
            const int need = ::_vscprintf_l(fmt, loc, measure);
            if (need >= 0) {
                const size_t size = static_cast<size_t>(need) + 1;
                whole.reset(new (std::nothrow) char[size]);
#pragma warning(suppress : 4996)
                if (whole && ::_vsnprintf_l(whole.get(), size, fmt, loc, again) < 0) whole.reset();
                if (whole) whole[need] = '\0';
            }
        }
    } else {
        wrote = std::vsnprintf(msg, sizeof(msg), fmt, args);
        if (wrote >= static_cast<int>(sizeof(msg))) {
            const size_t size = static_cast<size_t>(wrote) + 1;
            whole.reset(new (std::nothrow) char[size]);
            if (whole && std::vsnprintf(whole.get(), size, fmt, again) < 0) whole.reset();
        }
    }
    msg[sizeof(msg) - 1] = '\0';
    va_end(again);
    va_end(measure);
    va_end(args);
    // A line must never disappear because of its arguments: a conversion failure can leave the
    // buffer empty, and an empty message is indistinguishable from a bug that never logged. Fall
    // back to the format string, which names the site, the half worth keeping.
    if (whole) {
        // Formatted whole above: nothing to trim.
    } else if (wrote < 0 && msg[0] == '\0') {
        std::snprintf(msg, sizeof(msg), "%s [args unformattable]", fmt);
    } else if (wrote < 0 || wrote >= static_cast<int>(sizeof(msg))) {
        // Stopped mid-string (a conversion the formatter could not finish, or a line the heap
        // format above could not take). Drop a trailing UTF-8 sequence only if it is incomplete:
        // walking back past continuations and dropping the lead loses a whole valid character
        // every time.
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
    const char* const line = whole ? whole.get() : msg;

    char ts[32] = {};
    {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    }

    ::EnterCriticalSection(&g_lock);
    std::fprintf(g_file, "[%s] [%-5s] %s\n", ts, Tag(level), line);
    // Flush on warnings and errors only, keeping them visible at once; info lines ride the
    // runtime's buffer and land in bursts, since a per-line flush costs tens of synchronous disk
    // syncs per second during a burst, visibly tanking the frame rate. Info is flushed anyway once
    // the interval has passed, so the buffer can never outlive the process by more than that.
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

    // Mirror to the sink outside our critical section, so the console's own lock can never be
    // held under ours (no lock-order inversion). The sink must not log.
    if (Sink s = g_sink.load(std::memory_order_acquire)) s(level, line);
}

}  // namespace ue_wrap::log
