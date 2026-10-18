// coop/config/config_selftest.cpp -- the dev selftest seams of the config
// corpus instrument (probes are RULE-2-exempt). Path-parameterized twins of
// the two readers + the raw line list + the injected-failure scan + the typed
// resolver twins, so the env-gated autotest (harness/autotest/autotest_config.cpp) can
// run the REAL lexer over corpus ini files and prove the tri-state branches
// and the arc-3 default/sentinel semantics. The runtime layer's selftest also lives here:
// SelftestRuntimeLayer drives SetValue's own code against a scratch ini, and SelftestQuotedValues
// the writer's quoting against the reader's, and SelftestAnnounce the notify decision
// on pure inputs. Not for product use: product code reads only the module-dir ini
// via the public config.h API.
//
// Extracted from config.cpp (arc 3, soft-cap discipline -- the C5a twins
// pushed it past 800 LOC; the instrument seams are their own concept). All
// semantics live in config.cpp's internal:: cores (*FromRaw, the path readers)
// -- this TU is glue only, so product and instrument cannot diverge.

#include "coop/config/config.h"

#include "config_internal.h"
#include "coop/config/config_registry.h"
#include "coop/config/config_selftest.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::config {

using IniScan = internal::IniScan;

IniSelftestRead SelftestReadValue(const std::wstring& path, const char* key) {
    IniScan st = IniScan::Ok;
    IniFault fault = IniFault::None;
    IniSelftestRead r;
    const std::string sentinel = "\x01<absent>";
    r.value = internal::ReadIniValueAtPath(path, key, sentinel.c_str(), &st, &fault);
    r.found = (r.value != sentinel);
    if (!r.found) r.value.clear();
    r.scan = static_cast<int>(st);
    r.fault = fault;
    return r;
}

