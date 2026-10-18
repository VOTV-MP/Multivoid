// coop/config/config_registry.cpp -- the declarative per-key row table.
//
// See config_registry.h. The row LIST lives in config_registry_rows.inc (ONE
// list); this TU expands it seven times -- index enum, Row table, font-role row
// indices, row flags, row labels, typed handle definitions, font-role handle array -- so
// the artifacts cannot drift. ValidateRows() is the permanent constexpr
// compile gate on the table's own coherence (arc 3); the flag asserts below
// the credential list are the same gate for the rows' scope marks.

#include "coop/config/config_registry.h"

#include "coop/net/connect_history.h"  // History::kMaxStamps, the cap row's upper bound
#include "coop/net/protocol.h"  // kDefaultPort (row defaults ALIAS the one owning constant)
#include "ue_wrap/core/log.h"

#include <array>
#include <cstring>
#include <functional>
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
#define CFG_FLAG(ident, key, section, defB, envVar, desc) RowIndex_##ident,
#define CFG_INT(ident, key, section, defI, lo, hi, envVar, desc) RowIndex_##ident,
#define CFG_FLOAT(ident, key, section, defF, lo, hi, envVar, desc) RowIndex_##ident,
#define CFG_ENUM(ident, key, section, defS, tokens, envVar, desc) RowIndex_##ident,
#define CFG_ENUM_FAILCLOSED(ident, key, section, defS, tokens, envVar, desc) RowIndex_##ident,
#define CFG_STRING(ident, key, section, defS, envVar, seeded, desc) RowIndex_##ident,
#define CFG_IDENTITY(ident, key, section, desc) RowIndex_##ident,
#define CFG_FONTROLE(ident, key, suffix, defFam, desc) RowIndex_##ident,
#define CFG_ROWFLAGS(ident, flags)
#define CFG_LABEL(ident, text)
#include "coop/config/config_registry_rows.inc"
#undef CFG_LABEL
#undef CFG_ROWFLAGS
#undef CFG_FLAG
#undef CFG_INT
#undef CFG_FLOAT
#undef CFG_ENUM
#undef CFG_ENUM_FAILCLOSED
#undef CFG_STRING
#undef CFG_IDENTITY
#undef CFG_FONTROLE
    kRowCountIndex
};

// ---- expansion 2: the Row table ---------------------------------------------
constexpr Row kRows[] = {
#define CFG_FLAG(ident, key, section, defB, envVar, desc) \
    Row{key, section, Kind::Flag, kNoRange, kNoRange, nullptr, envVar, false, defB, 0, 0.0f, nullptr, desc},
#define CFG_INT(ident, key, section, defI, lo, hi, envVar, desc) \
    Row{key, section, Kind::Int, lo, hi, nullptr, envVar, false, false, static_cast<long>(defI), 0.0f, nullptr, desc},
#define CFG_FLOAT(ident, key, section, defF, lo, hi, envVar, desc) \
    Row{key, section, Kind::Float, lo, hi, nullptr, envVar, false, false, 0, defF, nullptr, desc},
#define CFG_ENUM(ident, key, section, defS, tokens, envVar, desc) \
    Row{key, section, Kind::Enum, kNoRange, kNoRange, tokens, envVar, false, false, 0, 0.0f, defS, desc},
#define CFG_ENUM_FAILCLOSED(ident, key, section, defS, tokens, envVar, desc) \
    Row{key, section, Kind::Enum, kNoRange, kNoRange, tokens, envVar, false, false, 0, 0.0f, defS, desc, true},
#define CFG_STRING(ident, key, section, defS, envVar, seeded, desc) \
    Row{key, section, Kind::String, kNoRange, kNoRange, nullptr, envVar, seeded, false, 0, 0.0f, defS, desc},
#define CFG_IDENTITY(ident, key, section, desc) \
    Row{key, section, Kind::Identity, kNoRange, kNoRange, nullptr, nullptr, false, false, 0, 0.0f, nullptr, desc},
#define CFG_FONTROLE(ident, key, suffix, defFam, desc) \
    Row{key, "ui", Kind::Enum, kNoRange, kNoRange, kFontFamilyTokensJoined, nullptr, false, false, 0, 0.0f, \
        kFontFamilyTokens[defFam], desc},
#define CFG_ROWFLAGS(ident, flags)
#define CFG_LABEL(ident, text)
#include "coop/config/config_registry_rows.inc"
#undef CFG_LABEL
#undef CFG_ROWFLAGS
#undef CFG_FLAG
#undef CFG_INT
#undef CFG_FLOAT
#undef CFG_ENUM
#undef CFG_ENUM_FAILCLOSED
#undef CFG_STRING
#undef CFG_IDENTITY
#undef CFG_FONTROLE
};
constexpr size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);
static_assert(kRowCount == kRowCountIndex, "index enum and row table drifted");

