// coop/session/player_handshake_prefs.cpp -- the live display-pref change
// family of the player_handshake module: SkinChange (v93) / NameplateChange
// (v94) / NickColorChange (v103). One shared shape per family member:
// AnnounceLocal* (the local player's mid-session change: client -> host,
// host -> all) + Handle* (host: forgery-guarded store + rebroadcast
// originator-excluded; client: host-only sender, store). The AT-JOIN state of
// each pref rides the Join/PlayerJoined payloads built by
// player_handshake.cpp -- this file is the LIVE-CHANGE wire only.
//
// Extracted from player_handshake.cpp 2026-07-05 (modular file-size rule:
// 1043 LOC after the v103 nick color landed). Shared internals (the skin
// side-table store + the skin field parser) come through
// player_handshake_detail.h; the public API stays in
// include/coop/session/player_handshake.h (same namespace, two TUs).

#include "coop/session/player_handshake.h"

#include "player_handshake_detail.h"  // co-located private header (src tree, not include/)

#include "coop/comms/chat_feed.h"
#include "coop/net/session.h"
#include "coop/player/nameplate.h"
#include "coop/player/nick_color.h"
#include "coop/player/players_registry.h"
#include "coop/player/skin_registry.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::player_handshake {

namespace {

// [u8 slot][u8 len][name ASCII] -- the SkinChange wire form (also what the host
// rebroadcasts verbatim).
std::vector<uint8_t> BuildSkinChangePayload(uint8_t slot, const std::string& name) {
    std::vector<uint8_t> out;
    const uint8_t len = static_cast<uint8_t>(name.size() > 48 ? 48 : name.size());
    out.reserve(2 + len);
    out.push_back(slot);
    out.push_back(len);
    out.insert(out.end(), name.begin(), name.begin() + len);
    return out;
}

// [u8 slot][u8 visible] -- the NameplateChange wire form (host rebroadcasts verbatim).
std::vector<uint8_t> BuildNameplateChangePayload(uint8_t slot, bool visible) {
    return { slot, static_cast<uint8_t>(visible ? 1 : 0) };
}

// [u8 slot][u8 has][u8 r][u8 g][u8 b] -- the NickColorChange wire form (host
// rebroadcasts verbatim).
std::vector<uint8_t> BuildNickColorChangePayload(uint8_t slot, uint32_t packed) {
    return { slot, static_cast<uint8_t>(coop::nick_color::IsCustom(packed) ? 1 : 0),
             coop::nick_color::R(packed), coop::nick_color::G(packed),
             coop::nick_color::B(packed) };
}

}  // namespace

void AnnounceLocalSkin(net::Session& session, const std::string& name) {
    UE_ASSERT_GAME_THREAD("AnnounceLocalSkin");
    if (!coop::skins::IsValidSkinName(name)) return;
    const uint8_t selfSlot = coop::players::Registry::Get().LocalPeerId();
    if (selfSlot >= net::kMaxPeers) return;  // not in a session yet -- the Join will carry it
    const std::vector<uint8_t> p = BuildSkinChangePayload(selfSlot, name);
    if (session.role() == net::Role::Host) {
        for (int x = 1; x < net::kMaxPeers; ++x) {
            if (!session.IsSlotReady(x)) continue;
            session.SendReliableToSlot(x, net::ReliableKind::SkinChange,
                                       p.data(), static_cast<int>(p.size()));
        }
    } else {
        session.SendReliableToSlot(0, net::ReliableKind::SkinChange,
                                   p.data(), static_cast<int>(p.size()));
    }
    UE_LOGI("player_handshake: announced local skin '%s' (slot %u)", name.c_str(),
            static_cast<unsigned>(selfSlot));
}

