// coop/config/config_registry.h -- the declarative config registry.
//
// BORN (ini rework arc 1, 2026-07-25) with the section order + the my-name
// constant; GREW the full per-key row table in arc 2 (T2-enum); arc 3
// (T2-migrate) moved DEFAULT OWNERSHIP into the rows and made the row the
// TYPED READ/WRITE HANDLE (the const Row& ratchet) per the certified design
// (research/findings/tooling/votv-ini-config-registry-DESIGN-2026-07-24.md +
// the arc-3 impl design votv-ini-arc3-impl-DESIGN-2026-07-25.md).
//
// The registry is the SINGLE SOURCE for per-key config metadata:
//   - the canonical key spelling + its multivoid.ini section;
//   - the value KIND (flag / int / float / enum / free string / minted
//     identity) with the numeric range or the enum token list;
//   - the DEFAULT (arc 3) -- typed, aliasing the one owning constant where one
//     exists (kDefaultPort / kOfficialMasterUrl / ... ) -- never a second copy;
//   - the twin environment variable (env beats ini, F17);
//   - the one seeded-active marker (net.nick, user-ruled).
//
// THE RATCHET: the only public read/write APIs (config.h) take the TYPED
// handles declared below (rows::<ident>). Handles are constructible ONLY by
// the registry TU (private-tag ctor) -- a future producer cannot mint an
// unregistered key, and a wrong-kind read is a compile error. The row list
// itself lives in config_registry_rows.inc (ONE list -> table + handles).
//
// ValidateRows() (config_registry.cpp) is a constexpr compile gate: numeric
// defaults in [lo,hi], enum defaults in tokens (empty-sentinel allowlist:
// net.role / net.ice), font-role rows key/suffix/def-family coherent.

#pragma once

#include <cstddef>
#include <string>

