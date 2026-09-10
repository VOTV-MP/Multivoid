// coop/config/config_registry.h -- the declarative config registry, the single source for
// per-key config metadata: the canonical key spelling and its multivoid.ini section; the
// value kind (flag, int, float, enum, free string, minted identity) with the numeric range or
// the enum token list; the typed default, aliasing the one owning constant where one exists
// (the default port, the official master URL), never a second copy; the twin environment
// variable (env beats ini); and the one seeded-active marker (net.nick). The ratchet: the
// only public read and write APIs (config.h) take the typed handles declared below, which
// are constructible only by the registry TU (a private-tag constructor), so a future
// producer cannot mint an unregistered key and a wrong-kind read is a compile error. The row
// list itself lives in config_registry_rows.inc, one list feeding both the table and the
// handles. ValidateRows (config_registry.cpp) is a constexpr compile gate: numeric defaults
// in range, enum defaults among the tokens (an empty-sentinel allowlist for net.role and
// net.ice), font-role rows coherent in key, suffix and default family.

#pragma once

#include <cstddef>
#include <string>

namespace coop::config_registry {

// The default for my own display name wherever the local player's nick is resolved with
// nothing configured (env absent, ini absent). A fresh ini seeds a visible net.nick line
// meant to be seen and replaced, so the value's whole job is to announce itself as a
// placeholder rather than read as someone's name. Never used for other peers' missing nicks.
// The length matters: a nick is capped at kNickMaxChars codepoints and the host's arbiter
// appends a dense smallest-free suffix over the whole requested name as the stem; every
// fresh install seeds the same value, so a full lobby is the collision case by construction,
// and the longest name this produces (the default plus a one-digit suffix) must fit the cap.
// The arbiter also sizes its variants against the cap, so a longer default degrades
// gracefully, but keep a new default short enough not to rely on that.
inline constexpr const char* kMyNameDefault = "PlayerNickname";

// The wide twin, built from the one narrow constant (ASCII), never a second literal: the
// transcription drift this header exists to kill.
inline std::wstring MyNameDefaultW() {
    std::wstring w;
    for (const char* p = kMyNameDefault; *p; ++p) w.push_back(static_cast<wchar_t>(*p));
    return w;
}

// The canonical multivoid.ini section order: [net] first and [dev] last, the middle grouped
// by domain. Sections are decorative to the parser; this order exists for the human reading
// the file and for the writer's section placement in a headered file.
inline constexpr const char* kSectionOrder[] = {
    "net",     // multiplayer: nick, master, signaling, topology...
    "player",  // durable identity: player_guid, player_skin, nameplate, nick_color
    "ui",      // fonts, scale, panels
    "voice",   // devices, gates, volumes
    "dev",     // dev/test flags -- deliberately last, out of casual sight
};
inline constexpr size_t kSectionCount = sizeof(kSectionOrder) / sizeof(kSectionOrder[0]);

// The per-key row table.

enum class Kind : unsigned char {
    Flag,      // truthiness: 1|true|yes|on / 0|false|no|off (ci); anything else = garbage
    Int,       // integer; valid iff the WHOLE string parses and lands in [lo, hi]
    Float,     // float; same whole-string + range rule
    Enum,      // one of `tokens` (ci, '|'-separated); anything else = garbage
    String,    // free string -- no value validation
    Identity,  // minted + persisted by the mod (player_guid / player_skin); no def
};

struct Row {
    const char* key;      // canonical spelling (all lowercase; no case-insensitive twins)
    const char* section;  // one of kSectionOrder
    Kind kind;
    double lo, hi;        // Int/Float validity range (unused otherwise)
    const char* tokens;   // Enum: "a|b|c" (ci). nullptr otherwise.
    const char* envVar;   // twin env var (env beats ini) or nullptr
    bool seededActive;    // the skeleton seeds the key with the name default (net.nick only)
    // The typed default: exactly the kind's member is meaningful; Identity rows have none.
    bool defB;
    long defI;
    float defF;
    const char* defS;     // Enum: the default token, empty meaning unset; String: the default; null otherwise
    // The catalog columns.
    const char* gatedBy;  // key of the Flag row gating the ini read, or null; must exist and be a Flag row
    const char* desc;     // catalog text, semantics only; tokens, range and env twin are emitted from the columns
};

// The row list. Completeness against the call-site universe is enforced by the ratchet
// itself (no string-keyed read or write API exists); the reverse direction, a row nobody
// references, is policed by .github/ci/registry_gate.ps1 in CI.
const Row* Rows(size_t& count);

// The first row whose key equals `key` case-insensitively, or null. For the schema's own
// machinery only (the unknown-key sweep, the writer and the panel classify keys discovered
// in the file, inherently by string); its result feeds no read API, since typed handles
// cannot be built from it outside the registry TU.
const Row* FindRow(const char* key);

// True if `key` is a registry key (case-insensitive). The unknown-key report is the
// complement of this predicate.
bool IsKnownKey(const char* key);

// A key that used to be a real setting and was retired: the sentence a player should read
// instead of a typo warning, or null for a key this build never had. Keys get retired, and
// the settings sweep can only ask whether a key is in the registry, so every retirement
// would otherwise present a player with their own ini line flagged as a probable typo, in a
// popup, with no explanation; the next retirement gets the same treatment for one added
// line. The row stays unknown, so the tidy-up still removes it; what changes is that the
// panel can say where the setting went.
const char* RetiredKeyNote(const char* key);

// The typed handles.

namespace detail {
// The private construction tag: only the registry TU can mint handles, so a row pointer from
// FindRow cannot be wrapped elsewhere.
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
// Identity rows have no read handle (the mint machinery reads them internally, in
// config.cpp); this handle exists for the write door only, so the mint persist and the skin
// picker go through the same typed write as every other product write.
struct IdentityRow {
    const Row* row;
    constexpr IdentityRow(const Row* r, detail::RegistryCtorKey) : row(r) {}
};

// The named handles, one per row, generated from config_registry_rows.inc (definitions in
// config_registry.cpp). Reference them qualified: using-directives, using-declarations and
// namespace aliases for this namespace are forbidden and CI-asserted, which is what makes
// the gate's reference census exact.
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

// The composed ui.font.<role> family, real Enum rows.

// Role ini-key suffixes, in the font Role order (fonts.cpp asserts its role table against
// this count; ValidateRows pins each font-role row's key to this suffix, in this order).
inline constexpr const char* kFontRoleKeys[] = {
    "menu", "chat", "net", "nameplate", "toast",
};
inline constexpr size_t kFontRoleCount = sizeof(kFontRoleKeys) / sizeof(kFontRoleKeys[0]);

// Font family ini tokens, in the font Family order (fonts.cpp consumes these by index; the
// token spelling lives only here).
inline constexpr const char* kFontFamilyTokens[] = {
    "jetbrains", "roboto", "cascadia", "fixedsys",
};
inline constexpr size_t kFontFamilyCount =
    sizeof(kFontFamilyTokens) / sizeof(kFontFamilyTokens[0]);

// The per-role default family index, generated from the same rows; the per-role assignment
// lives in the row list only.
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

// The per-role Enum row handle, in Role order, for the fonts module's indexed read.
const EnumRow& FontRoleRow(size_t roleIdx);

}  // namespace coop::config_registry