IniSelftestRead SelftestReadLiveValue(const char* key) {
    IniScan st = IniScan::Ok;
    IniFault fault = IniFault::None;
    IniSelftestRead r;
    const std::string sentinel = "\x01<absent>";
    r.value = internal::ReadLiveIniValue(key, sentinel.c_str(), &st, &fault);
    r.found = (r.value != sentinel);
    if (!r.found) r.value.clear();
    r.scan = static_cast<int>(st);
    r.fault = fault;
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

// ---- typed-resolver twins (arc 3 C5; see config_selftest.h) -----------------

namespace {

// The twins' ini pick: same authoritative-line read as the live layer, over
// `path` instead of the module ini; NO env layer (the env layer is drilled by
// its own live-resolver control -- see config_selftest.h).
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
FailClosedRead SelftestResolveFailClosedAt(const std::wstring& path,
                                           const config_registry::FailClosedEnumRow& h,
                                           std::string& out, IniFault* faultOut) {
    static const char* kAbsent = "\x01<absent>";
    IniScan st = IniScan::Ok;
    IniFault fault = IniFault::None;
    const std::string v = internal::ReadIniValueAtPath(path, h.row->key, kAbsent, &st, &fault);
    const bool have = v != kAbsent;
    return internal::FailClosedFromPick(h.row, have, have ? v : std::string(), /*fromEnv*/ false,
                                        st, fault, out, nullptr, nullptr, faultOut);
}

// ---- the runtime layer (config_runtime.cpp) ----------------------------------

namespace {

// The probe row's subscriber: counts its calls and records whether it ran on the thread the
// notifications are meant for. Both subscribers below stay registered on the probe row after the
// run (the layer has no unsubscribe), so a later keep-line, set or reset on that key prints a
// stray REFUSED warning from the second one.
std::atomic<int> g_subscriberCalls{0};
std::atomic<bool> g_subscriberOnNotifyThread{false};
bool (*g_onNotifyThread)() = nullptr;

// A subscriber that tries to set the row: the layer refuses it. The scratch path is the run's,
// kept here because a subscriber takes no arguments.
std::atomic<int> g_nestedSetResult{-1};
std::wstring g_scratchPath;

void ProbeSubscriber() {
    g_subscriberCalls.fetch_add(1);
    g_subscriberOnNotifyThread.store(g_onNotifyThread && g_onNotifyThread());
}

void NestedSetSubscriber() {
    g_nestedSetResult.store(static_cast<int>(internal::SetValueAt(
        g_scratchPath, config_registry::rows::selftest_runtime_probe.row, "9")));
}

}  // namespace

int SelftestRuntimeLayer(void (*drain)(), bool (*onNotifyThread)()) {
    namespace reg = config_registry;
    const reg::IntRow& P = reg::rows::selftest_runtime_probe;
    const std::wstring dataDir = ue_wrap::paths::ModDataDir();
    if (dataDir.empty()) {
        UE_LOGI("config-selftest: runtime layer skipped -- no data folder");
        return 0;
    }
    const std::wstring scratch = dataDir + L"\\multivoid.selftest-runtime.ini";
    const char* const kEnv = "VOTVCOOP_SELFTEST_RUNTIME_PROBE";
    int fail = 0;
    auto expect = [&](const char* what, bool ok) {
        if (ok) UE_LOGI("config-selftest: runtime layer %s ok", what);
        else { UE_LOGW("config-selftest: FAIL runtime layer %s", what); ++fail; }
    };

    // The environment twin is cleared so check 0 sees the row's default whatever the launch pinned,
    // and restored after check 12.
    char old[256] = {};
    const DWORD oldLen = ::GetEnvironmentVariableA(kEnv, old, sizeof(old));
    ::SetEnvironmentVariableA(kEnv, nullptr);
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, scratch.c_str(), L"w") == 0 && f) std::fclose(f);
    }
    g_onNotifyThread = onNotifyThread;
    g_scratchPath = scratch;

    // 0: a peer whose ini holds the probe row fails here, so no later check can pass or fail for
    // that reason.
    expect("clean probe", ResolveInt(P) == 0);

    // 1: the red knob skips the set, so every check that reads the value it would have set fails.
    {
        bool setOk = true;
        if (!ResolveFlag(reg::rows::selftest_break_runtime_layer))
            setOk = internal::SetValueAt(scratch, P.row, "7") == SetResult::Saved;
        expect("set -> resolve", setOk && ResolveInt(P) == 7);
    }
    // 2: the ini holds what was set.
    {
        const IniSelftestRead r = SelftestReadValue(scratch, P.row->key);
        expect("written", r.found && r.value == "7");
    }
    // 3: the layer answers above the environment twin.
    ::SetEnvironmentVariableA(kEnv, "3");
    expect("layer above env", ResolveInt(P) == 7);
    // 4: a refused value changes nothing.
    {
        const SetResult r = internal::SetValueAt(scratch, P.row, "banana");
        expect("refused value", r == SetResult::Refused && ResolveInt(P) == 7);
    }
    // 5: the value is held as the writer normalises it.
    {
        const SetResult r = internal::SetValueAt(scratch, P.row, " 5\r\n");
        std::string raw;
        const bool got = internal::RuntimeLayerGet(P.row, raw);
        expect("normalised", r == SetResult::Saved && got && raw == "5");
    }
    // 6: one call per set however often the pair was registered, on the notification thread.
    {
        g_subscriberCalls.store(0);
        g_subscriberOnNotifyThread.store(false);
        g_nestedSetResult.store(-1);
        Subscribe(P, &ProbeSubscriber);
        Subscribe(P, &ProbeSubscriber);
        Subscribe(P, &NestedSetSubscriber);
        internal::SetValueAt(scratch, P.row, "6");
        drain();
        expect("subscriber on the game thread",
               g_subscriberCalls.load() == 1 && g_subscriberOnNotifyThread.load());
        // 6b: the set made from inside the subscriber was refused and changed nothing.
        expect("refused inside a subscriber",
               g_nestedSetResult.load() == static_cast<int>(SetResult::Refused) &&
                   ResolveInt(P) == 6);
    }
    // 7: with the layer entry dropped, the environment answers again.
    internal::RuntimeLayerDrop(P.row);
    expect("drop -> env answers", ResolveInt(P) == 3);
    // 8: the keep-line over a scratch file with two copies of the key drops the layer entry and
    // tells the subscribers again (check 6's function: the counter reaches 2).
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, scratch.c_str(), L"w") == 0 && f) {
            std::fputs("selftest_runtime_probe=1\nselftest_runtime_probe=2\n", f);
            std::fclose(f);
        }
        internal::RuntimeLayerPut(P.row, "4");
        const bool kept = internal::RemoveDuplicateKeyLinesLayered(
            scratch, P.row->key, "2");
        std::string raw;
        const bool stillHeld = internal::RuntimeLayerGet(P.row, raw);
        drain();
        expect("keep-line drops the layer",
               kept && !stillHeld && g_subscriberCalls.load() == 2);
    }
    // 9: a reset forgets the layer entry and removes the key's line, so the environment twin
    // answers again, and tells the subscribers. The counter: check 6's function stays subscribed
    // (the layer has no unsubscribe), so it ran at 6, 8, this set and this reset.
    {
        internal::SetValueAt(scratch, P.row, "8");
        drain();
        const SetResult r = internal::ResetValueAt(scratch, P.row);
        drain();
        expect("reset -> default, line gone, told",
               r == SetResult::Saved && ResolveInt(P) == 3 &&
                   !SelftestReadValue(scratch, P.row->key).found &&
                   g_subscriberCalls.load() == 4);
    }
    // 10 and 11: a label and the live mark are read from the row list; the probe row is a dev row
    // that has neither.
    expect("label of a labelled row",
           reg::RowLabel(reg::rows::voice_distance_cm.row) != nullptr &&
               std::string(reg::RowLabel(reg::rows::voice_distance_cm.row)) == "Voice range");
    expect("no label on a dev row", reg::RowLabel(P.row) == nullptr);
    expect("live mark", reg::IsLive(reg::rows::voice_distance_cm.row) && !reg::IsLive(P.row));
    // 12: a by-name setter refuses a null row, a pointer outside the table and a local row before it
    // writes anything. The Saved path is not here: a server row's set changes live state.
    {
        const reg::Row stray{};
        const bool refused =
            SetServerRow(nullptr, "1") == SetResult::Refused &&
            ResetServerRow(nullptr) == SetResult::Refused &&
            SetServerRow(&stray, "1") == SetResult::Refused &&
            ResetServerRow(&stray) == SetResult::Refused &&
            SetServerRow(P.row, "1") == SetResult::Refused &&
            ResetServerRow(P.row) == SetResult::Refused;
        expect("by-name setters refuse a row that is not server-scope",
               refused && !SelftestReadLiveValue(P.row->key).found && ResolveInt(P) == 3);
    }

    ::SetEnvironmentVariableA(kEnv, (oldLen > 0 && oldLen < sizeof(old)) ? old : nullptr);
    ::DeleteFileW(scratch.c_str());
    return fail;
}

