// ue_wrap/log.h -- minimal levelled logger for the standalone mod.
//
// Writes to multivoid.log next to the mod DLL. The point is fast diagnosis:
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

// Force the CRT stdio buffer to disk NOW. INFO lines normally ride the ~4 KB
// buffer (flushed only on WARN/ERROR -- a perf fix so hot-path INFO spam doesn't
// trigger a synchronous disk sync per line). Call this at a LOW-FREQUENCY boot
// milestone (e.g. once the boot mode is decided) so the whole boot sequence
// (banner -> version -> HEALTH -> mode-ready) is visible on disk immediately for
// live-tailing / post-mortem, WITHOUT re-introducing per-INFO flush cost. Do NOT
// call on a hot path.
void Flush();

}  // namespace ue_wrap::log

#define UE_LOGI(...) ::ue_wrap::log::Write(::ue_wrap::log::Level::Info, __VA_ARGS__)
#define UE_LOGW(...) ::ue_wrap::log::Write(::ue_wrap::log::Level::Warn, __VA_ARGS__)
#define UE_LOGE(...) ::ue_wrap::log::Write(::ue_wrap::log::Level::Error, __VA_ARGS__)