bool HandleSkinChange(net::Session& session,
                      const net::Session::ReliableMessage& msg) {
    UE_ASSERT_GAME_THREAD("g_skinBySlot (HandleSkinChange)");
    if (msg.payloadLen < 2) {
        UE_LOGW("player_handshake: SkinChange payload %zu B too short -- dropping",
                static_cast<size_t>(msg.payloadLen));
        return true;
    }
    const uint8_t describedSlot = msg.payload[0];
    std::string name;
    if (ParseSkinField(msg.payload + 1, static_cast<size_t>(msg.payloadLen) - 1, &name) == 0 ||
        name.empty()) {
        UE_LOGW("player_handshake: SkinChange malformed/invalid name from slot %d -- dropping",
                msg.senderPeerSlot);
        return true;
    }
    if (describedSlot >= net::kMaxPeers) return true;

    if (session.role() == net::Role::Host) {
        // Forgery guard: a client may only change ITS OWN skin.
        if (msg.senderPeerSlot != describedSlot || describedSlot == 0) {
            UE_LOGW("player_handshake: SkinChange slot=%u from senderSlot=%d -- forged, dropping",
                    static_cast<unsigned>(describedSlot), msg.senderPeerSlot);
            return true;
        }
        StoreSkinForSlot(describedSlot, name);
        coop::chat_feed::Push(NicknameForSlot(describedSlot) + L" changed skin to " +
                              std::wstring(name.begin(), name.end()));
        // Rebroadcast to every other ready client (originator excluded).
        const std::vector<uint8_t> p = BuildSkinChangePayload(describedSlot, name);
        for (int x = 1; x < net::kMaxPeers; ++x) {
            if (x == describedSlot) continue;
            if (!session.IsSlotReady(x)) continue;
            session.SendReliableToSlot(x, net::ReliableKind::SkinChange,
                                       p.data(), static_cast<int>(p.size()));
        }
        return true;
    }

    // Client: only the host relays skin state.
    if (msg.senderPeerSlot != 0) {
        UE_LOGW("player_handshake: SkinChange from non-host senderPeerSlot=%d -- dropping",
                msg.senderPeerSlot);
        return true;
    }
    if (describedSlot == coop::players::Registry::Get().LocalPeerId()) return true;  // our own echo
    StoreSkinForSlot(describedSlot, name);
    coop::chat_feed::Push(NicknameForSlot(describedSlot) + L" changed skin to " +
                          std::wstring(name.begin(), name.end()));
    return true;
}

void AnnounceLocalNameplate(net::Session& session, bool visible) {
    UE_ASSERT_GAME_THREAD("AnnounceLocalNameplate");
    const uint8_t selfSlot = coop::players::Registry::Get().LocalPeerId();
    if (selfSlot >= net::kMaxPeers) return;  // not in a session yet -- the Join will carry it
    const std::vector<uint8_t> p = BuildNameplateChangePayload(selfSlot, visible);
    if (session.role() == net::Role::Host) {
        for (int x = 1; x < net::kMaxPeers; ++x) {
            if (!session.IsSlotReady(x)) continue;
            session.SendReliableToSlot(x, net::ReliableKind::NameplateChange,
                                       p.data(), static_cast<int>(p.size()));
        }
    } else {
        session.SendReliableToSlot(0, net::ReliableKind::NameplateChange,
                                   p.data(), static_cast<int>(p.size()));
    }
    UE_LOGI("player_handshake: announced local nameplate %s (slot %u)",
            visible ? "VISIBLE" : "HIDDEN", static_cast<unsigned>(selfSlot));
}

bool HandleNameplateChange(net::Session& session,
                           const net::Session::ReliableMessage& msg) {
    UE_ASSERT_GAME_THREAD("HandleNameplateChange");
    if (msg.payloadLen < 2) {
        UE_LOGW("player_handshake: NameplateChange payload %zu B too short -- dropping",
                static_cast<size_t>(msg.payloadLen));
        return true;
    }
    const uint8_t describedSlot = msg.payload[0];
    const bool visible = msg.payload[1] != 0;
    if (describedSlot >= net::kMaxPeers) return true;

    if (session.role() == net::Role::Host) {
        // Forgery guard: a client may only toggle ITS OWN plate.
        if (msg.senderPeerSlot != describedSlot || describedSlot == 0) {
            UE_LOGW("player_handshake: NameplateChange slot=%u from senderSlot=%d -- forged, dropping",
                    static_cast<unsigned>(describedSlot), msg.senderPeerSlot);
            return true;
        }
        coop::nameplate::StoreVisibleForSlot(describedSlot, visible);
        // Rebroadcast to every other ready client (originator excluded).
        const std::vector<uint8_t> p = BuildNameplateChangePayload(describedSlot, visible);
        for (int x = 1; x < net::kMaxPeers; ++x) {
            if (x == describedSlot) continue;
            if (!session.IsSlotReady(x)) continue;
            session.SendReliableToSlot(x, net::ReliableKind::NameplateChange,
                                       p.data(), static_cast<int>(p.size()));
        }
        return true;
    }

    // Client: only the host relays plate prefs.
    if (msg.senderPeerSlot != 0) {
        UE_LOGW("player_handshake: NameplateChange from non-host senderPeerSlot=%d -- dropping",
                msg.senderPeerSlot);
        return true;
    }
    if (describedSlot == coop::players::Registry::Get().LocalPeerId()) return true;  // our own echo
    coop::nameplate::StoreVisibleForSlot(describedSlot, visible);
    return true;
}

