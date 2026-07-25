// coop/config/config_review.h -- the T10 config review: boot-time
// file-vs-schema sweep + the row store behind the review panel.
//
// (ini rework arc 2; design research/findings/tooling/
// votv-ini-config-registry-DESIGN-2026-07-24.md T10.)
//
// The sweep validates the ini FILE (and the set env twins) against the
// registry schema -- statically, off each row's kind -- so even never-read
// gated keys (F11: desk_diag_ms behind desk_diag) get their verdicts. It
// REPORTS, never rewrites; the in-memory default substitution still happens
// at each read site. The panel re-arms every launch while any row lives;
// dismissal is in-memory, session-local. The only writes are the OWNER
// actions (keep-line / reformat), routed through config's guarded file ops.

#pragma once

#include <string>
#include <vector>

namespace coop::config_review {

struct DupLine {
    int lineNo = 0;          // 1-based line number in the current file
    std::string value;
};

struct Row {
    enum class Type {
        Rejected,            // garbage value -> the default is in effect
        Unknown,             // key not in the registry (dead ui.font=..., typos)
        DuplicateDormant,    // N>1 differing values; winner = the first line
        IdentityNotDurable,  // guid/skin is session-only this launch
        IniUnreadable,       // some live-ini access hit UNREADABLE this launch
    };
    Type type = Type::Rejected;
    std::string key;         // Rejected/Unknown/DuplicateDormant
    std::string origin;      // Rejected: "multivoid.ini" or the env var name
    std::string value;       // Rejected/Unknown: the offending raw value
    std::string reason;      // Rejected: why it was rejected
    bool identityKey = false;  // DuplicateDormant on player_guid/player_skin
    std::vector<DupLine> dupLines;  // DuplicateDormant: every line, first = winner
};

// Run the file-vs-schema sweep and (re)fill the row store. Boot thread at
// launch (harness, after the guid/skin mints so the post-mint file state is
// what gets reviewed); re-run by the panel after an owner action.
void RunBootSweep();

// Render-thread access.
void GetSnapshot(std::vector<Row>& out);
bool PanelOpen();       // rows exist AND not dismissed this session
void Dismiss();         // session-local; a fresh launch re-arms by re-sweeping

// Owner actions (panel buttons). Both re-run the sweep on success.
bool KeepDuplicateLine(const std::string& key, int keepLineNo);
struct ReformatOutcome { bool ok = false; int collapsed = 0, placed = 0, frozen = 0; };
ReformatOutcome ReformatNow();

}  // namespace coop::config_review
