// coop/session/player_handshake_prefs.cpp -- the live display-pref change family of the
// player_handshake module: SkinChange, NameplateChange and NickColorChange. One shared shape
// per family member: AnnounceLocal* for the local player's mid-session change (client to host,
// host to all), plus Handle* -- on the host a forgery guard, then a changed value applied (stored
// and re-sent to the other clients) at the kind's pace, the newest kept past it and applied by
// TickPrefs; on a client a host-only sender and a store. The AT-JOIN state of each pref rides
// the Join and PlayerJoined payloads built by player_handshake.cpp; this file is the
// LIVE-CHANGE wire only.
//
// The shared internals -- the skin side-table store and the skin field parser -- come through
// player_handshake_detail.h, and the public API stays in
// include/coop/session/player_handshake.h: one namespace, two translation units.

#include "coop/session/player_handshake.h"

#include "player_handshake_detail.h"  // co-located private header (src tree, not include/)

#include "coop/comms/chat_feed.h"
#include "coop/net/latest_wins.h"
#include "coop/net/net_clock.h"
#include "coop/net/refusal_tally.h"
#include "coop/net/session.h"
#include "coop/player/nameplate.h"
#include "coop/player/nick_color.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"
#include "coop/player/skin_registry.h"
#include "l10n/l10n.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::player_handshake {

namespace {

// [u8 slot][u8 len][name ASCII] -- the SkinChange wire form; the host re-emits the same form
// after its forgery guard.
std::vector<uint8_t> BuildSkinChangePayload(uint8_t slot, const std::string& name) {
    std::vector<uint8_t> out;
    const uint8_t len = static_cast<uint8_t>(name.size() > 48 ? 48 : name.size());
    out.reserve(2 + len);
    out.push_back(slot);
    out.push_back(len);
    out.insert(out.end(), name.begin(), name.begin() + len);
    return out;
}

// [u8 slot][u8 visible] -- the NameplateChange wire form; the host re-emits it after its guard.
std::vector<uint8_t> BuildNameplateChangePayload(uint8_t slot, bool visible) {
    return { slot, static_cast<uint8_t>(visible ? 1 : 0) };
}

// [u8 slot][u8 has][u8 r][u8 g][u8 b] -- the NickColorChange wire form; the host re-emits it
// after its guard.
std::vector<uint8_t> BuildNickColorChangePayload(uint8_t slot, uint32_t packed) {
    return { slot, static_cast<uint8_t>(coop::nick_color::IsCustom(packed) ? 1 : 0),
             coop::nick_color::R(packed), coop::nick_color::G(packed),
             coop::nick_color::B(packed) };
}

// The "<nick> changed skin to <skin>" line, History: something a PEER did, the same family as a
// peer-action line. The skin is its registry token, shown as the name it is.
void PushSkinChangedLine(uint8_t describedSlot, const std::string& skin) {
    const std::string nickUtf8 = coop::chat_feed::ToUtf8(NicknameForSlot(describedSlot));
    char line[256];
    if (l10n::Fmt(line, sizeof(line), l10n::T("%1$s changed skin to %2$s"), nickUtf8.c_str(),
                  skin.c_str()) >= 0)
        coop::chat_feed::Push(std::string(line), coop::chat_feed::Keep::History);
}

// Host: the changes of each kind applied for a slot's occupant (PrefApplies); [0] skin, [1] plate, [2] colour.
coop::roster_ledger::PerSlotState<std::array<uint32_t, 3>> g_prefApplies;

// Host: re-send a slot's change to every other ready client, the originator excluded.
void ResendToOthers(net::Session& session, uint8_t slot, net::ReliableKind kind,
                    const std::vector<uint8_t>& p) {
    for (int x = 1; x < net::kMaxPeers; ++x) {
        if (x == slot) continue;
        if (!session.IsSlotReady(x)) continue;
        session.SendReliableToSlot(x, kind, p.data(), static_cast<int>(p.size()));
    }
}

// Host: one client's change applied -- stored, its line said where it has one, and re-sent to the others.
void ApplySkinChange(net::Session& session, uint8_t slot, const std::string& name) {
    StoreSkinForSlot(slot, name);
    PushSkinChangedLine(slot, name);
    ResendToOthers(session, slot, net::ReliableKind::SkinChange, BuildSkinChangePayload(slot, name));
    ++g_prefApplies[slot][0];
}

void ApplyNameplateChange(net::Session& session, uint8_t slot, bool visible) {
    coop::nameplate::StoreVisibleForSlot(slot, visible);
    ResendToOthers(session, slot, net::ReliableKind::NameplateChange,
                   BuildNameplateChangePayload(slot, visible));
    ++g_prefApplies[slot][1];
}

void ApplyNickColorChange(net::Session& session, uint8_t slot, uint32_t packed) {
    coop::nick_color::StoreForSlot(slot, packed);
    ResendToOthers(session, slot, net::ReliableKind::NickColorChange,
                   BuildNickColorChangePayload(slot, packed));
    ++g_prefApplies[slot][2];
}

// The budgets, each above the most an honest player sent in the archived logs. A skin's application puts a persistent
// line on every peer, so it borrows two of Source's chat numbers: tier 1's bucket size, 4, and tier 2's pace, one per
// 10 s (player.cpp:98-101). Source passes a line only while a tier stays above 1 after its spend (two from a full
// bucket) and adds a 0.66 s interval; here the bucket's four pass, and the newest skin is kept where Source drops.
// A plate's or a colour's application is a store and two sends: four at once, then one a second.
constexpr net::IntentBudget kSkinBudget{4.0f, 0.1f};
constexpr net::IntentBudget kPrefBudget{4.0f, 1.0f};

// Host: per slot and kind, the newest changed value and its pace; one struct, so both clear with the occupant.
struct PendingSkin  { std::string value; net::LatestWins pace; };
struct PendingPlate { bool value = false; net::LatestWins pace; };
struct PendingColor { uint32_t value = 0; net::LatestWins pace; };
coop::roster_ledger::PerSlotState<PendingSkin>  g_pendingSkin;
coop::roster_ledger::PerSlotState<PendingPlate> g_pendingPlate;
coop::roster_ledger::PerSlotState<PendingColor> g_pendingColor;

// The family's refusals, said at the first after a quiet spell and summed per window (coop/net/refusal_tally.h), so a
// sender at the edge's rate cannot make the log the cost.
net::RefusalTally g_refusals{"player_handshake prefs"};

void Refuse(int sender, const char* why) {
    g_refusals.Note(static_cast<uint8_t>(sender), why, net::RefusalTally::Clock::now());
}

}  // namespace

