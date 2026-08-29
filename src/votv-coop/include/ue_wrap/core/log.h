// ue_wrap/log.h -- minimal levelled logger for the standalone mod.
//
// Writes to multivoid.log beside the game exe. The point is fast diagnosis:
// when the mod is brought up against a new game build and something is wrong,
// the log says exactly which primitive failed to resolve or validate, instead
// of a silent crash. Thread-safe; lazy-initialises on first use.

#pragma once

namespace ue_wrap::log {

enum class Level { Info, Warn, Error };

// Open/truncate the log file and write a header. Optional; Write() lazy-inits.
void Init();
void Shutdown();

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

// THE STANDING GUARANTEE (2026-08-29): the log on disk is never more than one
// second behind the process. INFO rides the CRT's ~4 KB buffer for the perf
// reason in log.cpp, but a write that finds the buffer older than that syncs it,
// so an abnormal exit -- a kill, a crash, a close that misses Shutdown() -- can
// cost at most the lines written in the final second of activity. You do NOT have
// to call Flush() to make a post-mortem readable; that used to be true and it is
// why a four-minute session once left a 65-line log ending mid-boot.
//
// Force the CRT stdio buffer to disk NOW, ahead of that bound. Worth it only when
// something EXTERNAL is about to read the file and cannot wait a second -- a test
// runner polling for a verdict line, or a boot milestone you want tailable at
// once. Do NOT call on a hot path: this is a synchronous disk sync, and per-line
// flushing is exactly what the 2026-05-27 audit removed.
void Flush();

}  // namespace ue_wrap::log

#define UE_LOGI(...) ::ue_wrap::log::Write(::ue_wrap::log::Level::Info, __VA_ARGS__)
#define UE_LOGW(...) ::ue_wrap::log::Write(::ue_wrap::log::Level::Warn, __VA_ARGS__)
#define UE_LOGE(...) ::ue_wrap::log::Write(::ue_wrap::log::Level::Error, __VA_ARGS__)
