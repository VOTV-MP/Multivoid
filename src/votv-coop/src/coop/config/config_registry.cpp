// coop/config/config_registry.cpp -- the declarative per-key row table.
//
// See config_registry.h. The row LIST lives in config_registry_rows.inc (ONE
// list); this TU expands it three times -- index enum, Row table, typed handle
// definitions -- so the artifacts cannot drift. ValidateRows() is the
// permanent constexpr compile gate on the table's own coherence (arc 3).

#include "coop/config/config_registry.h"

#include "coop/net/protocol.h"  // kDefaultPort / kOfficialMasterUrl / kOfficialSignalingUrl
                                // (row defaults ALIAS the one owning constant)

#include <cstring>
#include <string>

namespace coop::config_registry {

namespace detail {
// The only minting authority for typed handles (private-tag ctor; header).
struct RegistryDef {
    static constexpr RegistryCtorKey K() { return {}; }
};
}  // namespace detail

namespace {

constexpr double kNoRange = 0.0;

// The Enum token list for the ui.font.<role> rows: the SAME families as
// kFontFamilyTokens, as one pipe-joined literal (ValidateRows pins each array
// token into this list, so the two spellings cannot drift).
constexpr const char* kFontFamilyTokensJoined = "jetbrains|roboto|cascadia|fixedsys";

// ---- expansion 1: row indices (table positions, .inc order) -----------------
enum RowIndex : size_t {
#define CFG_FLAG(ident, key, section, defB, envVar) RowIndex_##ident,
#define CFG_INT(ident, key, section, defI, lo, hi, envVar) RowIndex_##ident,
#define CFG_FLOAT(ident, key, section, defF, lo, hi, envVar) RowIndex_##ident,
#define CFG_ENUM(ident, key, section, defS, tokens, envVar) RowIndex_##ident,
#define CFG_STRING(ident, key, section, defS, envVar, seeded) RowIndex_##ident,
#define CFG_IDENTITY(ident, key, section) RowIndex_##ident,
#define CFG_FONTROLE(ident, key, suffix, defFam) RowIndex_##ident,
#include "coop/config/config_registry_rows.inc"
#undef CFG_FLAG
#undef CFG_INT
#undef CFG_FLOAT
#undef CFG_ENUM
#undef CFG_STRING
#undef CFG_IDENTITY
#undef CFG_FONTROLE
    kRowCountIndex
};

// ---- expansion 2: the Row table ---------------------------------------------
constexpr Row kRows[] = {
#define CFG_FLAG(ident, key, section, defB, envVar) \
    Row{key, section, Kind::Flag, kNoRange, kNoRange, nullptr, envVar, false, defB, 0, 0.0f, nullptr},
#define CFG_INT(ident, key, section, defI, lo, hi, envVar) \
    Row{key, section, Kind::Int, lo, hi, nullptr, envVar, false, false, static_cast<long>(defI), 0.0f, nullptr},
#define CFG_FLOAT(ident, key, section, defF, lo, hi, envVar) \
    Row{key, section, Kind::Float, lo, hi, nullptr, envVar, false, false, 0, defF, nullptr},
#define CFG_ENUM(ident, key, section, defS, tokens, envVar) \
    Row{key, section, Kind::Enum, kNoRange, kNoRange, tokens, envVar, false, false, 0, 0.0f, defS},
#define CFG_STRING(ident, key, section, defS, envVar, seeded) \
    Row{key, section, Kind::String, kNoRange, kNoRange, nullptr, envVar, seeded, false, 0, 0.0f, defS},
#define CFG_IDENTITY(ident, key, section) \
    Row{key, section, Kind::Identity, kNoRange, kNoRange, nullptr, nullptr, false, false, 0, 0.0f, nullptr},
#define CFG_FONTROLE(ident, key, suffix, defFam) \
    Row{key, "ui", Kind::Enum, kNoRange, kNoRange, kFontFamilyTokensJoined, nullptr, false, false, 0, 0.0f, \
        kFontFamilyTokens[defFam]},
#include "coop/config/config_registry_rows.inc"
#undef CFG_FLAG
#undef CFG_INT
#undef CFG_FLOAT
#undef CFG_ENUM
#undef CFG_STRING
#undef CFG_IDENTITY
#undef CFG_FONTROLE
};
constexpr size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);
static_assert(kRowCount == kRowCountIndex, "index enum and row table drifted");

// ---- the constexpr compile gate (arc 3) -------------------------------------
constexpr bool CEq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == *b;
}
constexpr bool CPrefix(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s != *prefix) return false;
        ++s; ++prefix;
    }
    return true;
}
// Exact (case-sensitive) token membership in a '|'-joined list. Registry
// defaults are written canonical, so cs equality is the right gate here (the
// RUNTIME matcher stays ci -- that is the user-input side).
constexpr bool TokenInList(const char* list, const char* tok) {
    while (*list) {
        const char* t = tok;
        while (*list && *list != '|' && *t && *list == *t) { ++list; ++t; }
        if ((*list == '\0' || *list == '|') && *t == '\0') return true;
        while (*list && *list != '|') ++list;
        if (*list == '|') ++list;
    }
    return false;
}
constexpr bool SectionKnown(const char* sec) {
    for (size_t i = 0; i < kSectionCount; ++i)
        if (CEq(sec, kSectionOrder[i])) return true;
    return false;
}

