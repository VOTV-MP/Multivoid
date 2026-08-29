// coop/config/config_ini_write.cpp -- the guarded multivoid.ini MUTATION
// engine: the T1 skeleton seeder, the T3/T3b single-key writer, the T1b
// owner reformat and the keep-line dedup. Extracted from config.cpp
// (ini rework arc 2 C6 -- the file crossed the 800-LOC soft cap; reading and
// mutating the ini are two concepts). Shares the reader core's primitives
// via config_internal.h; every public entry holds the one ini mutex.
//
// DESTRUCTION GUARDS heritage (born 2026-07-02: the HOST's ini lost its whole
// head after a locked-file write rebuilt it from an empty line list): never
// rebuild from a file that exists but cannot be read CLEANLY (lock OR
// mid-stream error, tri-state F37/F38); all rebuilds go .new -> checked
// writes -> atomic MoveFileExW.

#include "coop/config/config.h"

#include "config_internal.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace coop::config {

using IniScan = internal::IniScan;

namespace {

// Is `line` the section header `[name]` (edge-trimmed, ci)? Sections are
// decorative to the PARSER (F3) but drive the T3 write PLACEMENT in a file
// that carries our headers (a fresh skeleton, or one the owner reformatted).
bool IsSectionHeader(const std::string& line, std::string& nameOut) {
    const std::string t = internal::TrimEdgesStr(line);
    if (t.size() < 2 || t.front() != '[' || t.back() != ']') return false;
    nameOut = t.substr(1, t.size() - 2);
    return true;
}

// The checked ".new then atomic swap" tail shared by every file rebuild
// (single-key write, T1b reformat, keep-line dedup). Every write is checked
// BEFORE the swap: a disk-full .new must never replace the good ini (the one
// data-loss path this exists to close -- audit 2026-07-02).
bool AtomicWriteLines(const std::wstring& path, const std::vector<std::string>& lines,
                      const char* what) {
    const std::wstring tmp = path + L".new";
    FILE* f = nullptr;
    // BINARY mode (arc-4 audit): the primitive writes EXACTLY the bytes given.
    // Text mode silently translated every '\n' to CRLF on disk, which made the
    // catalog's byte compare-first ("identical -> skip") permanently false --
    // every boot re-swapped and logged "regenerated". Enumerated consequence:
    // ini rebuilds now emit LF endings (the lexer reads both; the text-mode
    // scan already normalized CRLF away, so scanned content is unchanged).
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) {
        UE_LOGW("config: %s could not open multivoid.ini.new for write", what);
        return false;
    }
    bool wrote = true;
    for (const auto& l : lines)
        if (std::fputs(l.c_str(), f) == EOF) { wrote = false; break; }
    if (std::ferror(f)) wrote = false;
    if (std::fclose(f) != 0) wrote = false;
    if (!wrote) {
        ::DeleteFileW(tmp.c_str());
        UE_LOGW("config: %s writing multivoid.ini.new FAILED (disk?) -- ini left unchanged",
                what);
        return false;
    }
    if (!::MoveFileExW(tmp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        UE_LOGW("config: %s atomic swap failed (err=%lu) -- ini left unchanged, "
                "multivoid.ini.new kept", what, ::GetLastError());
        return false;
    }
    return true;
}

// The ini section a key belongs to, for write placement and the reformat: a
// literal row's section, or "ui" for the composed ui.font.<role> family
// (audit IMP-3: the writer and the reformat MUST share this resolution --
// FindRow alone is blind to composed keys, so a font write on a headered file
// fell back to EOF-append while the reformat placed the same key under [ui]).
// nullptr = unknown key (never placed; today's append behavior).
const char* SectionForKey(const char* key) {
    if (const config_registry::Row* row = config_registry::FindRow(key))
        return row->section;
    if (config_registry::IsKnownKey(key)) return "ui";  // composed = fonts
    return nullptr;
}

// Path-parameterized writer core (no lock -- the public wrapper holds it; the
// selftest drives COPIES of corpus files, never the live ini).
bool WriteIniValueAt(const std::wstring& path, const char* key, const char* value) {
    // Scrub CR/LF from the value (an embedded newline -- e.g. pasted into a text field --
    // would split the "key=value" line and corrupt the NEXT key on read-back), then
    // edge-trim. Interior spaces are part of the value (device names) and round-trip
    // verbatim through ReadIniValue's parse.
    std::string safe;
    for (const char* p = value; *p; ++p)
        if (*p != '\n' && *p != '\r') safe.push_back(*p);
    safe = internal::TrimEdgesStr(safe);
    const char* wantSec = SectionForKey(key);
    if (!ValueValidForKey(key, safe, nullptr)) {
        UE_LOGW("config: WriteIniValue('%s'='%s') REFUSED -- the value would be rejected "
                "on read (registry kind/range/tokens); not persisting garbage (T3b)",
                key, safe.c_str());
        return false;
    }
    const std::string newLine = std::string(key) + "=" + safe + "\n";
    // Read existing lines, replacing the key's line IN PLACE if present.
    //
    // TARGETING = the unified occurrence rule (T3): the authoritative line is
    // the FIRST case-INSENSITIVE key occurrence, edited in place with the
    // canonical spelling (safe: zero ci collisions between distinct keys,
    // F40). The old case-sensitive writer MISSED `Enabled=1` when writing
    // `enabled`, appended a second occurrence, and the two readers then
    // disagreed from one write. At N>1 only the authoritative line is edited
    // -- moving past a duplicate would hand victory to the un-written line.
    // All other bytes verbatim; the rewritten line's inline comment is
    // deleted (today's behavior, F36 -- it described the old value).
    //
    // PLACEMENT (T3b, arc 2): a MOVE exists only in a file that CARRIES the
    // key's registry section header; headerless files keep the old behavior
    // (append at EOF, no relocation).
    std::vector<std::string> lines;
    bool found = false;
    int foundIdx = -1;            // index of the rewritten authoritative line
    int occurrences = 0;          // ci occurrence count of `key`
    bool foundInSection = false;  // authoritative line already under its header
    int sectionEndIdx = -1;       // last content line of the key's section (-1 = no header)
    IniScan st = IniScan::Absent;
    for (int attempt = 0; attempt < 5; ++attempt) {  // transient sharing locks
        lines.clear();
        found = false;
        foundIdx = -1;
        occurrences = 0;
        foundInSection = false;
        sectionEndIdx = -1;
        std::string curSection;
        bool inWantSection = false;
        st = internal::ScanIniFile(path, [&](const std::string& s) {
            std::string hdr;
            if (IsSectionHeader(s, hdr)) {
                curSection = hdr;
                inWantSection = wantSec && _stricmp(hdr.c_str(), wantSec) == 0;
                if (inWantSection) sectionEndIdx = static_cast<int>(lines.size());
            } else if (inWantSection && !internal::TrimEdgesStr(s).empty()) {
                sectionEndIdx = static_cast<int>(lines.size());
            }
            std::string k, v;
            if (internal::ParseIniKeyValue(s, k, v) && _stricmp(k.c_str(), key) == 0) {
                ++occurrences;
                if (!found) {
                    found = true;
                    foundIdx = static_cast<int>(lines.size());
                    foundInSection = inWantSection;
                    lines.push_back(newLine);
                    return;
                }
            }
            lines.push_back(s);
        });
        if (st != IniScan::Unreadable) break;
        ::Sleep(20);
    }
    if (st == IniScan::Unreadable) {
        // Exists-but-locked, or a mid-stream read error: either way the
        // collected line list is not the whole file -- rebuilding from it is
        // the 2026-07-02 loss shape. Refuse.
        UE_LOGW("config: WriteIniValue('%s') SKIPPED -- multivoid.ini locked or failing "
                "mid-read; refusing to rebuild the file from a partial view", key);
        return false;
    }
    // Only the file's LAST line can lack a trailing newline; EVERY insertion
    // below lands after an existing line, so normalize once HERE -- an insert
    // after a newline-less final line would splice two lines into one
    // ("[dev]devkeys=1"), breaking both on the next parse (audit CRIT-1; the
    // old fix lived only in the EOF-append branch and missed the section
    // inserts). Parse-neutral: '\n' at EOF changes no verdict.
    if (!lines.empty() && !lines.back().empty() && lines.back().back() != '\n')
        lines.back() += "\n";
    if (found) {
        // The MOVE: only at N==1 and only when the line sits OUTSIDE its
        // section in a headered file (e.g. pasted at EOF). The rewritten line
        // relocates to the END of its section block.
        if (occurrences == 1 && sectionEndIdx >= 0 && !foundInSection) {
            const std::string moved = lines[static_cast<size_t>(foundIdx)];
            lines.erase(lines.begin() + foundIdx);
            int ins = sectionEndIdx;
            if (foundIdx <= sectionEndIdx) --ins;  // erase shifted the target up
            lines.insert(lines.begin() + (ins + 1), moved);
        }
    } else if (sectionEndIdx >= 0) {
        // New key in a headered file: insert at the end of its section block
        // instead of the EOF append.
        lines.insert(lines.begin() + (sectionEndIdx + 1), newLine);
    } else {
        lines.push_back(newLine);  // headerless/unknown key: today's EOF append
    }
    if (!AtomicWriteLines(path, lines, "WriteIniValue")) return false;
    UE_LOGI("config: persisted %s=%s", key, safe.c_str());
    return true;
}

}  // namespace

