// coop/config/config_selftest.h -- the dev selftest seams of the config layer: path-parameterised
// twins of the readers and writers, and the selftests of the runtime layer, the quoted-value
// grammar, the announce decision and the generated catalog. Defined in config_selftest.cpp, and
// where a seam sits beside the code it twins, in config_example.cpp (SelftestExampleVerify) and
// config_ini_write.cpp (SelftestRemoveDuplicates, SelftestReformat, SelftestWriteValue). Used by
// the autotest and the selftests only; never by product code.

#pragma once

#include "coop/config/config.h"
#include "coop/config/config_registry.h"

#include <string>
#include <vector>

namespace coop::config {

// The selftest verification of a generated catalog: the detectors (grammar, wrap, exactly-once
// in each direction, env-only, orphan, section placement) and the round trip (every copyable
// line uncommented into `scratchPath`, through the one lexer, found and typed-equal to the row
// defaults). Returns the failure count; each failure logs one catalog FAIL line.
int SelftestExampleVerify(const std::wstring& examplePath, const std::wstring& scratchPath);

// The dev selftest seams: path-parameterised twins of the readers, the raw line list and a
// failing-source scan, so the env-gated autotest runs the real lexer over corpus ini files and
// proves the tri-state branches. Not for product use. `scan` codes: 0 ok, 1 absent, 2
// unreadable (an open failure other than absence, a mid-stream read error, or bytes that are not
// text), with `fault` saying which.
struct IniSelftestRead {
    int scan = 0;
    IniFault fault = IniFault::None;
    bool found = false;
    std::string value;
};
IniSelftestRead SelftestReadValue(const std::wstring& path, const char* key);
// The live ini's stored value of `key`, read from the file under the ini lock, never from the
// runtime layer or the environment: for a drill that asserts what the NEXT launch will read.
IniSelftestRead SelftestReadLiveValue(const char* key);
int SelftestFlagTriState(const std::wstring& path, const char* key);
// The typed-resolver twins: the ini and default halves of the layered resolve over `path`, the
// same per-kind validate and default cores as the live Resolve functions, with no env layer.
// The env layer is drilled by its own control on the live resolver through a dedicated
// env-twinned row.
bool        SelftestResolveFlagAt(const std::wstring& path, const config_registry::FlagRow& row);
long        SelftestResolveIntAt(const std::wstring& path, const config_registry::IntRow& row);
float       SelftestResolveFloatAt(const std::wstring& path, const config_registry::FloatRow& row);
std::string SelftestResolveEnumAt(const std::wstring& path, const config_registry::EnumRow& row);
std::string SelftestResolveStringAt(const std::wstring& path, const config_registry::StringRow& row);
FailClosedRead SelftestResolveFailClosedAt(const std::wstring& path,
                                           const config_registry::FailClosedEnumRow& row,
                                           std::string& out, IniFault* faultOut);
int SelftestListLines(const std::wstring& path, std::vector<std::string>& out);
int SelftestScanWithFailure(int failAfterLines);
bool SelftestWriteValue(const std::wstring& path, const char* key, const char* value);
bool SelftestRemoveDuplicates(const std::wstring& path, const char* key, const char* keepValue);
bool SelftestReformat(const std::wstring& path, ReformatStats& stats);
// The runtime layer's selftest: drives SetValue's own code against a scratch ini in the mod's folder
// (never the live ini). `drain` returns once every notification posted so far has run;
// `onNotifyThread` says whether the caller is on the thread notifications run on. Returns the
// failure count; each check logs one config-selftest line. Not on the game thread.
int SelftestRuntimeLayer(void (*drain)(), bool (*onNotifyThread)());
// The quoted-value grammar's selftest on a scratch ini in the mod's folder: what the writer quotes, the
// reader returns whole. Returns the failure count; each check logs one config-selftest line.
int SelftestQuotedValues();
// ShouldAnnounce on pure inputs; returns the failure count, one config-selftest line per check.
int SelftestAnnounce();

}  // namespace coop::config
