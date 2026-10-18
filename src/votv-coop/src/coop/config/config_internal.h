// coop/config/config_internal.h -- TU-private seams between the config reader core (config.cpp),
// the ini mutation engine (config_ini_write.cpp), the runtime layer (config_runtime.cpp), the
// session layer (config_session.cpp) and the selftest TU.
//
// The internal-header pattern: shared primitives are declared here and defined in config.cpp or
// config_ini_read.cpp, and never exported to include/ -- product code uses the public
// coop/config/config.h API only.

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

// <mod data dir>\multivoid.ini (ue_wrap::paths::ModDataDir); empty when the mod has no data folder.
std::wstring LiveIniPath();

// Deliver every line of `path` to `cb` -- unbounded, trailing newline kept, a CRLF ending as LF --
// and return the tri-state, with `faultOut` set on Unreadable. No lock: callers hold IniMutex for
// the live ini, and the selftests feed corpus paths.
IniScan ScanIniFile(const std::wstring& path,
                    const std::function<void(const std::string&)>& cb,
                    IniFault* faultOut = nullptr);

// The shared lexer pieces: edge-trim; split "key=value" with an edge-trimmed key and the value
// kept exactly as written between its edges, returning false for a line with no '=' and for an
// empty key; and the one cooker of a value's text (quoted values, the inline-comment cut), where
// wsPrecededOnly is the string layer's narrowing. The grammar is written above its definition.
std::string TrimEdgesStr(const std::string& s);
bool ParseIniKeyValue(const std::string& line, std::string& key, std::string& value);
std::string CookIniValue(const std::string& v, bool wsPrecededOnly);

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

// The layered raw-value pick: the session's value of a server-scope row while a session runs (the
// session layer), then a value set while the game runs (the runtime layer), then the environment
// variable, then the ini value, then absent. A set env wins over the ini, valid or not (garbage
// env shadows the ini). True with `raw` when a layer supplied a value, and `fromEnvOut` says
// which layer won (false when the session or the runtime layer answered). The census reports
// the layer, so it asks the precedence rule itself rather than re-reading the environment and
// risking a second, disagreeing answer.
// `scanOut` gets the ini scan's verdict (Ok when the env layer answered), because an absent
// result from an Unreadable scan is not an answer, and a fail-closed read must not take it as one;
// `faultOut` says why it was Unreadable.
bool PickRawLayered(const config_registry::Row* row, std::string& raw,
                    bool* fromEnvOut = nullptr, IniScan* scanOut = nullptr,
                    IniFault* faultOut = nullptr);

// A value for a log line: printable ASCII only (anything else becomes '?') and capped at 64
// characters, so a stored or wire-supplied value can never break the line. Defined in
// config_census.cpp; the census and the session layer both print through it.
std::string Printable(const std::string& raw);

// C-locale numeric emission for a float row's value, so the catalog's default and the census's
// resolved value are the same string on any machine. Defined in config_example.cpp.
std::string FormatFloat(float v);

// A row's default as the text its kind prints -- what the catalog writes and what a layer holds
// when nothing is configured: `defS`; a flag as `1`/`0`; an int with `%ld`; a float through
// FormatFloat. Defined in config_example.cpp.
std::string DefaultText(const config_registry::Row& r);

// What the row resolves to with no session layer: the runtime layer's entry if held, else the
// environment twin if set, else the ini's cooked value at `iniPath` if the line is there (read
// under IniMutex, so never call it under the layer lock), else DefaultText. What a session opens
// with on the host, and what a host-side reset leaves in force.
void RawBelowSession(const std::wstring& iniPath, const config_registry::Row* row,
                     std::string& raw);

// The writer's normalisation: CR and LF removed, and the edges trimmed when `trimEdges` (every
// row but a String row; config_ini_write.cpp).
std::string NormalizeValue(const char* value, bool trimEdges);
// A String row's value as its ini line holds it: `safe` itself unless the reader could not return
// it whole (it begins with '"' or ';', contains a space or tab before ';', or has an edge space or
// tab), then quoted with '\' and '"' escaped. Any other row kind (`stringRow` false) is never
// quoted.
std::string QuoteIniValueIfNeeded(const std::string& safe, bool stringRow);
// The one locked ini write: takes IniMutex, writes `key=value` into the ini at `path`.
bool WriteIniKeyAtPath(const std::wstring& path, const char* key, const char* value);
// The one locked line removal: takes IniMutex, drops every `key` line from the ini at `path`.
// `removed` is their count; false when the file could not be read or rewritten (nothing lost),
// true when it is absent or had no such line.
bool RemoveIniKeyAtPath(const std::wstring& path, const char* key, int& removed);

// The runtime layer (config_runtime.cpp): a row set while the game runs. Any thread.
bool RuntimeLayerGet(const config_registry::Row* row, std::string& raw);
void RuntimeLayerPut(const config_registry::Row* row, const std::string& raw);
void RuntimeLayerDrop(const config_registry::Row* row);
// Whether `row` has a subscriber; and: call each of them, on the calling thread.
bool HasSubscriber(const config_registry::Row* row);
void NotifySubscribers(const config_registry::Row* row);
// Deliver a change of `row` to its subscribers: through the notifier when one is set, else on the
// calling thread; nothing when the row has none.
void PostNotify(const config_registry::Row* row);
// The lock that makes a change of the runtime layer and its ini write one step: SetValueAt,
// ResetValueAt, the keep-line, SessionLayerBegin and SessionLayerEnd hold it. Taken before
// IniMutex, never after; never held across a notification.
std::mutex& SetMutex();
// The runtime layer's mutex, which also guards the session layer's map and the subscriber list.
// Taken after SetMutex, never before; the ini is never read under it, and no call out is made
// while it is held.
std::mutex& LayerMutex();

// The session layer (config_session.cpp): the session's value of each server-scope row, above the
// runtime layer. Any thread.
// 0 none, 1 host, 2 client.
int SessionRole();
// The session's value of `row`: false unless a session runs and the row is held.
bool SessionLayerGet(const config_registry::Row* row, std::string& raw);
// The host's in-setter put: SetValueAt and ResetValueAt call it with the set lock held, before
// their one notification, so it notifies no subscriber; it marks a changed notify row for
// announcement (TakePendingAnnouncements).
void SessionLayerPutNoNotify(const config_registry::Row* row, const std::string& raw);
// The one sanitiser a value passes before it enters the host's session layer (a session's start,
// a reset): `raw` when the reader accepts it and, for a replicated row, the wire can carry it;
// else the row's DefaultText, logged without the value. `why` names the caller in that line.
std::string SessionSafe(const config_registry::Row* row, const std::string& raw, const char* why);
// The keep-line whole: the dedup of `key` in the ini at `path`, then, on success, the row dropped
// from the runtime layer and its subscribers told. The public RemoveDuplicateKeyLines passes
// LiveIniPath(); the selftest a scratch file.
bool RemoveDuplicateKeyLinesLayered(const std::wstring& path, const char* key,
                                    const char* keepValue);
// SetValue's whole body, with the ini to write as a parameter: SetValue passes LiveIniPath(), the
// selftest a scratch file.
SetResult SetValueAt(const std::wstring& iniPath, const config_registry::Row* row,
                     const char* value);
// ResetValue's whole body, the same way: the row dropped from the runtime layer, its line removed
// from the ini at `iniPath`, logged, announced.
SetResult ResetValueAt(const std::wstring& iniPath, const config_registry::Row* row);

}  // namespace coop::config::internal
