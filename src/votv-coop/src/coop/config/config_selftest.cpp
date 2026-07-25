// coop/config/config_selftest.cpp -- the dev selftest seams of the config
// corpus instrument (probes are RULE-2-exempt). Path-parameterized twins of
// the two readers + the raw line list + the injected-failure scan + the typed
// resolver twins, so the env-gated autotest (harness/autotest_config.cpp) can
// run the REAL lexer over corpus ini files and prove the tri-state branches
// and the arc-3 default/sentinel semantics. Not for product use: product code
// reads only the module-dir ini via the public config.h API.
//
// Extracted from config.cpp (arc 3, soft-cap discipline -- the C5a twins
// pushed it past 800 LOC; the instrument seams are their own concept). All
// semantics live in config.cpp's internal:: cores (*FromRaw, the path readers)
// -- this TU is glue only, so product and instrument cannot diverge.

#include "coop/config/config.h"

#include "config_internal.h"
#include "coop/config/config_registry.h"

#include <string>
#include <vector>

namespace coop::config {

using IniScan = internal::IniScan;

IniSelftestRead SelftestReadValue(const std::wstring& path, const char* key) {
    IniScan st = IniScan::Ok;
    IniSelftestRead r;
    const std::string sentinel = "\x01<absent>";
    r.value = internal::ReadIniValueAtPath(path, key, sentinel.c_str(), &st);
    r.found = (r.value != sentinel);
    if (!r.found) r.value.clear();
    r.scan = static_cast<int>(st);
    return r;
}

int SelftestFlagTriState(const std::wstring& path, const char* key) {
    return internal::LookupTriStateAtPath(path, key);
}

int SelftestListLines(const std::wstring& path, std::vector<std::string>& out) {
    const IniScan st =
        internal::ScanIniFile(path, [&](const std::string& line) { out.push_back(line); });
    return static_cast<int>(st);
}

int SelftestScanWithFailure(int failAfterLines) {
    return internal::ScanWithInjectedFailure(failAfterLines);
}

// ---- typed-resolver twins (arc 3 C5; see config.h) --------------------------

namespace {

// The twins' ini pick: same authoritative-line read as the live layer, over
// `path` instead of the module ini; NO env layer (the env layer is drilled by
// its own live-resolver control -- see config.h).
bool PickIniAt(const std::wstring& path, const config_registry::Row* row, std::string& raw) {
    static const char* kAbsent = "\x01<absent>";
    IniScan st = IniScan::Ok;
    const std::string v = internal::ReadIniValueAtPath(path, row->key, kAbsent, &st);
    if (v == kAbsent) return false;
    raw = v;
    return true;
}

}  // namespace

bool SelftestResolveFlagAt(const std::wstring& path, const config_registry::FlagRow& h) {
    std::string raw;
    const bool have = PickIniAt(path, h.row, raw);
    return internal::FlagFromRaw(h.row, have, raw);
}
long SelftestResolveIntAt(const std::wstring& path, const config_registry::IntRow& h) {
    std::string raw;
    const bool have = PickIniAt(path, h.row, raw);
    return internal::IntFromRaw(h.row, have, raw);
}
float SelftestResolveFloatAt(const std::wstring& path, const config_registry::FloatRow& h) {
    std::string raw;
    const bool have = PickIniAt(path, h.row, raw);
    return internal::FloatFromRaw(h.row, have, raw);
}
std::string SelftestResolveEnumAt(const std::wstring& path, const config_registry::EnumRow& h) {
    std::string raw;
    const bool have = PickIniAt(path, h.row, raw);
    return internal::EnumFromRaw(h.row, have, raw);
}
std::string SelftestResolveStringAt(const std::wstring& path, const config_registry::StringRow& h) {
    std::string raw;
    if (!PickIniAt(path, h.row, raw)) return h.row->defS;
    return raw;
}

}  // namespace coop::config
