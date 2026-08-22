// coop/config/config_example.cpp -- the T8 generated settings catalog
// `multivoid.ini.example` (ini rework arc 4; design research/findings/tooling/
// votv-ini-arc4-T8-catalog-impl-DESIGN-2026-07-25.md, /qf 12r "that holds").
//
// ONE file owns the catalog GRAMMAR for both emit and verify:
//   `;; `  prose (descriptions, allowed/range/env-twin lines -- NEVER contains
//          '=' so even a doubly-uncommented prose line stays parse-invisible);
//   `; `   copyable key line `; key=default` (uncomment = strip the leading
//          "; ");
//   bare   `[section]` headers, exactly kSectionOrder, once each (the parser
//          is section-blind (F3) so a bare header can activate nothing, and a
//          whole-file copy yields a headered ini the T3b writer can place
//          into);
//   blank  separators.
// The catalog is REGENERATED every launch and NEVER read back as config; the
// banner says so by NAME (so a line copied into the live ini stays true).
// Deterministic bytes: no timestamp, numeric emission pinned to the C locale
// (a comma LC_NUMERIC process would otherwise drift emit+parse together and
// stay silently green -- the drill's locale canary guards the parse side).

#include "coop/config/config.h"

#include "config_internal.h"
#include "coop/config/config_registry.h"
#include "coop/version.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <locale.h>
#include <string>
#include <vector>