bool EnsureIniSkeleton() {
    std::lock_guard<std::mutex> lk(internal::IniMutex());
    const std::wstring path = internal::LiveIniPath();
    // Seed ONLY on authoritative ABSENT (ENOENT). An existing file -- readable
    // or not -- is never touched: seeding over a locked-but-present ini is the
    // same destruction class the F7 writer guards close (design T1/F37).
    {
        FILE* probe = nullptr;
        const errno_t rc = _wfopen_s(&probe, path.c_str(), L"r");
        if (rc == 0 && probe) { std::fclose(probe); return false; }  // exists
        if (rc != ENOENT) {
            UE_LOGW("config: skeleton seeder skipped -- multivoid.ini unreadable (errno=%d), "
                    "not absent; refusing to seed over it", static_cast<int>(rc));
            return false;
        }
    }
    // The skeleton: ordered section headers from the registry ([net] first,
    // [dev] last) and ZERO default values (F4: a seeded key silently OVERRIDES
    // the code default) -- with exactly ONE user-ruled exception per the
    // seeded-active column: a visible, deliberately-editable net.nick line
    // (the joke is meant to be SEEN and replaced; design T1 "seeded-active").
    std::string content = "; multivoid.ini -- Multivoid configuration. Created on first launch.\n";
    size_t rowCount = 0;
    const config_registry::Row* rows = config_registry::Rows(rowCount);
    for (size_t i = 0; i < config_registry::kSectionCount; ++i) {
        const char* sec = config_registry::kSectionOrder[i];
        content += "\n[";
        content += sec;
        content += "]\n";
        for (size_t r = 0; r < rowCount; ++r)
            if (rows[r].seededActive && _stricmp(rows[r].section, sec) == 0)
                content += std::string(rows[r].key) + "=" +
                           config_registry::kMyNameDefault + "\n";
    }
    // Atomic create: .new then MoveFileExW WITHOUT REPLACE_EXISTING -- if the
    // file appeared concurrently the seeder loses the race gracefully.
    const std::wstring tmp = path + L".new";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"w") != 0 || !f) {
        UE_LOGW("config: skeleton seeder could not open multivoid.ini.new for write");
        return false;
    }
    bool wrote = std::fputs(content.c_str(), f) != EOF;
    if (std::ferror(f)) wrote = false;
    if (std::fclose(f) != 0) wrote = false;
    if (!wrote) {
        ::DeleteFileW(tmp.c_str());
        UE_LOGW("config: skeleton seeder write FAILED (disk?) -- no ini created");
        return false;
    }
    if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileW(tmp.c_str());
        UE_LOGW("config: skeleton seeder lost the create race (err=%lu) -- existing ini kept",
                ::GetLastError());
        return false;
    }
    UE_LOGI("config: seeded fresh multivoid.ini skeleton ([net] first, net.nick=%s, [dev] last)",
            config_registry::kMyNameDefault);
    return true;
}

