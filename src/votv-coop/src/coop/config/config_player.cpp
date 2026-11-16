// coop/config/config_player.cpp -- the readers of the player's own rows, nickname and body skin,
// and the durability flag of the skin mint. They name game modules (the skin registry, the
// handshake's nickname cap), so they are the DLL's: the rest of the config reader core has no
// game behind it and is linked into the net library.

#include "coop/config/config.h"

#include "config_ini_read.h"
#include "config_internal.h"
#include "coop/config/config_registry.h"
#include "coop/player/skin_registry.h"  // IsValidSkinName and PickRandomStarterSkin
#include "coop/session/player_handshake.h"  // kNickMaxChars
#include "coop/text/utf8_codec.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <string>

namespace coop::config {

namespace {

using IniScan = internal::IniScan;

// Whether the minted identity is session-only (the mint gate, or a failed persist); the boot
// sweep reads it for its panel row.
std::atomic<bool> g_identityNotDurable{false};

}  // namespace

std::wstring ReadNickname() {
    // The nickname default is the registry row's; the env name rides the row.
    std::string nick = ResolveString(config_registry::rows::net_nick);
    // The ini holds UTF-8, widened as UTF-8: a byte-at-a-time widen turned a UTF-8 name into
    // mojibake that the sanitiser stripped whole, the root of Cyrillic nicknames not working. Lossy
    // is right here and only here (the file is ours). The cap is in bytes because the wire's
    // nicklen is a uint8, and in characters because that is the display policy: two bounds, on
    // boundaries a raw resize would split.
    nick = coop::text::CapUtf8Bytes(std::move(nick), coop::text::kNickMaxBytes);
    return coop::text::CapCodepoints(
        coop::text::FromUtf8Lossy(nick.data(), nick.size()),
        coop::player_handshake::kNickMaxChars);
}

// The mint gate: the skin identity mints and persists only when the ini's answer is authoritative
// (a clean scan said absent or malformed, or the file is absent). On Unreadable (a share lock, a
// mid-stream error, bytes that are not text) it is session-only and nothing is written: reading a
// locked file as absent once minted and overwrote the real ini the moment the lock released, and a
// file saved as UTF-16 was cut to the one line the mint appended. The boot panel shows an
// "identity not durable" row for either outcome.
std::string ReadPlayerSkin() {
    // The persisted body-skin choice (multivoid.ini player_skin=). A new identity (an absent or
    // malformed key) rolls a random starter from the converter-skin list, filtered to the paks on
    // this install, the stock body when none; persisted at once, so the roll happens once per
    // identity.
    IniScan st = IniScan::Ok;
    IniFault fault = IniFault::None;
    std::string skin = internal::ReadLiveIniWithScan("player_skin", st, fault);
    if (!coop::skins::IsValidSkinName(skin)) {
        skin = coop::skins::PickRandomStarterSkin();
        const std::string shownSkin =
            config_registry::ValueForLog(config_registry::FindRow("player_skin"), skin);
        if (st == IniScan::Unreadable) {
            g_identityNotDurable.store(true, std::memory_order_relaxed);
            UE_LOGW("config: player_skin unreadable (multivoid.ini %s) -> '%s' "
                    "SESSION-ONLY; mint gate refuses to write over an unreadable ini",
                    IniFaultWords(fault), shownSkin.c_str());
        } else {
            // The log says whether the persist happened; on a locked file it does not.
            const bool persisted = WriteIniValue(config_registry::rows::player_skin, skin.c_str());
            if (!persisted) g_identityNotDurable.store(true, std::memory_order_relaxed);
            UE_LOGI("config: player_skin absent/invalid -> random starter '%s' (%s)",
                    shownSkin.c_str(),
                    persisted ? "persisted to multivoid.ini" : "SESSION-ONLY -- ini write failed");
        }
    }
    return skin;
}

bool IdentityNotDurable() { return g_identityNotDurable.load(std::memory_order_relaxed); }

}  // namespace coop::config