// Font-role row indices, .inc order (expansion for the coherence check).
constexpr size_t kFontRoleRowIndex[] = {
#define CFG_FLAG(ident, key, section, defB, envVar)
#define CFG_INT(ident, key, section, defI, lo, hi, envVar)
#define CFG_FLOAT(ident, key, section, defF, lo, hi, envVar)
#define CFG_ENUM(ident, key, section, defS, tokens, envVar)
#define CFG_STRING(ident, key, section, defS, envVar, seeded)
#define CFG_IDENTITY(ident, key, section)
#define CFG_FONTROLE(ident, key, suffix, defFam) RowIndex_##ident,
#include "coop/config/config_registry_rows.inc"
#undef CFG_FLAG
#undef CFG_INT
#undef CFG_FLOAT
#undef CFG_ENUM
#undef CFG_STRING
#undef CFG_IDENTITY
#undef CFG_FONTROLE
};
static_assert(sizeof(kFontRoleRowIndex) / sizeof(kFontRoleRowIndex[0]) == kFontRoleCount,
              "font-role rows and kFontRoleKeys must stay in lockstep");

constexpr bool ValidateRows() {
    for (size_t i = 0; i < kRowCount; ++i) {
        const Row& r = kRows[i];
        if (!r.key || !*r.key || !SectionKnown(r.section)) return false;
        switch (r.kind) {
            case Kind::Int:
                if (static_cast<double>(r.defI) < r.lo || static_cast<double>(r.defI) > r.hi)
                    return false;
                break;
            case Kind::Float:
                if (static_cast<double>(r.defF) < r.lo || static_cast<double>(r.defF) > r.hi)
                    return false;
                break;
            case Kind::Enum:
                if (!r.tokens || !r.defS) return false;
                // "" = the unset sentinel, allowed ONLY for the allowlisted keys.
                if (CEq(r.defS, "")) {
                    if (!(CEq(r.key, "net.role") || CEq(r.key, "net.ice"))) return false;
                } else if (!TokenInList(r.tokens, r.defS)) {
                    return false;
                }
                break;
            case Kind::String:
                if (!r.defS) return false;
                break;
            case Kind::Flag:
            case Kind::Identity:
                break;
        }
    }
    // The joined font-token literal and the token array cannot drift.
    for (size_t i = 0; i < kFontFamilyCount; ++i)
        if (!TokenInList(kFontFamilyTokensJoined, kFontFamilyTokens[i])) return false;
    // Font-role coherence: key == "ui.font." + kFontRoleKeys[i] IN ORDER, and
    // the row default token == kFontFamilyTokens[kFontRoleDefaultFamily[i]].
    for (size_t i = 0; i < kFontRoleCount; ++i) {
        const Row& r = kRows[kFontRoleRowIndex[i]];
        if (!CPrefix(r.key, "ui.font.")) return false;
        if (!CEq(r.key + 8, kFontRoleKeys[i])) return false;
        const int fam = kFontRoleDefaultFamily[i];
        if (fam < 0 || static_cast<size_t>(fam) >= kFontFamilyCount) return false;
        if (!CEq(r.defS, kFontFamilyTokens[fam])) return false;
    }
    return true;
}
static_assert(ValidateRows(), "config registry row table is incoherent (default/range/token/font-role)");

}  // namespace

