// coop/config/config_registry.h -- the declarative config registry.
//
// BORN (ini rework arc 1, 2026-07-25) with the section order + the my-name
// constant; GREW the full per-key row table in arc 2 (T2-enum), per the
// certified design (research/findings/tooling/
// votv-ini-config-registry-DESIGN-2026-07-24.md, T2).
//
// The registry is the SINGLE SOURCE for per-key config metadata:
//   - the canonical key spelling + its multivoid.ini section;
//   - the value KIND (flag / int / float / enum / free string / minted
//     identity) with the numeric range or the enum token list;
//   - the twin environment variable (env beats ini, F17);
//   - the one seeded-active marker (net.nick, user-ruled).
// DEFAULTS are NOT here yet: default ownership migrates from the call-site
// literals in arc 3 (T2-migrate); in arc 2 every Resolve call still passes its
// site default. Columns with no arc-2 consumer (gatedBy, catalog comment) are
// NOT born until their consumer is (arc 4) -- a consumer-less column is the
// banner-marker mistake (RULE 2 at birth).
//
// The composed key family ui.font.<role> is included BY REFERENCE: this header
// owns the role-name list (kFontRoleKeys) and the family token list
// (kFontFamilyTokens); ui/fonts.cpp consumes BOTH by index (static_assert on
// the counts), so a 6th role or a 5th family lands here once and everywhere.

#pragma once

#include <cstddef>
#include <string>

namespace coop::config_registry {

// The default for MY OWN display name everywhere the local player's nick is
// resolved with nothing configured (env absent, ini absent). The joke is
// deliberate and user-ruled: a fresh ini SEEDS a visible net.nick=Pelmentor
// line meant to be seen and replaced ("оно должно не навязываться, игрок
// поменяет это на свой ник"). Never used for OTHER peers' missing nicks.
inline constexpr const char* kMyNameDefault = "Pelmentor";

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
    "dev",     // dev/test flags -- deliberately last, out of casual sight
};
inline constexpr size_t kSectionCount = sizeof(kSectionOrder) / sizeof(kSectionOrder[0]);

// ---- the per-key row table (T2-enum, arc 2) ---------------------------------

enum class Kind : unsigned char {
    Flag,      // truthiness: 1|true|yes|on / 0|false|no|off (ci); anything else = garbage
    Int,       // integer; valid iff the WHOLE string parses and lands in [lo, hi]
    Float,     // float; same whole-string + range rule
    Enum,      // one of `tokens` (ci, '|'-separated); anything else = garbage
    String,    // free string -- no value validation (arc-3 migration candidates)
    Identity,  // minted + persisted by the mod (player_guid / player_skin)
};

struct Row {
    const char* key;      // canonical spelling (all-lowercase; F40: no ci twins)
    const char* section;  // one of kSectionOrder
    Kind kind;
    double lo, hi;        // Int/Float validity range (unused otherwise)
    const char* tokens;   // Enum: "a|b|c" (ci). nullptr otherwise.
    const char* envVar;   // twin env var (env beats ini) or nullptr
    bool seededActive;    // T1: the skeleton seeds key=kMyNameDefault (net.nick only)
};

// The literal-key rows. Completeness vs the call-site census is proven by the
// enum-completeness instrument (design §5/T2), not by eyeballing.
const Row* Rows(size_t& count);

// First row whose key ci-equals `key`, or nullptr. The unified occurrence rule
// (T4) makes ci the ONE key-equality everywhere; F40 measured zero ci
// collisions between distinct keys.
const Row* FindRow(const char* key);

// True if `key` is a registry literal key OR a valid composed key
// (ui.font.<role> for a known role). The T10 unknown-key report is the
// complement of this predicate.
bool IsKnownKey(const char* key);

// ---- the composed ui.font.<role> family (included by reference, F41) --------

// Role ini-key suffixes, in ui::fonts::Role order (fonts.cpp static_asserts
// its kRoles table against this count and composes keys from these names).
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

}  // namespace coop::config_registry
