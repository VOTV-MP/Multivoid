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
#include <cstring>
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
    // ---- T3 writer drills (design section 5, T3 row) -- on a COPY of the
    // corpus inject file, never the live ini.
    if (!dirA.empty()) {
        const std::wstring dir(dirA.begin(), dirA.end());
        const std::wstring src = dir + L"\\inject.ini";
        const std::wstring wrk = dir + L"\\__write_drill__.ini";
        if (::CopyFileW(src.c_str(), wrk.c_str(), FALSE)) {
            std::vector<std::string> pre;
            cfg::SelftestListLines(src, pre);
            auto countCi = [&](const char* key) {
                std::vector<std::string> ls;
                cfg::SelftestListLines(wrk, ls);
                int n = 0;
                for (const auto& l : ls) {
                    const std::string k = EnumKeyOf(l);
                    if (!k.empty() && _stricmp(k.c_str(), key) == 0) ++n;
                }
                return n;
            };
            auto expect = [&](const char* what, bool ok) {
                if (ok) UE_LOGI("config-selftest: T3 %s ok", what);
                else { UE_LOGW("config-selftest: T3 FAIL %s", what); ++fail; }
            };
            // Drill 1: ci-targeting + normalize. inject.ini has Player_Guid=...
            // ABOVE player_guid=...; writing 'player_guid' must edit the FIRST
            // (case-variant) line in place with canonical spelling -- count
            // stays 2, read-back returns the new value from line 1.
            const char* newGuid = "22222222222222222222222222222222";
            expect("write player_guid returns true",
                   cfg::SelftestWriteValue(wrk, "player_guid", newGuid));
            expect("ci occurrence count still 2", countCi("player_guid") == 2);
            {
                std::vector<std::string> ls;
                cfg::SelftestListLines(wrk, ls);
                bool firstEdited = false;
                for (const auto& l : ls) {
                    const std::string k = EnumKeyOf(l);
                    if (!k.empty() && _stricmp(k.c_str(), "player_guid") == 0) {
                        firstEdited = (k == "player_guid") &&
                                      (l.find(newGuid) != std::string::npos);
                        break;  // the FIRST ci occurrence is the one under test
                    }
                }
                expect("authoritative line edited in place, canonical spelling", firstEdited);
                const cfg::IniSelftestRead rb = cfg::SelftestReadValue(wrk, "player_guid");
                expect("write-then-read same-line equality",
                       rb.found && rb.value == newGuid);
            }
            // Drill 2: N>1 duplicate -- edit-first, never move, count preserved.
            expect("write dup.diff returns true", cfg::SelftestWriteValue(wrk, "dup.diff", "third"));
            expect("dup.diff count still 2", countCi("dup.diff") == 2);
            {
                std::vector<std::string> ls;
                cfg::SelftestListLines(wrk, ls);
                int seen = 0;
                bool firstIsThird = false, secondIsSecond = false;
                for (const auto& l : ls) {
                    const std::string k = EnumKeyOf(l);
                    if (!k.empty() && _stricmp(k.c_str(), "dup.diff") == 0) {
                        ++seen;
                        if (seen == 1) firstIsThird = l.find("third") != std::string::npos;
                        if (seen == 2) secondIsSecond = l.find("second") != std::string::npos;
                    }
                }
                expect("dup.diff first=third second=untouched", firstIsThird && secondIsSecond);
            }
            // Drill 3: untouched long line survives byte-identical (the old
            // 512-chunk writer could splice it; design F31).
            {
                std::vector<std::string> post;
                cfg::SelftestListLines(wrk, post);
                bool longSurvived = false;
                for (const auto& l : pre)
                    if (l.rfind("longline=", 0) == 0)
                        for (const auto& p : post)
                            if (p == l) { longSurvived = true; break; }
                expect("380-char line byte-identical after writes", longSurvived);
            }
            // Drill 4: new key appends at EOF (arc-1 MOVE is inert by design).
            expect("write brand.new returns true", cfg::SelftestWriteValue(wrk, "brand.new", "1"));
            {
                std::vector<std::string> ls;
                cfg::SelftestListLines(wrk, ls);
                expect("brand.new appended as the last line",
                       !ls.empty() && ls.back().rfind("brand.new=1", 0) == 0);
            }
            ::DeleteFileW(wrk.c_str());
        } else {
            UE_LOGW("config-selftest: T3 drills skipped -- inject.ini copy failed");
            ++fail;
        }
    }

    // ---- arc-2 T3b/T4 drills: section placement, validation refusal, the
    // unified vocabulary -- on a synthetic HEADERED file (the skeleton shape).
    if (!dirA.empty()) {
        const std::wstring dir(dirA.begin(), dirA.end());
        const std::wstring wrk = dir + L"\\__headered_drill__.ini";
        {
            FILE* f = nullptr;
            if (_wfopen_s(&f, wrk.c_str(), L"w") == 0 && f) {
                std::fputs("; banner\n\n[net]\nnet.nick=Pelmentor\n\n[player]\n\n[ui]\n"
                           "\n[voice]\n\n[dev]\ndevkeys=1\nNAMEPLATE=0\n", f);
                std::fclose(f);
            }
        }
        auto expect = [&](const char* what, bool ok) {
            if (ok) UE_LOGI("config-selftest: arc2 %s ok", what);
            else { UE_LOGW("config-selftest: arc2 FAIL %s", what); ++fail; }
        };
        auto lineIndexOf = [&](const char* prefix) {
            std::vector<std::string> ls;
            cfg::SelftestListLines(wrk, ls);
            for (size_t i = 0; i < ls.size(); ++i)
                if (ls[i].rfind(prefix, 0) == 0) return static_cast<int>(i);
            return -1;
        };
        // Drill A: NEW key inserts at its section end, not EOF ([ui] block).
        expect("write ui.netstats=1 returns true",
               cfg::SelftestWriteValue(wrk, "ui.netstats", "1"));
        {
            const int ui = lineIndexOf("[ui]");
            const int k = lineIndexOf("ui.netstats=1");
            const int voice = lineIndexOf("[voice]");
            expect("ui.netstats landed inside [ui]", ui >= 0 && k > ui && k < voice);
        }
        // Drill B: the MOVE -- NAMEPLATE=0 sits in [dev] (wrong section, N==1);
        // writing nameplate relocates the rewritten line under [player] with
        // canonical spelling.
        expect("write nameplate=1 returns true",
               cfg::SelftestWriteValue(wrk, "nameplate", "1"));
        {
            const int player = lineIndexOf("[player]");
            const int k = lineIndexOf("nameplate=1");
            const int ui = lineIndexOf("[ui]");
            expect("nameplate MOVED under [player], canonical spelling",
                   player >= 0 && k > player && k < ui && lineIndexOf("NAMEPLATE") < 0);
        }
        // Drill C: T3b refusal -- garbage on a typed row is never persisted.
        {
            std::vector<std::string> before;
            cfg::SelftestListLines(wrk, before);
            expect("write ui.netstats=banana REFUSED",
                   !cfg::SelftestWriteValue(wrk, "ui.netstats", "banana"));
            std::vector<std::string> after;
            cfg::SelftestListLines(wrk, after);
            expect("file unchanged after refusal", before == after);
            expect("write desk_diag_ms=99999999 (out of range) REFUSED",
                   !cfg::SelftestWriteValue(wrk, "desk_diag_ms", "99999999"));
        }
        // Drill D: the unified vocabulary + occurrence rule on the flag layer.
        {
            FILE* f = nullptr;
            if (_wfopen_s(&f, wrk.c_str(), L"w") == 0 && f) {
                std::fputs("yes_key=yes\non_key=on\nno_key=no\nempty_key=\ngarbage_key=2\n"
                           "MIXED_case=1\ndup_gav=banana\ndup_gav=1\n", f);
                std::fclose(f);
            }
            expect("=yes -> true", cfg::SelftestFlagTriState(wrk, "yes_key") == 1);
            expect("=on -> true", cfg::SelftestFlagTriState(wrk, "on_key") == 1);
            expect("=no -> false", cfg::SelftestFlagTriState(wrk, "no_key") == -1);
            expect("present-but-empty -> garbage(0)",
                   cfg::SelftestFlagTriState(wrk, "empty_key") == 0);
            expect("=2 -> garbage(0)", cfg::SelftestFlagTriState(wrk, "garbage_key") == 0);
            expect("wrong-case key visible to the flag layer (ci occurrence)",
                   cfg::SelftestFlagTriState(wrk, "mixed_case") == 1);
            const cfg::IniSelftestRead cs = cfg::SelftestReadValue(wrk, "mixed_case");
            expect("wrong-case key visible to the string layer too",
                   cs.found && cs.value == "1");
            expect("garbage-above-valid dup: authoritative line wins -> garbage(0)",
                   cfg::SelftestFlagTriState(wrk, "dup_gav") == 0);
        }
        ::DeleteFileW(wrk.c_str());
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