// ---- the row flags (CFG_ROWFLAGS lines, by row identifier) -------------------
// Zero for a row with no line. A line naming no row does not compile (RowIndex_<ident>), and a row
// marked twice throws in constant evaluation, which is a compile error.
constexpr std::array<unsigned, kRowCountIndex> BuildRowFlags() {
    std::array<unsigned, kRowCountIndex> a{};
#define CFG_FLAG(ident, key, section, defB, envVar, desc)
#define CFG_INT(ident, key, section, defI, lo, hi, envVar, desc)
#define CFG_FLOAT(ident, key, section, defF, lo, hi, envVar, desc)
#define CFG_ENUM(ident, key, section, defS, tokens, envVar, desc)
#define CFG_ENUM_FAILCLOSED(ident, key, section, defS, tokens, envVar, desc)
#define CFG_STRING(ident, key, section, defS, envVar, seeded, desc)
#define CFG_IDENTITY(ident, key, section, desc)
#define CFG_FONTROLE(ident, key, suffix, defFam, desc)
#define CFG_ROWFLAGS(ident, flags) \
    a[RowIndex_##ident] = (a[RowIndex_##ident] != 0 ? throw "a row is marked twice" : (flags));
#define CFG_LABEL(ident, text)
#include "coop/config/config_registry_rows.inc"
#undef CFG_LABEL
#undef CFG_ROWFLAGS
#undef CFG_FLAG
#undef CFG_INT
#undef CFG_FLOAT
#undef CFG_ENUM
#undef CFG_ENUM_FAILCLOSED
#undef CFG_STRING
#undef CFG_IDENTITY
#undef CFG_FONTROLE
    return a;
}
constexpr auto kRowFlags = BuildRowFlags();

// ---- the row labels (CFG_LABEL lines, by row identifier) ----------------------
// The pairs of a row and its plain-English name, from the CFG_LABEL lines alone. A line naming no
// row does not compile (RowIndex_<ident>), and a row labelled twice throws in constant evaluation,
// which is a compile error.
struct RowLabelEntry {
    const Row* row;
    const char* label;
};
constexpr RowLabelEntry kRowLabels[] = {
#define CFG_FLAG(ident, key, section, defB, envVar, desc)
#define CFG_INT(ident, key, section, defI, lo, hi, envVar, desc)
#define CFG_FLOAT(ident, key, section, defF, lo, hi, envVar, desc)
#define CFG_ENUM(ident, key, section, defS, tokens, envVar, desc)
#define CFG_ENUM_FAILCLOSED(ident, key, section, defS, tokens, envVar, desc)
#define CFG_STRING(ident, key, section, defS, envVar, seeded, desc)
#define CFG_IDENTITY(ident, key, section, desc)
#define CFG_FONTROLE(ident, key, suffix, defFam, desc)
#define CFG_ROWFLAGS(ident, flags)
#define CFG_LABEL(ident, text) {&kRows[RowIndex_##ident], text},
#include "coop/config/config_registry_rows.inc"
#undef CFG_LABEL
#undef CFG_ROWFLAGS
#undef CFG_FLAG
#undef CFG_INT
#undef CFG_FLOAT
#undef CFG_ENUM
#undef CFG_ENUM_FAILCLOSED
#undef CFG_STRING
#undef CFG_IDENTITY
#undef CFG_FONTROLE
};
constexpr size_t kRowLabelCount = sizeof(kRowLabels) / sizeof(kRowLabels[0]);
constexpr bool RowsLabelledOnce() {
    for (size_t i = 0; i < kRowLabelCount; ++i)
        for (size_t j = i + 1; j < kRowLabelCount; ++j)
            if (kRowLabels[i].row == kRowLabels[j].row) throw "a row is labelled twice";
    return true;
}
static_assert(RowsLabelledOnce(), "a row is labelled twice");

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
#define CFG_FLAG(ident, key, section, defB, envVar, desc)
#define CFG_INT(ident, key, section, defI, lo, hi, envVar, desc)
#define CFG_FLOAT(ident, key, section, defF, lo, hi, envVar, desc)
#define CFG_ENUM(ident, key, section, defS, tokens, envVar, desc)
#define CFG_ENUM_FAILCLOSED(ident, key, section, defS, tokens, envVar, desc)
#define CFG_STRING(ident, key, section, defS, envVar, seeded, desc)
#define CFG_IDENTITY(ident, key, section, desc)
#define CFG_FONTROLE(ident, key, suffix, defFam, desc) RowIndex_##ident,
#define CFG_ROWFLAGS(ident, flags)
#define CFG_LABEL(ident, text)
#include "coop/config/config_registry_rows.inc"
#undef CFG_LABEL
#undef CFG_ROWFLAGS
#undef CFG_FLAG
#undef CFG_INT
#undef CFG_FLOAT
#undef CFG_ENUM
#undef CFG_ENUM_FAILCLOSED
#undef CFG_STRING
#undef CFG_IDENTITY
#undef CFG_FONTROLE
};
static_assert(sizeof(kFontRoleRowIndex) / sizeof(kFontRoleRowIndex[0]) == kFontRoleCount,
              "font-role rows and kFontRoleKeys must stay in lockstep");

