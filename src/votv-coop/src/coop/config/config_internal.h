// coop/config/config_internal.h -- TU-private seams between the config reader core (config.cpp)
// and the ini mutation engine (config_ini_write.cpp).
//
// The internal-header pattern: shared primitives are declared here and defined in config.cpp,
// and never exported to include/ -- product code uses the public coop/config/config.h API only.

#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace coop::config_registry { struct Row; }

namespace coop::config::internal {

// The tri-state scan verdict of the ONE line primitive:
//   Ok         -- clean end of stream (feof, no ferror): ABSENT is authoritative;
//   Absent     -- the file does not exist (ENOENT at open);
//   Unreadable -- open failed otherwise (lock, perms) or a MID-STREAM error.
enum class IniScan { Ok = 0, Absent = 1, Unreadable = 2 };

// The one process-wide multivoid.ini lock (readers + writers + rebuilds).
std::mutex& IniMutex();

// <exe dir>\multivoid.ini (ue_wrap::paths::ExeDir, the install-dir anchor).
std::wstring LiveIniPath();

// Deliver every line of `path` to `cb` unchanged -- unbounded, trailing newline kept -- and
// return the tri-state. No lock: callers hold IniMutex for the live ini, and the selftests feed
// corpus paths.
IniScan ScanIniFile(const std::wstring& path,
                    const std::function<void(const std::string&)>& cb);

// The shared lexer pieces: edge-trim; split "key=value" with an edge-trimmed key and the value
// kept exactly as written between its edges, returning false for a line with no '=' and for an
// empty key; and the inline-comment strip, where wsPrecededOnly is the string layer's
// narrowing.
std::string TrimEdgesStr(const std::string& s);
bool ParseIniKeyValue(const std::string& line, std::string& key, std::string& value);
std::string StripInlineCommentStr(const std::string& v, bool wsPrecededOnly);

// ---- seams for the selftest TU (config_selftest.cpp; arc-3 soft-cap cut) ----
// The path-parameterized reader cores (no lock -- corpus paths only) + the
// injected-failure scan + the per-kind validate/default cores shared with the
// live Resolve* (ONE semantics for product and instrument, by construction).
std::string ReadIniValueAtPath(const std::wstring& path, const char* key, const char* def,
                               IniScan* scanOut);
int LookupTriStateAtPath(const std::wstring& path, const char* key);
int ScanWithInjectedFailure(int failAfterLines);
bool FlagFromRaw(const config_registry::Row* row, bool have, const std::string& raw);
long IntFromRaw(const config_registry::Row* row, bool have, const std::string& raw);
float FloatFromRaw(const config_registry::Row* row, bool have, const std::string& raw);
std::string EnumFromRaw(const config_registry::Row* row, bool have, const std::string& raw);

// The ONE atomic-swap file writer (.new + checked writes + MoveFileExW),
// shared with the T8 catalog generator (config_example.cpp; arc 4) -- never a
// second swap implementation. Defined in config_ini_write.cpp.
bool AtomicWriteAllLines(const std::wstring& path, const std::vector<std::string>& lines,
                         const char* what);

}  // namespace coop::config::internal