// The internal seam for the T8 catalog generator (arc 4): the SAME atomic-swap
// primitive every ini rebuild uses, path-parameterized (the .example is never
// the live ini; no lock needed -- single writer at boot).
namespace internal {
bool AtomicWriteAllLines(const std::wstring& path, const std::vector<std::string>& lines,
                         const char* what) {
    return AtomicWriteLines(path, lines, what);
}
}  // namespace internal

// The one locked live-ini write behind every typed overload (C3b): the handle
// carries the canonical key; everything below it is the string engine.
static bool WriteIniValueRow(const config_registry::Row* row, const char* value) {
    std::lock_guard<std::mutex> lk(internal::IniMutex());
    return WriteIniValueAt(internal::LiveIniPath(), row->key, value);
}

bool WriteIniValue(const config_registry::FlagRow& row, const char* value) {
    return WriteIniValueRow(row.row, value);
}
bool WriteIniValue(const config_registry::IntRow& row, const char* value) {
    return WriteIniValueRow(row.row, value);
}
bool WriteIniValue(const config_registry::FloatRow& row, const char* value) {
    return WriteIniValueRow(row.row, value);
}
bool WriteIniValue(const config_registry::EnumRow& row, const char* value) {
    return WriteIniValueRow(row.row, value);
}
bool WriteIniValue(const config_registry::StringRow& row, const char* value) {
    return WriteIniValueRow(row.row, value);
}
bool WriteIniValue(const config_registry::IdentityRow& row, const char* value) {
    return WriteIniValueRow(row.row, value);
}