void AnnounceLocalNickColor(net::Session& session, uint32_t packed) {
    UE_ASSERT_GAME_THREAD("AnnounceLocalNickColor");
    const uint8_t selfSlot = coop::players::Registry::Get().LocalPeerId();
    if (selfSlot >= net::kMaxPeers) return;  // not in a session yet -- the Join will carry it
    const std::vector<uint8_t> p = BuildNickColorChangePayload(selfSlot, packed);
    if (session.role() == net::Role::Host) {
        for (int x = 1; x < net::kMaxPeers; ++x) {
            if (!session.IsSlotReady(x)) continue;
            session.SendReliableToSlot(x, net::ReliableKind::NickColorChange,
                                       p.data(), static_cast<int>(p.size()));
        }
    } else {
        session.SendReliableToSlot(0, net::ReliableKind::NickColorChange,
                                   p.data(), static_cast<int>(p.size()));
    }
    UE_LOGI("player_handshake: announced local nick color %s (slot %u)",
            coop::nick_color::IsCustom(packed) ? "CUSTOM" : "DEFAULT",
            static_cast<unsigned>(selfSlot));
}

bool HandleNickColorChange(net::Session& session,
                           const net::Session::ReliableMessage& msg) {
    UE_ASSERT_GAME_THREAD("HandleNickColorChange");
    if (msg.payloadLen < 5) {
        UE_LOGW("player_handshake: NickColorChange payload %zu B too short -- dropping",
                static_cast<size_t>(msg.payloadLen));
        return true;
    }
    const uint8_t describedSlot = msg.payload[0];
    const uint32_t packed = msg.payload[1] != 0
        ? coop::nick_color::Pack(msg.payload[2], msg.payload[3], msg.payload[4])
        : 0u;
    if (describedSlot >= net::kMaxPeers) return true;

    if (session.role() == net::Role::Host) {
        // Forgery guard: a client may only color ITS OWN nick.
        if (msg.senderPeerSlot != describedSlot || describedSlot == 0) {
            UE_LOGW("player_handshake: NickColorChange slot=%u from senderSlot=%d -- forged, dropping",
                    static_cast<unsigned>(describedSlot), msg.senderPeerSlot);
            return true;
        }
        coop::nick_color::StoreForSlot(describedSlot, packed);
        // Rebroadcast to every other ready client (originator excluded).
        const std::vector<uint8_t> p = BuildNickColorChangePayload(describedSlot, packed);
        for (int x = 1; x < net::kMaxPeers; ++x) {
            if (x == describedSlot) continue;
            if (!session.IsSlotReady(x)) continue;
            session.SendReliableToSlot(x, net::ReliableKind::NickColorChange,
                                       p.data(), static_cast<int>(p.size()));
        }
        return true;
    }

    // Client: only the host relays color prefs.
    if (msg.senderPeerSlot != 0) {
        UE_LOGW("player_handshake: NickColorChange from non-host senderPeerSlot=%d -- dropping",
                msg.senderPeerSlot);
        return true;
    }
    if (describedSlot == coop::players::Registry::Get().LocalPeerId()) return true;  // our own echo
    coop::nick_color::StoreForSlot(describedSlot, packed);
    return true;
}

}  // namespace coop::player_handshake
