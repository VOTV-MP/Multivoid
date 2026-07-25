// coop/config/config_internal.h -- TU-private seams between the config
// reader core (config.cpp) and the ini mutation engine (config_ini_write.cpp).
//
// (Born with the arc-2 C6 extraction, 2026-07-25 -- config.cpp crossed the
// 800-LOC soft cap; the write/mutation side is its own concept. The s28
// internal-header pattern: shared primitives declared here, defined in
// config.cpp, NEVER exported to include/ -- product code uses the public
// coop/config/config.h API only.)

#pragma once

#include <functional>
#include <mutex>
#include <string>

namespace coop::config::internal {

// The tri-state scan verdict of the ONE line primitive (design F37/F38):
//   Ok         -- clean end of stream (feof, no ferror): ABSENT is authoritative;
//   Absent     -- the file does not exist (ENOENT at open);
//   Unreadable -- open failed otherwise (lock, perms) or a MID-STREAM error.
enum class IniScan { Ok = 0, Absent = 1, Unreadable = 2 };

// The one process-wide multivoid.ini lock (readers + writers + rebuilds).
std::mutex& IniMutex();

// ModuleDir()\multivoid.ini.
std::wstring LiveIniPath();

// Deliver every line of `path` (verbatim, unbounded, trailing newline kept)
// to `cb`; returns the tri-state. No lock -- callers hold IniMutex for the
// live ini; selftests feed corpus paths.
IniScan ScanIniFile(const std::wstring& path,
                    const std::function<void(const std::string&)>& cb);

// The shared lexer pieces: edge-trim; split "key=value" (edge-trimmed key,
// interior-verbatim value; false for '='-less lines / empty keys); the T5
// inline-comment strip (wsPrecededOnly = the string layer's narrowing).
std::string TrimEdgesStr(const std::string& s);
bool ParseIniKeyValue(const std::string& line, std::string& key, std::string& value);
std::string StripInlineCommentStr(const std::string& v, bool wsPrecededOnly);

}  // namespace coop::config::internal