void TickPrefs(net::Session& session) {
    g_refusals.Flush(net::RefusalTally::Clock::now());
    if (session.role() != net::Role::Host) return;
    uint64_t now = 0;  // read once, and only when a value is owed
    for (int slot = 1; slot < net::kMaxPeers; ++slot) {
        PendingSkin& skin = g_pendingSkin[slot];
        PendingPlate& plate = g_pendingPlate[slot];
        PendingColor& color = g_pendingColor[slot];
        if (!skin.pace.Owed() && !plate.pace.Owed() && !color.pace.Owed()) continue;
        if (now == 0) now = net::NowMs();
        const uint8_t s = static_cast<uint8_t>(slot);
        if (skin.pace.Due(kSkinBudget, now)) ApplySkinChange(session, s, skin.value);
        if (plate.pace.Due(kPrefBudget, now)) ApplyNameplateChange(session, s, plate.value);
        if (color.pace.Due(kPrefBudget, now)) ApplyNickColorChange(session, s, color.value);
    }
}

void ClearPrefRefusals() { g_refusals.Clear(); }

void WithdrawOwedPrefs(int slot) {
    if (slot < 1 || slot >= net::kMaxPeers) return;
    g_pendingSkin[slot].pace.Withdraw();
    g_pendingPlate[slot].pace.Withdraw();
    g_pendingColor[slot].pace.Withdraw();
}

uint32_t PrefApplies(int slot, net::ReliableKind kind) {
    if (slot < 0 || slot >= net::kMaxPeers) return 0;
    switch (kind) {
    case net::ReliableKind::SkinChange:      return g_prefApplies[slot][0];
    case net::ReliableKind::NameplateChange: return g_prefApplies[slot][1];
    case net::ReliableKind::NickColorChange: return g_prefApplies[slot][2];
    default:                                 return 0;
    }
}

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
    UE_ASSERT_GAME_THREAD("ledger row skin (HandleSkinChange)");
    if (msg.payloadLen < 2) {
        Refuse(msg.senderPeerSlot, "a SkinChange too short");
        return true;
    }
    const uint8_t describedSlot = msg.payload[0];
    std::string name;
    if (ParseSkinField(msg.payload + 1, static_cast<size_t>(msg.payloadLen) - 1, &name) == 0 ||
        name.empty()) {
        Refuse(msg.senderPeerSlot, "a SkinChange whose name is malformed or not a skin name");
        return true;
    }
    if (describedSlot >= net::kMaxPeers) return true;

    if (session.role() == net::Role::Host) {
        // Forgery guard: a client may only change ITS OWN skin.
        if (msg.senderPeerSlot != describedSlot || describedSlot == 0) {
            Refuse(msg.senderPeerSlot, "a SkinChange for a slot not its own");
            return true;
        }
        // The newest value kept, applied at the budget's pace (coop/net/latest_wins.h); an unchanged one costs a compare.
        PendingSkin& pend = g_pendingSkin[describedSlot];
        if (name == coop::roster_ledger::Get(describedSlot).skin) {
            pend.pace.Withdraw();
            return true;
        }
        pend.value = name;
        if (pend.pace.Offer(kSkinBudget, net::NowMs())) ApplySkinChange(session, describedSlot, name);
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
    PushSkinChangedLine(describedSlot, name);
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
        Refuse(msg.senderPeerSlot, "a NameplateChange too short");
        return true;
    }
    const uint8_t describedSlot = msg.payload[0];
    const bool visible = msg.payload[1] != 0;
    if (describedSlot >= net::kMaxPeers) return true;

    if (session.role() == net::Role::Host) {
        // Forgery guard: a client may only toggle ITS OWN plate.
        if (msg.senderPeerSlot != describedSlot || describedSlot == 0) {
            Refuse(msg.senderPeerSlot, "a NameplateChange for a slot not its own");
            return true;
        }
        PendingPlate& pend = g_pendingPlate[describedSlot];
        if (visible == coop::nameplate::VisibleForSlot(describedSlot)) {
            pend.pace.Withdraw();
            return true;
        }
        pend.value = visible;
        if (pend.pace.Offer(kPrefBudget, net::NowMs())) ApplyNameplateChange(session, describedSlot, visible);
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
        Refuse(msg.senderPeerSlot, "a NickColorChange too short");
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
            Refuse(msg.senderPeerSlot, "a NickColorChange for a slot not its own");
            return true;
        }
        PendingColor& pend = g_pendingColor[describedSlot];
        if (packed == coop::nick_color::PackedForSlot(describedSlot)) {
            pend.pace.Withdraw();
            return true;
        }
        pend.value = packed;
        if (pend.pace.Offer(kPrefBudget, net::NowMs())) ApplyNickColorChange(session, describedSlot, packed);
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
