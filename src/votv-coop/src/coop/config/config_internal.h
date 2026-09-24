// coop/config/config_internal.h -- TU-private seams between the config reader core (config.cpp)
// and the ini mutation engine (config_ini_write.cpp).
//
// The internal-header pattern: shared primitives are declared here and defined in config.cpp,
// and never exported to include/ -- product code uses the public coop/config/config.h API only.

#pragma once

#include "coop/config/config.h"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace coop::config_registry { struct Row; }

namespace coop::config::internal {

// The tri-state scan verdict of the ONE line primitive:
//   Ok         -- clean end of stream (feof, no ferror): ABSENT is authoritative;
//   Absent     -- the file does not exist (ENOENT at open);
//   Unreadable -- open failed otherwise (lock, perms), a MID-STREAM error, or bytes that are not
//                 text (a zero byte, a UTF-16 byte-order mark); the IniFault says which. It stays
//                 the one refusing verdict, so no consumer can miss a newer kind of failure.
enum class IniScan { Ok = 0, Absent = 1, Unreadable = 2 };

// The one process-wide multivoid.ini lock (readers + writers + rebuilds).
std::mutex& IniMutex();

// <exe dir>\multivoid.ini (ue_wrap::paths::ExeDir, the install-dir anchor).
std::wstring LiveIniPath();

// Deliver every line of `path` to `cb` -- unbounded, trailing newline kept, a CRLF ending as LF --
// and return the tri-state, with `faultOut` set on Unreadable. No lock: callers hold IniMutex for
// the live ini, and the selftests feed corpus paths.
IniScan ScanIniFile(const std::wstring& path,
                    const std::function<void(const std::string&)>& cb,
                    IniFault* faultOut = nullptr);

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
                               IniScan* scanOut, IniFault* faultOut = nullptr);
// The live ini's raw read, under the ini lock and recording its fault like every live read: for a
// TU that must compare a stored value as written (config_example's retired-value migration).
std::string ReadLiveIniValue(const char* key, const char* def, IniScan* scanOut,
                             IniFault* faultOut);
int LookupTriStateAtPath(const std::wstring& path, const char* key);
int ScanWithInjectedFailure(int failAfterLines);
bool FlagFromRaw(const config_registry::Row* row, bool have, const std::string& raw);
long IntFromRaw(const config_registry::Row* row, bool have, const std::string& raw);
float FloatFromRaw(const config_registry::Row* row, bool have, const std::string& raw);
std::string EnumFromRaw(const config_registry::Row* row, bool have, const std::string& raw);
// The fail-closed verdict over a layered pick (`scan` is Ok when the env layer answered): one core
// for ResolveFailClosed and its selftest twin.
FailClosedRead FailClosedFromPick(const config_registry::Row* row, bool have,
                                  const std::string& raw, bool fromEnv, IniScan scan,
                                  IniFault fault, std::string& out, std::string* refusedOut,
                                  std::string* originOut, IniFault* faultOut);

// The layered raw-value pick: a set env wins, valid or not (garbage env shadows the ini); else
// the ini's authoritative line; else absent. True with `raw` when a layer supplied a value, and
// `fromEnvOut` says which layer won. The census reports the layer, so it asks the precedence
// rule itself rather than re-reading the environment and risking a second, disagreeing answer.
// `scanOut` gets the ini scan's verdict (Ok when the env layer answered), because an absent
// result from an Unreadable scan is not an answer, and a fail-closed read must not take it as one;
// `faultOut` says why it was Unreadable.
bool PickRawLayered(const config_registry::Row* row, std::string& raw,
                    bool* fromEnvOut = nullptr, IniScan* scanOut = nullptr,
                    IniFault* faultOut = nullptr);

// C-locale numeric emission for a float row's value, so the catalog's default and the census's
// resolved value are the same string on any machine. Defined in config_example.cpp.
std::string FormatFloat(float v);

// The ONE atomic-swap file writer (.new + checked writes + MoveFileExW),
// shared with the T8 catalog generator (config_example.cpp; arc 4) -- never a
// second swap implementation. Defined in config_ini_write.cpp.
bool AtomicWriteAllLines(const std::wstring& path, const std::vector<std::string>& lines,
                         const char* what);

}  // namespace coop::config::internal
