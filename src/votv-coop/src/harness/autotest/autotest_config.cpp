// harness/autotest_config.cpp -- the config-corpus selftest (ini rework arc 1).
//
// Runs the REAL C++ lexer (coop/config selftest seams) over a corpus directory
// of ini files and prints one verdict line per (file, key) for BOTH value
// layers -- the AFTER half of the design's T4 instrument (the BEFORE half is
// simulated offline by the instrument script; research/findings/tooling/
// votv-ini-config-registry-DESIGN-2026-07-24.md section 5). Also proves the
// tri-state branches: ENOENT -> Absent, injected mid-stream failure ->
// Unreadable (never a clean end that would read as ABSENT downstream).
//
// SOLO, role-agnostic, no session and no settle needed (pure file ops).
// Gated by env VOTVCOOP_RUN_CONFIG_SELFTEST="1"; corpus dir from
// VOTVCOOP_CONFIG_CORPUS_DIR (skipped with a log line when unset).

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace cfg = coop::config;

// Instrument scaffolding ONLY: which keys to query. The VERDICTS come from the
// real lexer via the selftest seams; this local split just enumerates distinct
// raw key spellings present in the file (first '=' cut, edge-trimmed).
std::string EnumKeyOf(const std::string& line) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return {};
    std::string k = line.substr(0, eq);
    const size_t b = k.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = k.find_last_not_of(" \t\r\n");
    return k.substr(b, e - b + 1);
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

}  // namespace

void RunConfigSelftest() {
    int fail = 0;
    const std::string dirA = cfg::ReadEnv("VOTVCOOP_CONFIG_CORPUS_DIR");
    if (dirA.empty()) {
        UE_LOGI("config-selftest: VOTVCOOP_CONFIG_CORPUS_DIR unset -- corpus half skipped");
    } else {
        const std::wstring dir(dirA.begin(), dirA.end());
        WIN32_FIND_DATAW fd = {};
        HANDLE h = ::FindFirstFileW((dir + L"\\*.ini").c_str(), &fd);
        int files = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring path = dir + L"\\" + fd.cFileName;
                const std::string fileTag = Narrow(fd.cFileName);
                ++files;
                std::vector<std::string> lines;
                const int scan = cfg::SelftestListLines(path, lines);
                UE_LOGI("config-selftest: file=%s lines=%d scan=%d", fileTag.c_str(),
                        static_cast<int>(lines.size()), scan);
                // Raw line record (the non-key-line multiset check runs on these).
                for (size_t i = 0; i < lines.size(); ++i) {
                    std::string l = lines[i];
                    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                    UE_LOGI("config-selftest: file=%s line[%d]='%s'", fileTag.c_str(),
                            static_cast<int>(i), l.c_str());
                }
                // Distinct raw key spellings (+ lowercase twin when different, so a
                // case-variant line's visibility to each layer is on record).
                std::vector<std::string> keys;
                for (const auto& l : lines) {
                    std::string k = EnumKeyOf(l);
                    if (k.empty()) continue;
                    std::string lo = k;
                    std::transform(lo.begin(), lo.end(), lo.begin(),
                                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
                    for (const auto& cand : {k, lo})
                        if (std::find(keys.begin(), keys.end(), cand) == keys.end())
                            keys.push_back(cand);
                }
                for (const auto& k : keys) {
                    const cfg::IniSelftestRead r = cfg::SelftestReadValue(path, k.c_str());
                    const int flag = cfg::SelftestFlagTriState(path, k.c_str());
                    const std::string strRepr = r.found ? "'" + r.value + "'" : "ABSENT";
                    UE_LOGI("config-selftest: file=%s key='%s' str=%s scan=%d flag=%d",
                            fileTag.c_str(), k.c_str(), strRepr.c_str(), r.scan, flag);
                }
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }
        if (files == 0) {
            UE_LOGW("config-selftest: corpus dir '%s' has no .ini files", dirA.c_str());
            ++fail;
        }
        // ENOENT discrimination: a nonexistent file must scan Absent(1), found=false.
        const cfg::IniSelftestRead nf =
            cfg::SelftestReadValue(dir + L"\\__no_such_file__.ini", "any");
        if (nf.scan != 1 || nf.found) {
            UE_LOGW("config-selftest: FAIL absent-file check (scan=%d found=%d)", nf.scan,
                    nf.found ? 1 : 0);
            ++fail;
        } else {
            UE_LOGI("config-selftest: absent-file check ok (scan=1)");
        }
    }
    // Fault injection (design T4 must-FAIL control): a mid-stream error must
    // yield Unreadable(2) -- with lines already delivered and with none.
    for (const int n : {2, 0}) {
        const int st = cfg::SelftestScanWithFailure(n);
        if (st != 2) {
            UE_LOGW("config-selftest: FAIL fault-injection(after=%d) -> scan=%d (want 2)", n, st);
            ++fail;
        } else {
            UE_LOGI("config-selftest: fault-injection(after=%d) -> Unreadable ok", n);
        }
    }
    UE_LOGI("config-selftest: DONE fail=%d", fail);
}

DWORD WINAPI ConfigSelftestThread(LPVOID) {
    RunConfigSelftest();
    return 0;
}

}  // namespace harness::autotest