namespace coop::config_registry {

// The default for MY OWN display name everywhere the local player's nick is
// resolved with nothing configured (env absent, ini absent). A fresh ini SEEDS
// a visible net.nick line meant to be SEEN AND REPLACED, per the standing user
// rule ("оно должно не навязываться, игрок поменяет это на свой ник"), so the
// value's whole job is to ANNOUNCE ITSELF AS A PLACEHOLDER. It used to be the
// author's own handle, which read as a name a player might mistake for someone
// else's rather than as a blank to fill in (USER 2026-08-26). Never used for
// OTHER peers' missing nicks.
//
// Length is not incidental: `[V]` `player_handshake.h:51` caps a nick at
// kNickMaxChars = 20 codepoints, and the host's arbiter appends a dense
// smallest-free suffix over the WHOLE requested name as the stem
// (`nickname_arbiter.cpp:28`). Every fresh install seeds the SAME value, so
// a full lobby is the collision case by construction, not the rare one: `[V]`
// kMaxPeers = 4, so the longest name this can produce is PlayerNickname4 at 15
// of the 20 codepoints. The arbiter also sizes its variants against the cap
// (`player_handshake.h:46`), so a longer default degrades gracefully rather
// than silently -- but keep a new default short enough not to rely on that.
inline constexpr const char* kMyNameDefault = "PlayerNickname";

// Wide twin BUILT from the one narrow constant (ASCII) -- never a second
// literal (that would be the transcription drift this header exists to kill).
inline std::wstring MyNameDefaultW() {
    std::wstring w;
    for (const char* p = kMyNameDefault; *p; ++p) w.push_back(static_cast<wchar_t>(*p));
    return w;
}

// Canonical multivoid.ini section order. [net] FIRST and [dev] LAST are the
// verbatim ask; the middle is grouped by domain. Sections are decorative to
// the parser (F3) -- this order exists for the human reading the file, and for
// the T3 writer's section placement in a headered file.
inline constexpr const char* kSectionOrder[] = {
    "net",     // multiplayer: nick, master, signaling, topology...
    "player",  // durable identity: player_guid, player_skin, nameplate, nick_color
    "ui",      // fonts, scale, panels
    "voice",   // devices, gates, volumes
    "death",   // KO-respawn: ko_respawn, ko_ragdoll_seconds, ko_invulnerable_seconds, ko_spawn_at_start
    "dev",     // dev/test flags -- deliberately last, out of casual sight
};
inline constexpr size_t kSectionCount = sizeof(kSectionOrder) / sizeof(kSectionOrder[0]);

// ---- the per-key row table (T2-enum arc 2; defaults T2-migrate arc 3) -------

enum class Kind : unsigned char {
    Flag,      // truthiness: 1|true|yes|on / 0|false|no|off (ci); anything else = garbage
    Int,       // integer; valid iff the WHOLE string parses and lands in [lo, hi]
    Float,     // float; same whole-string + range rule
    Enum,      // one of `tokens` (ci, '|'-separated); anything else = garbage
    String,    // free string -- no value validation
    Identity,  // minted + persisted by the mod (player_guid / player_skin); no def
};

struct Row {
    const char* key;      // canonical spelling (all-lowercase; F40: no ci twins)
    const char* section;  // one of kSectionOrder
    Kind kind;
    double lo, hi;        // Int/Float validity range (unused otherwise)
    const char* tokens;   // Enum: "a|b|c" (ci). nullptr otherwise.
    const char* envVar;   // twin env var (env beats ini) or nullptr
    bool seededActive;    // T1: the skeleton seeds key=kMyNameDefault (net.nick only)
    // ---- the typed default (arc 3, T2-migrate; exactly the kind's member is
    // meaningful; Identity rows have none) ------------------------------------
    bool defB;
    long defI;
    float defF;
    const char* defS;     // Enum: the default token ("" = unset sentinel);
                          // String: the default value. nullptr for other kinds.
    // ---- the T8 catalog columns (arc 4) -------------------------------------
    const char* gatedBy;  // key of the Flag row gating the INI read (nullptr =
                          // ungated; ValidateRows: must exist + be a Flag row)
    const char* desc;     // human catalog text -- SEMANTICS ONLY (tokens/range/
                          // env twin are generator-emitted from the columns)
};

// The row list. Completeness vs the call-site universe is enforced by the
// ratchet itself (no string-keyed read/write API exists); the reverse
// direction (a row nobody references) is policed by tools/config/
// registry_gate.ps1 in CI.
const Row* Rows(size_t& count);

// First row whose key ci-equals `key`, or nullptr. FOR THE SCHEMA'S OWN
// MACHINERY ONLY (the T10 sweep / T3 writer / panel classify keys discovered
// IN THE FILE, inherently by string). Its result feeds no read API: typed
// handles cannot be built from it outside the registry TU.
const Row* FindRow(const char* key);

// True if `key` is a registry key (ci). The T10 unknown-key report is the
// complement of this predicate. (Since arc 3 the ui.font.<role> family are
// real rows, so this is a plain row lookup.)
bool IsKnownKey(const char* key);

// ---- typed handles (the arc-3 ratchet) --------------------------------------

namespace detail {
// Private construction tag: only the registry TU (RegistryDef) can mint
// handles -- FindRow's Row* cannot be wrapped elsewhere (impl-design R10).
struct RegistryCtorKey {
  private:
    constexpr RegistryCtorKey() = default;
    friend struct RegistryDef;
};
struct RegistryDef;  // defined in config_registry.cpp only
}  // namespace detail

struct FlagRow {
    const Row* row;
    constexpr FlagRow(const Row* r, detail::RegistryCtorKey) : row(r) {}
};
struct IntRow {
    const Row* row;
    constexpr IntRow(const Row* r, detail::RegistryCtorKey) : row(r) {}
};
struct FloatRow {
    const Row* row;
    constexpr FloatRow(const Row* r, detail::RegistryCtorKey) : row(r) {}
};
struct EnumRow {
    const Row* row;
    constexpr EnumRow(const Row* r, detail::RegistryCtorKey) : row(r) {}
};
struct StringRow {
    const Row* row;
    constexpr StringRow(const Row* r, detail::RegistryCtorKey) : row(r) {}
};
// Identity rows have NO read handle (the mint machinery reads them internally,
// config.cpp) -- this handle exists for the WRITE door only (C3b): the mint
// persist + the skin picker go through the same typed WriteIniValue as every
// other product write.
struct IdentityRow {
    const Row* row;
    constexpr IdentityRow(const Row* r, detail::RegistryCtorKey) : row(r) {}
};

// The named handles, one per row, generated from config_registry_rows.inc
// (definitions in config_registry.cpp). Reference these QUALIFIED
// (rows::<ident>) -- using-directives/-declarations/namespace-aliases for this
// namespace are forbidden and CI-asserted (registry_gate.ps1), which is what
// makes the gate's reference census exact.
namespace rows {
#define CFG_FLAG(ident, key, section, defB, envVar, desc) extern const FlagRow ident;
#define CFG_INT(ident, key, section, defI, lo, hi, envVar, desc) extern const IntRow ident;
#define CFG_FLOAT(ident, key, section, defF, lo, hi, envVar, desc) extern const FloatRow ident;
#define CFG_ENUM(ident, key, section, defS, tokens, envVar, desc) extern const EnumRow ident;
#define CFG_STRING(ident, key, section, defS, envVar, seeded, desc) extern const StringRow ident;
#define CFG_STRING_GATED(ident, key, section, defS, envVar, seeded, gatedBy, desc) \
    extern const StringRow ident;
#define CFG_IDENTITY(ident, key, section, desc) extern const IdentityRow ident;
#define CFG_FONTROLE(ident, key, suffix, defFam, desc) extern const EnumRow ident;
#include "coop/config/config_registry_rows.inc"
#undef CFG_FLAG
#undef CFG_INT
#undef CFG_FLOAT
#undef CFG_ENUM
#undef CFG_STRING
#undef CFG_STRING_GATED
#undef CFG_IDENTITY
#undef CFG_FONTROLE
}  // namespace rows

// ---- the composed ui.font.<role> family (real Enum rows since arc 3) --------

// Role ini-key suffixes, in ui::fonts::Role order (fonts.cpp static_asserts
// its kRoles table against this count; ValidateRows pins each font-role row's
// key to "ui.font." + this suffix, in this order).
inline constexpr const char* kFontRoleKeys[] = {
    "menu", "chat", "net", "nameplate", "toast",
};
inline constexpr size_t kFontRoleCount = sizeof(kFontRoleKeys) / sizeof(kFontRoleKeys[0]);

// Font family ini tokens, in ui::fonts::Family order (fonts.cpp consumes these
// by index for its kFamilies table -- the token spelling lives ONLY here).
inline constexpr const char* kFontFamilyTokens[] = {
    "jetbrains", "roboto", "cascadia", "fixedsys",
};
inline constexpr size_t kFontFamilyCount =
    sizeof(kFontFamilyTokens) / sizeof(kFontFamilyTokens[0]);

// Per-role DEFAULT family index (into kFontFamilyTokens), generated from the
// SAME .inc rows (arc 3; replaces fonts.cpp's RoleDesc.defaultFam column --
// the user-2026-07-09 per-role assignment now lives in the row list only).
inline constexpr int kFontRoleDefaultFamily[] = {
#define CFG_FLAG(ident, key, section, defB, envVar, desc)
#define CFG_INT(ident, key, section, defI, lo, hi, envVar, desc)
#define CFG_FLOAT(ident, key, section, defF, lo, hi, envVar, desc)
#define CFG_ENUM(ident, key, section, defS, tokens, envVar, desc)
#define CFG_STRING(ident, key, section, defS, envVar, seeded, desc)
#define CFG_STRING_GATED(ident, key, section, defS, envVar, seeded, gatedBy, desc)
#define CFG_IDENTITY(ident, key, section, desc)
#define CFG_FONTROLE(ident, key, suffix, defFam, desc) defFam,
#include "coop/config/config_registry_rows.inc"
#undef CFG_FLAG
#undef CFG_INT
#undef CFG_FLOAT
#undef CFG_ENUM
#undef CFG_STRING
#undef CFG_STRING_GATED
#undef CFG_IDENTITY
#undef CFG_FONTROLE
};
static_assert(sizeof(kFontRoleDefaultFamily) / sizeof(kFontRoleDefaultFamily[0]) ==
                  kFontRoleCount,
              "font-role rows and kFontRoleKeys must stay in lockstep");

// The per-role Enum row handle, in Role order (for ui/fonts.cpp's indexed read).
const EnumRow& FontRoleRow(size_t roleIdx);

}  // namespace coop::config_registry