// ---- expansion 3: the typed handle definitions ------------------------------
namespace rows {
#define CFG_FLAG(ident, key, section, defB, envVar) \
    const FlagRow ident{&kRows[RowIndex_##ident], detail::RegistryDef::K()};
#define CFG_INT(ident, key, section, defI, lo, hi, envVar) \
    const IntRow ident{&kRows[RowIndex_##ident], detail::RegistryDef::K()};
#define CFG_FLOAT(ident, key, section, defF, lo, hi, envVar) \
    const FloatRow ident{&kRows[RowIndex_##ident], detail::RegistryDef::K()};
#define CFG_ENUM(ident, key, section, defS, tokens, envVar) \
    const EnumRow ident{&kRows[RowIndex_##ident], detail::RegistryDef::K()};
#define CFG_STRING(ident, key, section, defS, envVar, seeded) \
    const StringRow ident{&kRows[RowIndex_##ident], detail::RegistryDef::K()};
#define CFG_IDENTITY(ident, key, section)
#define CFG_FONTROLE(ident, key, suffix, defFam) \
    const EnumRow ident{&kRows[RowIndex_##ident], detail::RegistryDef::K()};
#include "coop/config/config_registry_rows.inc"
#undef CFG_FLAG
#undef CFG_INT
#undef CFG_FLOAT
#undef CFG_ENUM
#undef CFG_STRING
#undef CFG_IDENTITY
#undef CFG_FONTROLE
}  // namespace rows

namespace {
// The per-role Enum handles in Role order (FontRoleRow's backing).
const EnumRow* const kFontRoleRows[] = {
#define CFG_FLAG(ident, key, section, defB, envVar)
#define CFG_INT(ident, key, section, defI, lo, hi, envVar)
#define CFG_FLOAT(ident, key, section, defF, lo, hi, envVar)
#define CFG_ENUM(ident, key, section, defS, tokens, envVar)
#define CFG_STRING(ident, key, section, defS, envVar, seeded)
#define CFG_IDENTITY(ident, key, section)
#define CFG_FONTROLE(ident, key, suffix, defFam) &rows::ident,
#include "coop/config/config_registry_rows.inc"
#undef CFG_FLAG
#undef CFG_INT
#undef CFG_FLOAT
#undef CFG_ENUM
#undef CFG_STRING
#undef CFG_IDENTITY
#undef CFG_FONTROLE
};
}  // namespace

const EnumRow& FontRoleRow(size_t roleIdx) {
    // Bounds: callers index by ui::fonts::Role, static_asserted to
    // kFontRoleCount; clamp defensively all the same.
    if (roleIdx >= kFontRoleCount) roleIdx = 0;
    return *kFontRoleRows[roleIdx];
}

const Row* Rows(size_t& count) {
    count = kRowCount;
    return kRows;
}

const Row* FindRow(const char* key) {
    if (!key || !*key) return nullptr;
    for (const Row& r : kRows)
        if (_stricmp(r.key, key) == 0) return &r;
    return nullptr;
}

bool IsKnownKey(const char* key) {
    // Since arc 3 the ui.font.<role> family are REAL rows -- one lookup.
    return FindRow(key) != nullptr;
}

}  // namespace coop::config_registry