// Correlates by VALUE, never by line number (audit CRIT-2): the panel's
// snapshot ages while it sits on screen, and an unrelated write elsewhere
// shifts every line index -- a stale index could delete BOTH copies of a
// duplicate identity key. The kept line = the FIRST ci-key line whose
// comment-stripped value equals what the user clicked; if NO current line
// carries that value the file changed underneath -> refuse, the caller
// re-sweeps and the panel shows fresh state.
static bool RemoveDuplicateKeyLinesAt(const std::wstring& path, const char* key,
                                      const char* keepValue) {
    std::vector<std::string> lines;
    if (internal::ScanIniFile(path, [&](const std::string& l) { lines.push_back(l); }) !=
        IniScan::Ok) {
        UE_LOGW("config: keep-line for '%s' SKIPPED -- ini unreadable; nothing deleted", key);
        return false;
    }
    auto displayValue = [](const std::string& v) {
        return internal::StripInlineCommentStr(internal::TrimEdgesStr(v), true);
    };
    // Pass 1: does the clicked value still exist for this key?
    bool valuePresent = false;
    for (const auto& l : lines) {
        std::string k, v;
        if (internal::ParseIniKeyValue(l, k, v) && _stricmp(k.c_str(), key) == 0 &&
            displayValue(v) == keepValue) {
            valuePresent = true;
            break;
        }
    }
    if (!valuePresent) {
        UE_LOGW("config: keep-line for '%s' REFUSED -- no current line carries the chosen "
                "value (the file changed since the report); re-sweeping instead", key);
        return false;
    }
    // Pass 2: keep the FIRST line with the chosen value; drop every other
    // ci-occurrence of the key.
    std::vector<std::string> out;
    out.reserve(lines.size());
    int removed = 0;
    bool kept = false;
    for (const auto& l : lines) {
        std::string k, v;
        if (internal::ParseIniKeyValue(l, k, v) && _stricmp(k.c_str(), key) == 0) {
            if (!kept && displayValue(v) == keepValue) {
                kept = true;
                out.push_back(l);
            } else {
                ++removed;
            }
            continue;
        }
        out.push_back(l);
    }
    if (removed == 0) return false;  // nothing to delete (already resolved)
    if (!AtomicWriteLines(path, out, "keep-line dedup")) return false;
    UE_LOGI("config: duplicate resolution for '%s' -- kept value '%s', removed %d line(s) "
            "(owner action from the config review)", key, keepValue, removed);
    return true;
}

bool RemoveDuplicateKeyLines(const char* key, const char* keepValue) {
    std::lock_guard<std::mutex> lk(internal::IniMutex());
    return RemoveDuplicateKeyLinesAt(internal::LiveIniPath(), key, keepValue);
}

bool SelftestRemoveDuplicates(const std::wstring& path, const char* key, const char* keepValue) {
    return RemoveDuplicateKeyLinesAt(path, key, keepValue);
}

