// ue_wrap/log.h -- minimal levelled logger for the standalone mod.
//
// Writes to votv-coop.log next to the mod DLL. The point is fast diagnosis:
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

}  // namespace ue_wrap::log

#define UE_LOGI(...) ::ue_wrap::log::Write(::ue_wrap::log::Level::Info, __VA_ARGS__)
#define UE_LOGW(...) ::ue_wrap::log::Write(::ue_wrap::log::Level::Warn, __VA_ARGS__)
#define UE_LOGE(...) ::ue_wrap::log::Write(::ue_wrap::log::Level::Error, __VA_ARGS__)
