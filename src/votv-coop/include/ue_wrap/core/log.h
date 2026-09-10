// ue_wrap/core/log.h -- minimal levelled logger for the standalone mod.
//
// Writes to multivoid.log beside the game exe. The point is fast diagnosis:
// when the mod is brought up against a new game build and something is wrong,
// the log says exactly which primitive failed to resolve or validate, instead
// of a silent crash. Thread-safe; lazy-initialises on first use.

#pragma once

namespace ue_wrap::log {

enum class Level { Info, Warn, Error };

// Open/truncate the log file and write a header. Optional; Write() lazy-inits.
//
// There is deliberately no close: nothing sets the FILE* back to null once it is open, so the
// un-locked null test every Write() and Flush() starts with cannot race a teardown, and the
// process exit is what closes the handle. The one-second sync below is what makes that safe.
void Init();

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

}  // namespace ue_wrap::log

#define UE_LOGI(...) ::ue_wrap::log::Write(::ue_wrap::log::Level::Info, __VA_ARGS__)
#define UE_LOGW(...) ::ue_wrap::log::Write(::ue_wrap::log::Level::Warn, __VA_ARGS__)
#define UE_LOGE(...) ::ue_wrap::log::Write(::ue_wrap::log::Level::Error, __VA_ARGS__)
