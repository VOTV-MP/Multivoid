// ue_wrap/core/log.h -- minimal levelled logger for the standalone mod.
//
// Writes to multivoid.log beside the game exe. The point is fast diagnosis:
// when the mod is brought up against a new game build and something is wrong,
// the log says exactly which primitive failed to resolve or validate, instead
// of a silent crash. Thread-safe. The process's first write opens the file and
// puts the header first, whichever thread writes it.
//
// There is deliberately no close: nothing sets the FILE* back to null once it is open, so the
// un-locked null test every Write() and Flush() starts with cannot race a teardown, and the
// process exit is what closes the handle. The one-second sync below is what makes that safe.

#pragma once

#include <string>
#include <string_view>

namespace ue_wrap::log {

enum class Level { Info, Warn, Error };

// printf-style (ANSI). Use %ls for wide strings (FName text is wide).
void Write(Level level, const char* fmt, ...);

// Optional log SINK: a callback that receives every formatted line (level + the message
// body, WITHOUT the "[ts] [TAG] " prefix) in addition to the file write. The in-game
// console subscribes to this so it can mirror the mod's log (connect progress, errors,
// general output) on screen. ONE sink (last set wins; nullptr clears). Invoked OUTSIDE the
// log's critical section, so the sink may take its own lock; it MUST NOT call back into the
// logger (no UE_LOG* inside a sink -- it would not deadlock, but it would recurse the line).
// Keep it cheap (it runs on whatever thread logged, including hot paths).
using Sink = void (*)(Level level, const char* msg);
void SetSink(Sink sink);

// THE STANDING GUARANTEE: the log on disk is never more than one second behind the process. INFO
// rides the CRT's ~4 KB buffer for the performance reason in log.cpp, but a write that finds the
// buffer older than that syncs it, so an abnormal exit -- a kill, a crash, a close that misses
// close -- costs at most the lines written in the final second of activity. Making a post-mortem
// readable does not require calling Flush().
//
// Force the CRT stdio buffer to disk NOW, ahead of that bound. Worth it only when something
// EXTERNAL is about to read the file and cannot wait a second -- a test runner polling for a
// verdict line, or a boot milestone you want tailable at once. Do NOT call on a hot path: this
// is a synchronous disk sync, and avoiding per-line flushing is the whole point of the buffer.
void Flush();

// The full paths of the live log and of the file the previous run's log was kept as (the live
// name with `.prev.log` for `.log`, `.prev` appended otherwise). The live log is the PID-named
// fallback (`multivoid.<PID>.log`) when another process of this install still writes the usual
// name, and then nothing was rotated: the previous path is empty unless THIS process kept a closed
// run's log. A fallback file is never rotated or removed; each one is an overlapped launch's whole
// log. The rig names its logs by the VOTVCOOP_LOG environment value, so no reader may hardcode
// `multivoid.log`: it asks here. Opens the log first, so the answer is the real one. Any thread.
std::wstring CurrentPath();
std::wstring PreviousPath();
// The path a process of this install writes while no other holds it: the exe directory's
// multivoid.log, or the VOTVCOOP_LOG name. The live log's third line names the process that writes
// it ("process <PID>"), so an overlapped launch can tell whose file the usual name is. Any thread.
std::wstring BasePath();

// The address mark. A log line that prints a peer's address, a typed dial text, or an endpoint
// that is not the project's own wraps the value in Addr(): it writes kAddrOpen + value +
// kAddrClose, so the bug-report redactor finds the span by its delimiters, since an address has
// no shape a scanner can tell from a version string. kLogFormatLine is line 2 of every log; bump
// the number when what is marked changes, so a report never trusts an older log's marks.
inline constexpr char kAddrOpen[] = "\xE2\x9F\xA8" "addr:";  // U+27E8 + "addr:"
inline constexpr char kAddrClose[] = "\xE2\x9F\xA9";          // U+27E9
inline constexpr char kLogFormatLine[] = "log format 1";
// MTA and Source print a peer's address raw in the server's own log (CGame.cpp:1411,
// EventLog.cpp:73), and MTA's client log, uploaded with a crash (CCrashDumpWriter.cpp:2450), holds
// the server it dialled (CConnectManager.cpp:157). Ours holds every peer's address and leaves in
// a report, so the site marks it.
// Pure, any thread, no lock, no log call. The value's own U+27E8 / U+27E9, CR and LF become
// '?', so a value cannot end its own mark or its line. Used as Addr(x).c_str() in a varargs.
std::string Addr(std::string_view value);

}  // namespace ue_wrap::log

#define UE_LOGI(...) ::ue_wrap::log::Write(::ue_wrap::log::Level::Info, __VA_ARGS__)
#define UE_LOGW(...) ::ue_wrap::log::Write(::ue_wrap::log::Level::Warn, __VA_ARGS__)
#define UE_LOGE(...) ::ue_wrap::log::Write(::ue_wrap::log::Level::Error, __VA_ARGS__)
