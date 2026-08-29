// coop/config/config_review.cpp -- see config_review.h.

#include "coop/config/config_review.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace coop::config_review {
namespace {

std::mutex g_mu;
std::vector<Row> g_rows;
std::atomic<bool> g_hasRows{false};
std::atomic<bool> g_dismissed{false};

std::string LowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Trim + strip a whitespace-preceded inline comment for DISPLAY (mirrors the
// string reader's narrowing; validation goes through config::ValueValidForKey,
// which applies the reader's own stripping).
std::string DisplayValue(const std::string& v) {
    std::string t = v;
    const size_t b = t.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const size_t e = t.find_last_not_of(" \t\r\n");
    t = t.substr(b, e - b + 1);
    for (size_t i = 0; i < t.size(); ++i)
        if (t[i] == ';' && (i == 0 || t[i - 1] == ' ' || t[i - 1] == '\t')) {
            t = t.substr(0, i);
            const size_t e2 = t.find_last_not_of(" \t");
            t = e2 == std::string::npos ? std::string() : t.substr(0, e2 + 1);
            break;
        }
    return t;
}

void SweepInto(std::vector<Row>& rows) {
    std::vector<std::string> lines;
    const int scan = coop::config::ListLiveIniLines(lines);
    if (scan == 2) {  // Unreadable: the ini layer dropped out for this launch
        Row r;
        r.type = Row::Type::IniUnreadable;
        rows.push_back(r);
    } else if (scan == 0) {
        // Parse key lines; group ci.
        struct Occ {
            std::string firstSpelling;
            std::vector<DupLine> lines;
        };
        std::vector<std::pair<std::string, Occ>> groups;  // keyLower -> occurrences
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string& raw = lines[i];
            const size_t eq = raw.find('=');
            if (eq == std::string::npos) continue;
            std::string key = raw.substr(0, eq);
            const size_t b = key.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) continue;
            const size_t e = key.find_last_not_of(" \t\r\n");
            key = key.substr(b, e - b + 1);
            if (key.empty() || key[0] == ';' || key[0] == '#' || key[0] == '[') continue;
            const std::string val = DisplayValue(raw.substr(eq + 1));
            const std::string lower = LowerAscii(key);
            bool foundGroup = false;
            for (auto& g : groups)
                if (g.first == lower) {
                    g.second.lines.push_back({static_cast<int>(i + 1), val});
                    foundGroup = true;
                    break;
                }
            if (!foundGroup)
                groups.push_back({lower, {key, {{static_cast<int>(i + 1), val}}}});
        }
        for (const auto& g : groups) {
            const std::string& key = g.second.firstSpelling;
            // Unknown key (vs the T2-enum + the composed family): the dead
            // ui.font=fixedsys is the canonical catch (F28).
            if (!coop::config_registry::IsKnownKey(key.c_str())) {
                Row r;
                r.type = Row::Type::Unknown;
                r.key = key;
                r.value = g.second.lines.front().value;
                // A key we RETIRED is not a typo, and telling a player their own
                // identity line looks like one is the wrong sentence. The row type
                // stays Unknown so "Tidy up" still offers to remove it.
                if (const char* note = coop::config_registry::RetiredKeyNote(key.c_str()))
                    r.reason = note;
                rows.push_back(r);
                continue;
            }
            // Differing duplicates: winner = the authoritative (first) line.
            if (g.second.lines.size() > 1) {
                bool differing = false;
                for (size_t j = 1; j < g.second.lines.size(); ++j)
                    if (g.second.lines[j].value != g.second.lines[0].value) {
                        differing = true;
                        break;
                    }
                if (differing) {
                    Row r;
                    r.type = Row::Type::DuplicateDormant;
                    r.key = key;
                    r.dupLines = g.second.lines;
                    // player_guid is gone from this list with its row (v144): the
                    // durable identity is a keypair in multivoid_identity.key now,
                    // so a leftover `player_guid=` line in an existing ini is
                    // simply DEAD -- and the sweep already has the right word for
                    // that, reporting it as an Unknown key one branch above.
                    r.identityKey = _stricmp(key.c_str(), "player_skin") == 0;
                    rows.push_back(r);
                }
            }
            // Typed validation of the authoritative line's value.
            std::string reason;
            if (!coop::config::ValueValidForKey(key.c_str(), g.second.lines[0].value,
                                                &reason)) {
                Row r;
                r.type = Row::Type::Rejected;
                r.key = key;
                r.origin = "multivoid.ini";
                r.value = g.second.lines[0].value;
                r.reason = reason;
                rows.push_back(r);
            }
        }
    }
    // Env twins: a SET env var that fails validation shadows the ini (T6) --
    // report it with its env origin so the operator sees which layer bit them.
    size_t rowCount = 0;
    const coop::config_registry::Row* regRows = coop::config_registry::Rows(rowCount);
    for (size_t i = 0; i < rowCount; ++i) {
        if (!regRows[i].envVar) continue;
        const std::string e = coop::config::ReadEnv(regRows[i].envVar);
        if (e.empty()) continue;
        std::string reason;
        if (!coop::config::ValueValidForKey(regRows[i].key, e, &reason)) {
            Row r;
            r.type = Row::Type::Rejected;
            r.key = regRows[i].key;
            r.origin = std::string(regRows[i].envVar) + " (env)";
            r.value = e;
            r.reason = reason;
            rows.push_back(r);
        }
    }
    if (coop::config::IdentityNotDurable()) {
        Row r;
        r.type = Row::Type::IdentityNotDurable;
        rows.push_back(r);
    }
    if (scan != 2 && coop::config::IniUnreadableSeen()) {
        Row r;
        r.type = Row::Type::IniUnreadable;
        rows.push_back(r);
    }
}

}  // namespace