namespace coop::config {

using IniScan = internal::IniScan;

namespace {

std::atomic<ExampleGen> g_status{ExampleGen::NotRun};
std::atomic<int> g_keyCount{0};

// C-locale numeric emission: %.9g under LC_NUMERIC="C" regardless of what the
// process locale does (FLT_DECIMAL_DIG=9 -> any float default round-trips).
_locale_t CLocale() {
    static _locale_t loc = _create_locale(LC_NUMERIC, "C");
    return loc;
}

std::string FormatFloat(float v) {
    char buf[48];
    _snprintf_s_l(buf, sizeof(buf), _TRUNCATE, "%.9g", CLocale(), static_cast<double>(v));
    return buf;
}

std::string FormatDouble(double v) {
    char buf[48];
    _snprintf_s_l(buf, sizeof(buf), _TRUNCATE, "%.9g", CLocale(), v);
    return buf;
}

// Word-wrap `text` into `;; ` prose lines of <= ~100 cols.
void EmitProse(std::vector<std::string>& out, const std::string& text, const char* indent = "") {
    const size_t cap = 100;
    std::string line = std::string(";; ") + indent;
    const size_t prefixLen = line.size();
    size_t pos = 0;
    while (pos < text.size()) {
        size_t sp = text.find(' ', pos);
        const std::string word =
            text.substr(pos, (sp == std::string::npos ? text.size() : sp) - pos);
        if (line.size() > prefixLen && line.size() + 1 + word.size() > cap) {
            out.push_back(line + "\n");
            line = std::string(";; ") + indent;
        }
        if (line.size() > prefixLen) line += " ";
        line += word;
        pos = (sp == std::string::npos) ? text.size() : sp + 1;
    }
    if (line.size() > prefixLen) out.push_back(line + "\n");
}

std::string DefaultValueOf(const config_registry::Row& r) {
    using config_registry::Kind;
    char buf[48];
    switch (r.kind) {
        case Kind::Flag:  return r.defB ? "1" : "0";
        case Kind::Int:
            std::snprintf(buf, sizeof(buf), "%ld", r.defI);
            return buf;
        case Kind::Float: return FormatFloat(r.defF);
        case Kind::Enum:
        case Kind::String: return r.defS ? r.defS : "";
        case Kind::Identity: return "";  // minted -- the copyable line stays valueless
    }
    return "";
}

// " | "-spaced view of a '|'-joined token list (prose readability).
std::string SpacedTokens(const char* tokens) {
    std::string out;
    for (const char* p = tokens; *p; ++p) {
        if (*p == '|') out += " | ";
        else out.push_back(*p);
    }
    return out;
}

std::vector<std::string> BuildExampleLines() {
    using config_registry::Kind;
    std::vector<std::string> out;
    // Banner: SELF-REFERENTIAL BY NAME (a copied line stays true in the live
    // ini). NO '=' in any prose line (parse-invisibility if force-uncommented).
    EmitProse(out, std::string("multivoid.ini.example -- the generated settings catalog for ") +
                       coop::version::kDisplayLabel + ".");
    EmitProse(out, "multivoid.ini.example is regenerated every launch and never read by the mod; "
                   "settings belong in multivoid.ini (same folder). To use a setting: remove the "
                   "leading semicolon-space from its line and move it into multivoid.ini under "
                   "the same section. Flags take 1 or 0.");
    EmitProse(out, "Most keys have a VOTVCOOP env twin that OVERRIDES the ini value; other "
                   "VOTVCOOP env variables are dev/test harness switches, not user settings.");
    size_t count = 0;
    config_registry::Rows(count);
    const config_registry::Row* rows = config_registry::Rows(count);
    for (size_t s = 0; s < config_registry::kSectionCount; ++s) {
        const char* sec = config_registry::kSectionOrder[s];
        out.push_back("\n");
        out.push_back(std::string("[") + sec + "]\n");
        for (size_t i = 0; i < count; ++i) {
            const config_registry::Row& r = rows[i];
            if (std::strcmp(r.section, sec) != 0) continue;
            out.push_back("\n");
            EmitProse(out, r.desc);
            if (r.kind == Kind::Enum && r.tokens)
                EmitProse(out, "allowed: " + SpacedTokens(r.tokens), "  ");
            if (r.kind == Kind::Int || r.kind == Kind::Float)
                EmitProse(out,
                          "range: [" +
                              (r.kind == Kind::Int ? std::to_string(static_cast<long>(r.lo))
                                                   : FormatDouble(r.lo)) +
                              ", " +
                              (r.kind == Kind::Int ? std::to_string(static_cast<long>(r.hi))
                                                   : FormatDouble(r.hi)) +
                              "]",
                          "  ");
            if (r.gatedBy)
                EmitProse(out, std::string("read only when ") + r.gatedBy + " is 1", "  ");
            if (r.envVar)
                EmitProse(out, std::string("env twin: ") + r.envVar + " (env overrides the ini)",
                          "  ");
            out.push_back(std::string("; ") + r.key + "=" + DefaultValueOf(r) + "\n");
        }
    }
    return out;
}

// Whole-file read for the compare-first tri-state. Returns the scan verdict;
// `bytes` valid only on Ok.
IniScan ReadWholeFile(const std::wstring& path, std::string& bytes) {
    FILE* f = nullptr;
    const errno_t rc = _wfopen_s(&f, path.c_str(), L"rb");
    if (rc != 0 || !f) return rc == ENOENT ? IniScan::Absent : IniScan::Unreadable;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.append(buf, n);
    const bool err = std::ferror(f) != 0;
    std::fclose(f);
    return err ? IniScan::Unreadable : IniScan::Ok;
}

}  // namespace

void GenerateExampleCatalog() {
    const std::wstring dir = ue_wrap::paths::ExeDir();
    if (dir.empty()) return;
    const std::wstring path = dir + L"\\multivoid.ini.example";
    const std::vector<std::string> lines = BuildExampleLines();
    int keys = 0;
    for (const auto& l : lines)
        if (l.rfind("; ", 0) == 0) ++keys;
    g_keyCount.store(keys, std::memory_order_relaxed);
    std::string fresh;
    for (const auto& l : lines) fresh += l;
    std::string existing;
    const IniScan st = ReadWholeFile(path, existing);
    if (st == IniScan::Unreadable) {
        // Present but unreadable (share lock?): no doomed swap attempt, and the
        // status makes the drill fail loudly this boot (stale-green closed).
        g_status.store(ExampleGen::SkippedUnreadable, std::memory_order_relaxed);
        UE_LOGW("config: multivoid.ini.example UNREADABLE -- catalog regen skipped (non-fatal)");
        return;
    }
    if (st == IniScan::Ok && existing == fresh) {
        g_status.store(ExampleGen::UpToDate, std::memory_order_relaxed);
        UE_LOGI("config: settings catalog up-to-date (multivoid.ini.example, %d keys)", keys);
        return;
    }
    if (!internal::AtomicWriteAllLines(path, lines, "ExampleCatalog")) {
        g_status.store(ExampleGen::FailedWrite, std::memory_order_relaxed);
        UE_LOGW("config: multivoid.ini.example write FAILED (disk/perms?) -- non-fatal");
        return;
    }
    g_status.store(ExampleGen::Regenerated, std::memory_order_relaxed);
    UE_LOGI("config: settings catalog regenerated (multivoid.ini.example, %d keys)", keys);
}

ExampleGen ExampleGenStatus(int* keyCountOut) {
    if (keyCountOut) *keyCountOut = g_keyCount.load(std::memory_order_relaxed);
    return g_status.load(std::memory_order_relaxed);
}

// ---- the arc-4 drill's verifier (six detectors + the round-trip) ------------

int SelftestExampleVerify(const std::wstring& examplePath, const std::wstring& scratchPath) {
    using config_registry::Kind;
    int fail = 0;
    auto failLog = [&](const std::string& what) {
        UE_LOGW("config-selftest: catalog FAIL %s", what.c_str());
        ++fail;
    };
    std::vector<std::string> lines;
    const IniScan st = internal::ScanIniFile(
        examplePath, [&](const std::string& l) { lines.push_back(l); });
    if (st != IniScan::Ok) {
        failLog("example file unreadable/absent (scan=" + std::to_string(static_cast<int>(st)) + ")");
        return fail;
    }
    size_t count = 0;
    const config_registry::Row* rows = config_registry::Rows(count);
    std::vector<int> seen(count, 0);
    std::vector<std::string> stripped;  // the uncommented copy (round-trip input)
    std::string curSection;
    size_t headerIdx = 0;
    for (const auto& raw : lines) {
        std::string l = raw;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        // Detector 2: wrap (~100 target, 110 tolerance).
        if (l.size() > 110) failLog("over-long line: " + l.substr(0, 60) + "...");
        if (l.empty()) continue;
        if (l.rfind(";;", 0) == 0) continue;  // prose
        if (l.rfind("; ", 0) == 0) {
            const std::string body = l.substr(2);
            // Detector 4: env-only pattern -- no VOTVCOOP_* in copyable shape.
            if (body.find("VOTVCOOP_") != std::string::npos) {
                failLog("env var in copyable form: " + body);
                continue;
            }
            std::string k, v;
            if (!internal::ParseIniKeyValue(body, k, v)) {
                failLog("copyable line is not key=value: " + body);
                continue;
            }
            // Detector 5: orphan key.
            const config_registry::Row* row = config_registry::FindRow(k.c_str());
            if (!row) {
                failLog("orphan key (not a registry row): " + k);
                continue;
            }
            const size_t idx = static_cast<size_t>(row - rows);
            seen[idx]++;
            // Detector 6: section placement under the CURRENT bare header.
            if (curSection != row->section)
                failLog("key '" + k + "' under section [" + curSection + "], row says [" +
                        row->section + "]");
            if (row->kind == Kind::Identity && !v.empty())
                failLog("identity row prints a value: " + k);
            stripped.push_back(body + "\n");
            continue;
        }
        if (l.front() == '[' && l.back() == ']') {
            const std::string name = l.substr(1, l.size() - 2);
            if (headerIdx >= config_registry::kSectionCount ||
                name != config_registry::kSectionOrder[headerIdx])
                failLog("section header out of order/unknown: [" + name + "]");
            else
                ++headerIdx;
            curSection = name;
            continue;
        }
        failLog("line is neither prose, copyable, header nor blank: " + l.substr(0, 60));
    }
    if (headerIdx != config_registry::kSectionCount)
        failLog("not all section headers present (" + std::to_string(headerIdx) + "/" +
                std::to_string(config_registry::kSectionCount) + ")");
    // Detector 3: TRI-directional exactly-once (presence half; dup half above
    // via the counter; orphan half = detector 5).
    for (size_t i = 0; i < count; ++i) {
        if (seen[i] == 0) failLog(std::string("registry key missing from the catalog: ") + rows[i].key);
        if (seen[i] > 1)
            failLog(std::string("registry key appears ") + std::to_string(seen[i]) + "x: " +
                    rows[i].key);
    }
    // Round-trip: the stripped copy through the ONE lexer + the product cores.
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, scratchPath.c_str(), L"wb") == 0 && f) {
            for (const auto& l : stripped) std::fwrite(l.data(), 1, l.size(), f);
            std::fclose(f);
        } else {
            failLog("cannot write the round-trip scratch copy");
            return fail;
        }
        for (size_t i = 0; i < count; ++i) {
            const config_registry::Row& r = rows[i];
            if (r.kind == Kind::Identity) continue;  // no default to equal
            IniScan rs = IniScan::Ok;
            static const char* kAbsent = "\x01<absent>";
            const std::string rawv =
                internal::ReadIniValueAtPath(scratchPath, r.key, kAbsent, &rs);
            const bool found = (rawv != kAbsent);
            if (!found) {
                failLog(std::string("round-trip: key not FOUND in the stripped copy: ") + r.key);
                continue;
            }
            bool ok = true;
            switch (r.kind) {
                case Kind::Flag:  ok = internal::FlagFromRaw(&r, true, rawv) == r.defB; break;
                case Kind::Int:   ok = internal::IntFromRaw(&r, true, rawv) == r.defI; break;
                case Kind::Float: ok = internal::FloatFromRaw(&r, true, rawv) == r.defF; break;
                case Kind::Enum:
                case Kind::String: {
                    const std::string want = r.defS ? r.defS : "";
                    const std::string got = (r.kind == Kind::Enum)
                                                ? internal::EnumFromRaw(&r, true, rawv)
                                                : rawv;
                    ok = got == want;
                    break;
                }
                default: break;
            }
            if (!ok) failLog(std::string("round-trip: value differs from the row default: ") + r.key);
        }
        ::DeleteFileW(scratchPath.c_str());
    }
    return fail;
}

}  // namespace coop::config
