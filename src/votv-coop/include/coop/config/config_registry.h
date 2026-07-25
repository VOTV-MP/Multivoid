// coop/config/config_registry.h -- the declarative config registry.
//
// BORN (ini rework arc 1, 2026-07-25) carrying exactly TWO pieces of
// single-source config data, per the certified design (research/findings/
// tooling/votv-ini-config-registry-DESIGN-2026-07-24.md, T1/T7):
//   1. the canonical SECTION ORDER of multivoid.ini ([net] first, [dev] last
//      -- the user's verbatim ask), consumed by the T1 skeleton seeder and by
//      the T3 writer's section placement (inert until the arc-2 per-key
//      section column lands);
//   2. the MY-NAME default constant ("Pelmentor"), consumed by the five
//      my-name sites (F19). Someone-else's-name fallbacks are NOT this
//      constant and keep their own literals.
// The full per-key row table (T2-enum) is an ARC-2 column; nothing here is
// transcribed anywhere else -- both consumers read THIS table.

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
// the parser (F3) -- this order exists for the human reading the file.
inline constexpr const char* kSectionOrder[] = {
    "net",     // multiplayer: nick, master, signaling, topology...
    "player",  // durable identity: player_guid, player_skin, nameplate, nick_color
    "ui",      // fonts, scale, panels
    "voice",   // devices, gates, volumes
    "dev",     // dev/test flags -- deliberately last, out of casual sight
};
inline constexpr size_t kSectionCount = sizeof(kSectionOrder) / sizeof(kSectionOrder[0]);

}  // namespace coop::config_registry