static bool ReformatIniAt(const std::wstring& path, ReformatStats& stats) {
    std::vector<std::string> lines;
    if (internal::ScanIniFile(path, [&](const std::string& l) { lines.push_back(l); }) !=
        IniScan::Ok) {
        UE_LOGW("config: reformat SKIPPED -- ini unreadable; file untouched");
        return false;
    }
    const size_t n = lines.size();
    // Classify.
    struct Cls {
        bool isKey = false, isHeader = false, isComment = false, isBlank = false;
        std::string keyLower, keySpelling, value;
    };
    std::vector<Cls> cls(n);
    for (size_t i = 0; i < n; ++i) {
        const std::string t = internal::TrimEdgesStr(lines[i]);
        std::string hdr, k, v;
        if (t.empty()) { cls[i].isBlank = true; continue; }
        if (t[0] == ';' || t[0] == '#') { cls[i].isComment = true; continue; }
        if (IsSectionHeader(lines[i], hdr)) { cls[i].isHeader = true; continue; }
        if (internal::ParseIniKeyValue(lines[i], k, v)) {
            cls[i].isKey = true;
            cls[i].keySpelling = k;
            for (char& c : k) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
            cls[i].keyLower = k;
            cls[i].value = internal::TrimEdgesStr(v);
        } else {
            cls[i].isComment = true;  // '='-less junk line: keep verbatim in the residue
        }
    }
    // ci occurrence groups.
    auto buildGroups = [&](std::vector<std::pair<std::string, std::vector<size_t>>>& out2) {
        out2.clear();
        for (size_t i = 0; i < n; ++i) {
            if (!cls[i].isKey) continue;
            bool foundGroup = false;
            for (auto& g : out2)
                if (g.first == cls[i].keyLower) { g.second.push_back(i); foundGroup = true; break; }
            if (!foundGroup) out2.push_back({cls[i].keyLower, {i}});
        }
    };
    std::vector<std::pair<std::string, std::vector<size_t>>> groups;
    buildGroups(groups);
    // RETIRE the review panel's fixable complaints (2026-07-26): an unknown key
    // (every occurrence) and a SINGLE-occurrence known key whose authoritative
    // value fails typed validation become comments -- the data stays readable
    // in the file, the next sweep sees no live line, the complaint resolves.
    // The classifiers are the sweep's own authorities (config_registry::
    // IsKnownKey + ValueValidForKey), never a second opinion. Differing
    // duplicate groups are untouched here (keep-line buttons adjudicate them;
    // commenting their first line would silently flip the winner).
    {
        bool anyRetired = false;
        for (const auto& g : groups) {
            const bool known = config_registry::IsKnownKey(cls[g.second[0]].keySpelling.c_str());
            if (known && g.second.size() > 1) continue;  // dup group: panel buttons own it
            const char* tag = nullptr;
            if (!known) {
                // A key WE retired says so, and says which build. The line stays
                // in the player's file either way; the difference is whether the
                // comment they find next year reads as a typo they made or as a
                // setting that moved. Upgrading a b133 ini is the case this is
                // for -- measured 2026-08-29, exactly two keys changed hands
                // between b133 and b144, and both are in the retired table.
                tag = config_registry::RetiredKeyNote(cls[g.second[0]].keySpelling.c_str())
                          ? "; retired setting (tidy): "
                          : "; unknown key (tidy): ";
            } else {
                std::string reason;
                if (!ValueValidForKey(cls[g.second[0]].keySpelling.c_str(),
                                      cls[g.second[0]].value, &reason))
                    tag = "; invalid value (tidy): ";
            }
            if (!tag) continue;
            for (size_t idx : g.second) {
                lines[idx] = tag + internal::TrimEdgesStr(lines[idx]);
                cls[idx].isKey = false;
                cls[idx].isComment = true;
                ++stats.retired;
                anyRetired = true;
            }
        }
        if (anyRetired) buildGroups(groups);  // the retired lines left the key universe
    }
    // Collapse value-identical duplicates (keep the FIRST line -- behavior-
    // preserving under the unified occurrence rule); differing groups FREEZE.
    std::vector<char> deleted(n, 0);
    for (auto& g : groups) {
        if (g.second.size() < 2) continue;
        bool identical = true;
        for (size_t j = 1; j < g.second.size(); ++j)
            if (cls[g.second[j]].value != cls[g.second[0]].value) { identical = false; break; }
        if (identical) {
            for (size_t j = 1; j < g.second.size(); ++j) {
                deleted[g.second[j]] = 1;
                ++stats.collapsed;
            }
            g.second.resize(1);
        } else {
            ++stats.frozen;
        }
    }
    // Attach a contiguous comment run directly above a key line to that line
    // (the user's annotation travels with its key). A blank or header breaks
    // the run; the file-leading banner stays a banner by that rule.
    std::vector<int> attachedTo(n, -1);
    {
        std::vector<size_t> pending;
        for (size_t i = 0; i < n; ++i) {
            if (cls[i].isComment) { pending.push_back(i); continue; }
            if (cls[i].isKey && !deleted[i])
                for (size_t c : pending) attachedTo[c] = static_cast<int>(i);
            pending.clear();
        }
    }
    // Emit. A moved/emitted line is normalized to end with '\n' (the original
    // last line may not); all bytes otherwise verbatim.
    auto withNl = [](std::string s) {
        if (s.empty() || s.back() != '\n') s += "\n";
        return s;
    };
    std::vector<char> consumed(n, 0);
    std::vector<std::string> out;
    // Banner: everything before the first header/keyline that isn't an
    // attached comment.
    for (size_t i = 0; i < n; ++i) {
        if (cls[i].isHeader || cls[i].isKey) break;
        if (attachedTo[i] >= 0) break;
        out.push_back(withNl(lines[i]));
        consumed[i] = 1;
    }
    auto emitKeyLine = [&](std::vector<std::string>& dst, size_t i) {
        for (size_t c = 0; c < n; ++c)
            if (attachedTo[c] == static_cast<int>(i) && !consumed[c]) {
                dst.push_back(withNl(lines[c]));
                consumed[c] = 1;
            }
        dst.push_back(withNl(lines[i]));
        consumed[i] = 1;
    };
    for (size_t s = 0; s < config_registry::kSectionCount; ++s) {
        const char* sec = config_registry::kSectionOrder[s];
        out.push_back(std::string("\n[") + sec + "]\n");
        for (const auto& g : groups) {
            if (g.second.size() != 1) continue;  // frozen differing pair: never repositioned
            const size_t i = g.second[0];
            if (consumed[i] || deleted[i]) continue;
            const char* keySec = SectionForKey(g.first.c_str());
            if (!keySec || _stricmp(keySec, sec) != 0) continue;
            emitKeyLine(out, i);
            ++stats.placed;
        }
    }
    // Residue: whatever remains, in ORIGINAL order -- frozen differing
    // duplicates ("never repositioned, never adjudicated"; relative order of
    // an un-collapsed pair is invariant), unknown keys, loose mid-file
    // comments. Old section header lines and collapsed duplicates are dropped
    // (the canonical headers above replace them); blank separators of moved
    // content are dropped too.
    std::vector<std::string> residue;
    for (size_t i = 0; i < n; ++i) {
        if (consumed[i] || deleted[i] || cls[i].isHeader || cls[i].isBlank) continue;
        if (cls[i].isComment && attachedTo[i] >= 0) continue;  // travels with its key line
        if (cls[i].isKey) { emitKeyLine(residue, i); continue; }
        residue.push_back(withNl(lines[i]));
        consumed[i] = 1;
    }
    if (!residue.empty()) {
        out.push_back("\n; --- kept as-is by the reformat (loose lines) ---\n");
        for (auto& r : residue) out.push_back(std::move(r));
    }
    if (!AtomicWriteLines(path, out, "reformat")) return false;
    UE_LOGI("config: reformat done -- %d duplicate line(s) collapsed, %d key(s) placed "
            "under their sections, %d unknown/invalid line(s) retired to comments, "
            "%d differing-duplicate key(s) left for the review panel",
            stats.collapsed, stats.placed, stats.retired, stats.frozen);
    return true;
}

bool ReformatLiveIni(ReformatStats& stats) {
    std::lock_guard<std::mutex> lk(internal::IniMutex());
    return ReformatIniAt(internal::LiveIniPath(), stats);
}

bool SelftestReformat(const std::wstring& path, ReformatStats& stats) {
    return ReformatIniAt(path, stats);
}

bool SelftestWriteValue(const std::wstring& path, const char* key, const char* value) {
    return WriteIniValueAt(path, key, value);
}

}  // namespace coop::config