void RunBootSweep() {
    std::vector<Row> rows;
    SweepInto(rows);
    const size_t count = rows.size();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_rows = std::move(rows);
        g_hasRows.store(count != 0, std::memory_order_release);
    }
    if (count) {
        UE_LOGI("config_review: sweep found %zu row(s) -- the review panel is armed", count);
        // NAME THEM. Arming this panel is not a quiet event: it puts an ImGui surface up,
        // and while any ImGui surface is up the game receives no mouse messages at all
        // (ui/imgui_overlay.h, CaptureOwners) -- so an armed panel silently makes every
        // native menu screen, ours and the game's, unclickable until it is dismissed. A
        // line that says only "armed" leaves the reader of a log with the consequence and
        // none of the cause; on 2026-08-29 that cost a three-day hunt for a close button
        // that was never broken. One line per row, so the next reader knows WHICH finding
        // put it there without opening the game.
        std::lock_guard<std::mutex> lk(g_mu);
        for (const Row& r : g_rows) {
            switch (r.type) {
                case Row::Type::Rejected:
                    UE_LOGI("config_review:   REJECTED %s='%s' from %s (%s)", r.key.c_str(),
                            r.value.c_str(), r.origin.c_str(), r.reason.c_str());
                    break;
                case Row::Type::Unknown:
                    UE_LOGI("config_review:   UNKNOWN key '%s'='%s' -- not in the registry",
                            r.key.c_str(), r.value.c_str());
                    break;
                case Row::Type::DuplicateDormant:
                    UE_LOGI("config_review:   DUPLICATE '%s' on %zu lines; the first wins",
                            r.key.c_str(), r.dupLines.size());
                    break;
                case Row::Type::IdentityNotDurable:
                    UE_LOGI("config_review:   IDENTITY NOT DURABLE -- guid/skin are "
                            "session-only this launch");
                    break;
                case Row::Type::IniUnreadable:
                    UE_LOGI("config_review:   INI UNREADABLE at least once this launch");
                    break;
            }
        }
    }
}

void GetSnapshot(std::vector<Row>& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    out = g_rows;
}

bool PanelOpen() {
    return g_hasRows.load(std::memory_order_acquire) &&
           !g_dismissed.load(std::memory_order_relaxed);
}

void Dismiss() { g_dismissed.store(true, std::memory_order_relaxed); }

bool KeepDuplicateLine(const std::string& key, const std::string& keepValue) {
    const bool ok = coop::config::RemoveDuplicateKeyLines(key.c_str(), keepValue.c_str());
    // Re-sweep on BOTH outcomes: success shows the resolved state; a refusal
    // means the file changed under the panel and the rows are stale.
    RunBootSweep();
    return ok;
}

ReformatOutcome ReformatNow() {
    ReformatOutcome o;
    coop::config::ReformatStats st;
    o.ok = coop::config::ReformatLiveIni(st);
    o.collapsed = st.collapsed;
    o.placed = st.placed;
    o.frozen = st.frozen;
    o.retired = st.retired;
    if (o.ok) RunBootSweep();
    return o;
}

}  // namespace coop::config_review