int SelftestQuotedValues() {
    namespace reg = config_registry;
    const std::wstring dataDir = ue_wrap::paths::ModDataDir();
    if (dataDir.empty()) {
        UE_LOGI("config-selftest: quoting skipped -- no data folder");
        return 0;
    }
    const std::wstring scratch = dataDir + L"\\multivoid.selftest-quoting.ini";
    const char* const kNick = "net.nick";
    // Read here and never in the product path: a read inside the writer would take the ini lock
    // the writer already holds.
    const bool breakQuoting = ResolveFlag(reg::rows::selftest_break_quoting);
    int fail = 0;
    auto expect = [&](const char* what, bool ok) {
        if (ok) UE_LOGI("config-selftest: quoting %s ok", what);
        else { UE_LOGW("config-selftest: FAIL quoting %s", what); ++fail; }
    };
    auto truncate = [&] {
        FILE* f = nullptr;
        if (_wfopen_s(&f, scratch.c_str(), L"w") == 0 && f) std::fclose(f);
    };
    auto putRaw = [&](const char* text) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, scratch.c_str(), L"w") == 0 && f) {
            std::fputs(text, f);
            std::fclose(f);
        }
    };
    // Whether the file holds `want` as a whole line, its newline (LF or CR LF) stripped.
    auto holdsLine = [&](const char* want) {
        std::vector<std::string> lines;
        SelftestListLines(scratch, lines);
        for (std::string& l : lines) {
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            if (l == want) return true;
        }
        return false;
    };
    // Writes `value` to `key` on a fresh file and says whether the writer took it.
    auto writeFresh = [&](const char* key, const char* value) {
        truncate();
        return SelftestWriteValue(scratch, key, value);
    };
    truncate();

    // 1 and 2: one write, read back whole and held quoted. The red knob puts the line there
    // unquoted by hand, skipping the step under test, so the value reads back cut.
    {
        bool wrote = true;
        if (breakQuoting) putRaw("net.nick=Bob ;)\n");
        else wrote = SelftestWriteValue(scratch, kNick, "Bob ;)");
        expect("round-trips a ;-value",
               wrote && SelftestReadValue(scratch, kNick).value == "Bob ;)");
        expect("the line is quoted", holdsLine("net.nick=\"Bob ;)\""));
    }
    // 3: a quote inside a quoted value is escaped.
    {
        const bool wrote = writeFresh(kNick, "say \"hi\" ; now");
        expect("escapes a quote",
               wrote && SelftestReadValue(scratch, kNick).value == "say \"hi\" ; now" &&
                   holdsLine("net.nick=\"say \\\"hi\\\" ; now\""));
    }
    // 4: so is a backslash, which a closing quote must not be taken for.
    {
        const bool wrote = writeFresh(kNick, "a ;b\\");
        expect("escapes a backslash",
               wrote && SelftestReadValue(scratch, kNick).value == "a ;b\\" &&
                   holdsLine("net.nick=\"a ;b\\\\\""));
    }
    // 5: a String row keeps its edge spaces, quoted.
    {
        const bool wrote = writeFresh(kNick, " pw ");
        expect("keeps edge spaces",
               wrote && SelftestReadValue(scratch, kNick).value == " pw " &&
                   holdsLine("net.nick=\" pw \""));
    }
    // 6: a value the reader returns whole is written as it is.
    {
        const bool wrote = writeFresh(kNick, "Bob");
        expect("a plain value stays plain", wrote && holdsLine("net.nick=Bob"));
    }
    // 7: a line written by hand with an inline comment still reads cut at it.
    {
        putRaw("net.nick=Legacy ; comment\n");
        expect("a legacy comment still cuts", SelftestReadValue(scratch, kNick).value == "Legacy");
    }
    // 8: a typed row is trimmed and never quoted.
    {
        const bool wrote = writeFresh("selftest_runtime_probe", " 7 ");
        expect("a typed row stays plain",
               wrote && holdsLine("selftest_runtime_probe=7") &&
                   SelftestReadValue(scratch, "selftest_runtime_probe").value == "7");
    }

    ::DeleteFileW(scratch.c_str());
    return fail;
}

int SelftestAnnounce() {
    namespace reg = config_registry;
    const bool breakAnnounce = ResolveFlag(reg::rows::selftest_break_announce);
    int fail = 0;
    auto expect = [&](const char* what, bool ok) {
        if (ok) UE_LOGI("config-selftest: announce %s ok", what);
        else { UE_LOGW("config-selftest: FAIL announce %s", what); ++fail; }
    };
    const auto& row = *reg::rows::voice_distance_cm.row;
    expect("equal spelling", !ShouldAnnounce(row, "4800", "4800.0"));
    expect("changed", ShouldAnnounce(row, "4800", breakAnnounce ? "4800" : "6000"));
    expect("not notify", !ShouldAnnounce(*reg::rows::net_ratecontrol.row, "0", "1"));
    return fail;
}

}  // namespace coop::config