// The defS catalog-safety guard (arc 4): no ';' anywhere (the inline-comment
// strip could truncate the value on read-back), no edge whitespace (the lexer
// edge-trims -- the stored default would not round-trip), no leading '"' (the
// reader would take it as a quoted value), no CR/LF.
constexpr bool DefSCatalogSafe(const char* s) {
    if (!s || !*s) return true;  // empty defaults are legal (round-trip as present-empty)
    const char* p = s;
    if (*p == ' ' || *p == '\t' || *p == '"') return false;
    const char* last = p;
    while (*p) {
        if (*p == ';' || *p == '\r' || *p == '\n') return false;
        last = p;
        ++p;
    }
    return !(*last == ' ' || *last == '\t');
}

constexpr bool ValidateRows() {
    for (size_t i = 0; i < kRowCount; ++i) {
        const Row& r = kRows[i];
        if (!r.key || !*r.key || !SectionKnown(r.section)) return false;
        // Every row carries human text for the catalog.
        if (!r.desc || !*r.desc) return false;
        if ((r.kind == Kind::String || r.kind == Kind::Enum) && !DefSCatalogSafe(r.defS))
            return false;
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
                // "" = the unset sentinel, allowed ONLY for the allowlisted key.
                if (CEq(r.defS, "")) {
                    if (!CEq(r.key, "net.role")) return false;
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
#define CFG_FLAG(ident, key, section, defB, envVar, desc) \
    const FlagRow ident{&kRows[RowIndex_##ident], detail::RegistryDef::K()};
#define CFG_INT(ident, key, section, defI, lo, hi, envVar, desc) \
    const IntRow ident{&kRows[RowIndex_##ident], detail::RegistryDef::K()};
#define CFG_FLOAT(ident, key, section, defF, lo, hi, envVar, desc) \
    const FloatRow ident{&kRows[RowIndex_##ident], detail::RegistryDef::K()};
#define CFG_ENUM(ident, key, section, defS, tokens, envVar, desc) \
    const EnumRow ident{&kRows[RowIndex_##ident], detail::RegistryDef::K()};
#define CFG_ENUM_FAILCLOSED(ident, key, section, defS, tokens, envVar, desc) \
    const FailClosedEnumRow ident{&kRows[RowIndex_##ident], detail::RegistryDef::K()};
#define CFG_STRING(ident, key, section, defS, envVar, seeded, desc) \
    const StringRow ident{&kRows[RowIndex_##ident], detail::RegistryDef::K()};
#define CFG_IDENTITY(ident, key, section, desc) \
    const IdentityRow ident{&kRows[RowIndex_##ident], detail::RegistryDef::K()};
#define CFG_FONTROLE(ident, key, suffix, defFam, desc) \
    const EnumRow ident{&kRows[RowIndex_##ident], detail::RegistryDef::K()};
#define CFG_ROWFLAGS(ident, flags)
#define CFG_LABEL(ident, text)
#include "coop/config/config_registry_rows.inc"
#undef CFG_LABEL
#undef CFG_ROWFLAGS
#undef CFG_FLAG
#undef CFG_INT
#undef CFG_FLOAT
#undef CFG_ENUM
#undef CFG_ENUM_FAILCLOSED
#undef CFG_STRING
#undef CFG_IDENTITY
#undef CFG_FONTROLE
}  // namespace rows

namespace {
// The per-role Enum handles in Role order (FontRoleRow's backing).
const EnumRow* const kFontRoleRows[] = {
#define CFG_FLAG(ident, key, section, defB, envVar, desc)
#define CFG_INT(ident, key, section, defI, lo, hi, envVar, desc)
#define CFG_FLOAT(ident, key, section, defF, lo, hi, envVar, desc)
#define CFG_ENUM(ident, key, section, defS, tokens, envVar, desc)
#define CFG_ENUM_FAILCLOSED(ident, key, section, defS, tokens, envVar, desc)
#define CFG_STRING(ident, key, section, defS, envVar, seeded, desc)
#define CFG_IDENTITY(ident, key, section, desc)
#define CFG_FONTROLE(ident, key, suffix, defFam, desc) &rows::ident,
#define CFG_ROWFLAGS(ident, flags)
#define CFG_LABEL(ident, text)
#include "coop/config/config_registry_rows.inc"
#undef CFG_LABEL
#undef CFG_ROWFLAGS
#undef CFG_FLAG
#undef CFG_INT
#undef CFG_FLOAT
#undef CFG_ENUM
#undef CFG_ENUM_FAILCLOSED
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

const char* RetiredKeyNote(const char* key) {
    if (!key) return nullptr;
    struct Retired { const char* key; const char* note; };
    // Keep the newest first; a line stays here for as long as an ini written by
    // the build that had the key could still be on someone's disk.
    static const Retired kRetired[] = {
        { "net.master.custom",
          "retired when the server browser got its list of masters -- a master of your own "
          "is now an entry in net.masters (Name=host:port), picked with its tab in the "
          "browser. This line does nothing now and is safe to remove." },
        { "warn.perf_mods",
          "retired in b146 together with the frame-rate notice it controlled -- the mod "
          "census now only writes a line to multivoid.log and never interrupts you. This "
          "line does nothing now and is safe to remove." },
        { "player_guid",
          "retired in b144 -- your player identity moved to the file "
          "multivoid_identity.key in the mod's own folder (Mods\\Multivoid), which "
          "cannot be copied out of a screenshot. This line does nothing now and is "
          "safe to remove." },
        { "net.identity",
          "retired in b144 -- a peer's network name is now its own identity key, "
          "so there is nothing left to configure. Safe to remove." },
    };
    for (const Retired& r : kRetired)
        if (_stricmp(key, r.key) == 0) return r.note;
    return nullptr;
}

// Named rows rather than a spelling rule, so the list has the failure mode of going STALE: a
// credential row renamed without touching this array stops matching and its value starts reaching
// the log. CredentialKeys exposes the array so a caller can check each name still resolves to a
// row, which is a louder failure than a silently unredacted password.
constexpr const char* const kCredentials[] = {
    "net.lobby_password",    // the secret a locked lobby requires
    "net.join_password",     // the secret a joiner offers
    "net.signaling_token",   // the relay's bearer
    "net.turn_pass",         // the TURN credential
    "net.turn_user",         // the TURN account name: a credential of the pair, so not quoted
};

// The scope marks, checked by the compiler (Source declares FCVAR_REPLICATED and FCVAR_PROTECTED
// on the cvar itself and keeps the two apart, iconvar.h:62, 46). Both lists are lowercase by the
// registry's rule (Row::key), so the exact compare is IsCredentialKey's case-insensitive test.
namespace {
constexpr size_t CLen(const char* s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}
constexpr bool ReplicatedImpliesServer() {
    for (size_t i = 0; i < kRowCount; ++i)
        if ((kRowFlags[i] & kRowReplicated) && !(kRowFlags[i] & kRowServer)) return false;
    return true;
}
constexpr bool NoLabelledIdentity() {
    for (size_t i = 0; i < kRowLabelCount; ++i)
        if (kRowLabels[i].row->kind == Kind::Identity) return false;
    return true;
}
constexpr bool NoReplicatedCredential() {
    for (size_t i = 0; i < kRowCount; ++i) {
        if (!(kRowFlags[i] & kRowReplicated)) continue;
        for (const char* c : kCredentials)
            if (CEq(kRows[i].key, c)) return false;
    }
    return true;
}
constexpr bool NoCredentialAddress() {
    for (size_t i = 0; i < kRowCount; ++i) {
        if (!(kRowFlags[i] & kRowAddress)) continue;
        for (const char* c : kCredentials)
            if (CEq(kRows[i].key, c)) return false;
    }
    return true;
}
constexpr bool NoServerAddress() {
    for (size_t i = 0; i < kRowCount; ++i)
        if ((kRowFlags[i] & kRowServer) && (kRowFlags[i] & kRowAddress)) return false;
    return true;
}
constexpr bool NotifyImpliesReplicated() {
    for (size_t i = 0; i < kRowCount; ++i)
        if ((kRowFlags[i] & kRowNotify) && !(kRowFlags[i] & kRowReplicated)) return false;
    return true;
}
constexpr bool NoNotifyAddress() {
    for (size_t i = 0; i < kRowCount; ++i)
        if ((kRowFlags[i] & kRowNotify) && (kRowFlags[i] & kRowAddress)) return false;
    return true;
}
constexpr bool NotifyIsLabelled() {
    for (size_t i = 0; i < kRowCount; ++i) {
        if (!(kRowFlags[i] & kRowNotify)) continue;
        bool labelled = false;
        for (size_t j = 0; j < kRowLabelCount; ++j)
            if (kRowLabels[j].row == &kRows[i]) labelled = true;
        if (!labelled) return false;
    }
    return true;
}
constexpr bool NotifyIsScalar() {
    for (size_t i = 0; i < kRowCount; ++i) {
        if (!(kRowFlags[i] & kRowNotify)) continue;
        const Kind k = kRows[i].kind;
        if (k != Kind::Flag && k != Kind::Int && k != Kind::Float && k != Kind::Enum) return false;
    }
    return true;
}
constexpr bool ReplicatedKeysFitTheWire() {
    for (size_t i = 0; i < kRowCount; ++i)
        if ((kRowFlags[i] & kRowReplicated) && CLen(kRows[i].key) > kServerSettingKeyMax)
            return false;
    return true;
}
constexpr bool ReplicatedDefaultsFitTheWire() {
    for (size_t i = 0; i < kRowCount; ++i)
        if ((kRowFlags[i] & kRowReplicated) && kRows[i].defS &&
            CLen(kRows[i].defS) > kServerSettingTextMax)
            return false;
    return true;
}
}  // namespace
static_assert(ReplicatedImpliesServer(), "a kRowReplicated row must also be kRowServer");
static_assert(NoLabelledIdentity(), "a labelled row must not be Kind::Identity: a pane has no drawer for it");
static_assert(NoReplicatedCredential(), "a credential row must never be kRowReplicated");
static_assert(NoCredentialAddress(), "a credential row must never be kRowAddress");
static_assert(NoServerAddress(),
              "a kRowServer row must never be kRowAddress: the action log writes a server setting's value as it is");
static_assert(NotifyImpliesReplicated(), "a notify row must be replicated");
static_assert(NoNotifyAddress(), "a notify row is never an address row");
static_assert(NotifyIsLabelled(), "a notify row must be labelled");
static_assert(NotifyIsScalar(), "a notify row must be a flag, int, float or enum");
static_assert(ReplicatedKeysFitTheWire(),
              "a kRowReplicated row's key is longer than kServerSettingKeyMax");
static_assert(ReplicatedDefaultsFitTheWire(),
              "a kRowReplicated row's default text is longer than kServerSettingTextMax");

const char* const* CredentialKeys(size_t& count) {
    count = sizeof(kCredentials) / sizeof(kCredentials[0]);
    return kCredentials;
}

bool IsCredentialKey(const char* key) {
    if (!key) return false;
    for (const char* c : kCredentials)
        if (_stricmp(key, c) == 0) return true;
    return false;
}

unsigned RowFlags(const Row* row) {
    // std::less: a total order over pointers, so a pointer outside the table is simply "outside".
    const std::less<const Row*> before;
    if (!row || before(row, kRows) || !before(row, kRows + kRowCount)) return 0;
    return kRowFlags[static_cast<size_t>(row - kRows)];
}

bool IsServerScope(const Row* row) { return (RowFlags(row) & kRowServer) != 0; }

bool IsReplicated(const Row* row) { return (RowFlags(row) & kRowReplicated) != 0; }

bool IsLive(const Row* row) { return (RowFlags(row) & kRowLive) != 0; }

bool IsAddressRow(const Row* row) { return row && (RowFlags(row) & kRowAddress) != 0; }

bool IsNotify(const Row* row) { return (RowFlags(row) & kRowNotify) != 0; }

std::string ValueForLog(const Row* row, std::string_view value) {
    if (!row) return "<not shown>";
    if (row->kind == Kind::Identity || IsCredentialKey(row->key)) return "<set>";
    if (IsAddressRow(row)) {
        if (value == (row->defS ? row->defS : "")) return std::string(value);
        return ue_wrap::log::Addr(value);
    }
    return std::string(value);
}

const char* RowLabel(const Row* row) {
    for (const RowLabelEntry& e : kRowLabels)
        if (e.row == row) return e.label;
    return nullptr;
}

bool IsKnownKey(const char* key) {
    // Since arc 3 the ui.font.<role> family are REAL rows -- one lookup.
    return FindRow(key) != nullptr;
}

}  // namespace coop::config_registry
