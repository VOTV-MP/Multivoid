// coop/player_handshake.cpp -- the Join handshake: what a peer sends about itself (its element id,
// nickname, skin, display prefs, nick colour and game target), how the receiver writes it into
// the roster ledger, the host's roster relay, the connect and joined lines, and the slot
// assignment. See coop/player_handshake.h.

#include "coop/session/player_handshake.h"

#include "player_handshake_detail.h"  // co-located private header (src tree, not include/)

#include "coop/config/config.h"           // persist a host-assigned name
#include "coop/config/config_registry.h"  // the my-name default
#include "coop/text/repertoire.h"
#include "coop/text/utf8_codec.h"
#include "coop/session/session_manager.h"
#include "coop/moderation/seen_players.h"

#include "coop/element/player.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/local_body.h"
#include "coop/player/nameplate.h"
#include "coop/player/hand_item.h"  // hand-item mirrors: reset and slot disconnect
#include "coop/player/nick_color.h"
#include "coop/player/nickname_arbiter.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/player/skin_registry.h"
#include "coop/version.h"                // kGameTarget, the Join's game field
#include "ue_wrap/core/hot_path_guard.h"
#include "coop/comms/chat_bubbles.h"
#include "coop/comms/chat_feed.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace coop::player_handshake {

namespace {

// Whether our Join has gone out over a link, per slot. Not a ledger row field: a client sends its
// Join to slot 0 before row 0 exists (the row is born from the host's own Join), and a row setter
// ignores an unoccupied slot, so the client would re-send every tick forever. PerSlotState clears
// itself when the occupant changes. The person's own fields (nick, guid, skin, joinAnnounced)
// live on the row and die with it.
coop::roster_ledger::PerSlotState<bool> g_joinSent;

// The connect line already shown for this slot, a one-shot latch outside the row for the same
// reason. The Join legitimately arrives twice (a joiner with no element yet sends the 0 sentinel
// and retries with a real eid), and the roster relay skips the first arrival; without this latch
// the line printed on both.
coop::roster_ledger::PerSlotState<bool> g_connectAnnounced;

}  // namespace

// Stores the skin and applies it now if the slot's puppet is already spawned (a mid-session
// SkinChange, or a Join that raced the first pose). External linkage: HandleSkinChange lives in
// player_handshake_prefs.cpp.
void StoreSkinForSlot(int slot, std::string name) {
    if (slot < 0 || slot >= net::kMaxPeers) return;
    if (coop::roster_ledger::Get(slot).skin == name) return;
    coop::roster_ledger::SetSkin(slot, std::move(name));
    if (RemotePlayer* p = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot)))
        p->ApplySkin(coop::roster_ledger::Get(slot).skin);
}

void TickSkinConverge() {
    // Throttled to one pass per 2 s; the call site stays bare.
    static uint64_t sLastMs = 0;
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (now - sLastMs < 2000) return;
    sLastMs = now;
    auto& reg = coop::players::Registry::Get();
    for (int slot = 0; slot < net::kMaxPeers; ++slot) {
        RemotePlayer* p = reg.Puppet(static_cast<uint8_t>(slot));
        if (!p) continue;
        const std::string& skin = coop::roster_ledger::Get(slot).skin;
        if (!skin.empty()) p->ApplySkin(skin);  // early-out when already applied
    }
}

// One [u8 len][ASCII] field; returns the bytes consumed (0 if malformed or absent), and writes
// `out` only for a well-formed non-empty name that validates as a skin.
size_t ParseSkinField(const uint8_t* p, size_t remaining, std::string* out) {
    if (remaining < 1) return 0;
    const int len = p[0];
    if (1 + len > static_cast<int>(remaining)) return 0;
    if (len > 0) {
        std::string name(reinterpret_cast<const char*>(p + 1), static_cast<size_t>(len));
        // The name becomes a LoadObject package path component.
        if (coop::skins::IsValidSkinName(name))
            *out = std::move(name);
        else
            UE_LOGW("handshake: peer sent an invalid skin name (%d bytes) -- ignored", len);
    }
    return 1 + static_cast<size_t>(len);
}

namespace {

// The display-prefs flags byte (after the skin field in Join and PlayerJoined): the bit layout is
// the wire contract, extended with new bits, never re-ordered.
constexpr uint8_t kPrefNameplateVisible = 0x01;

uint8_t BuildLocalPrefsFlags() {
    uint8_t f = 0;
    if (coop::nameplate::LocalVisible()) f |= kPrefNameplateVisible;
    return f;
}

}  // namespace: the local prefs byte; everything below is shared through player_handshake_detail.h
   //            shared with the sibling TUs via player_handshake_detail.h]

uint8_t PrefsFlagsForSlot(int slot) {
    uint8_t f = 0;
    if (coop::nameplate::VisibleForSlot(slot)) f |= kPrefNameplateVisible;
    return f;
}

void StorePrefsFlagsForSlot(int slot, uint8_t flags) {
    coop::nameplate::StoreVisibleForSlot(slot, (flags & kPrefNameplateVisible) != 0);
}

// The nick colour field, [u8 has][u8 r][u8 g][u8 b] after the prefs byte in Join and RosterRow;
// has=0 leaves the three bytes present but ignored (a fixed 4-byte field).
void AppendNickColorField(std::vector<uint8_t>& out, uint32_t packed) {
    out.push_back(coop::nick_color::IsCustom(packed) ? 1 : 0);
    out.push_back(coop::nick_color::R(packed));
    out.push_back(coop::nick_color::G(packed));
    out.push_back(coop::nick_color::B(packed));
}

// The 4-byte colour field at `p`, stored for `slot`; returns the bytes consumed (0 if absent or
// truncated, the slot's colour untouched).
size_t ParseNickColorField(const uint8_t* p, size_t remaining, int slot) {
    if (remaining < 4) return 0;
    coop::nick_color::StoreForSlot(
        slot, p[0] != 0 ? coop::nick_color::Pack(p[1], p[2], p[3]) : 0u);
    return 4;
}

// The encoding adapters, SanitizeNickname, the local-name stores and the request-and-adopt policy
// live in player_handshake_nick.cpp, declared in player_handshake_detail.h.



const std::string& GuidForSlot(int slot) {
    return coop::roster_ledger::Get(slot).guid;  // GT-asserted inside the ledger
}

const std::string& SkinForSlot(int slot) {
    return coop::roster_ledger::Get(slot).skin;  // GT-asserted inside the ledger
}

void Reset() {
    // The per-slot identity state is the ledger's; clearing a row fires the transition that drives
    // every registered teardown. The load-bearing clear is ClearAll at session stop.
    coop::roster_ledger::Reset();
    coop::hand_item::Reset();  // destroy the hand-item display mirrors and states
}

void MaybeSendJoinToSlot(net::Session& session, int slot,
                         std::vector<uint8_t>& joinPayload,
                         bool& joinPayloadBuilt) {
    // From the net-pump connect edge each tick. The boot-thread writer SetLocalNickname runs once
    // before the pump sends any Join.
    UE_ASSERT_GAME_THREAD("g_localNick/g_joinSent (MaybeSendJoinToSlot)");
    if (slot < 0 || slot >= net::kMaxPeers) return;
    if (g_joinSent[slot]) return;
    // The first Join waits for our own Player element: sent with senderElementId 0 the receiver
    // installs no mirror, and Registry::Get(senderElementId) would never resolve for the session. A
    // client allocates one pump tick after AssignPeerSlot; the host has its element at pump start.
    const coop::element::ElementId selfEidProbe =
        coop::players::Registry::Get().LocalPlayerElementId();
    if (selfEidProbe == coop::element::kInvalidId) {
        return;  // retry next tick
    }
    if (!joinPayloadBuilt) {
        // The prefix: [uint32 senderElementId], then [uint8 nicklen][nick UTF-8]. The receiver
        // registers the id as a mirror in the sender's slot, so packets carrying it resolve through
        // Registry::Get.
        const uint32_t selfEidWire = selfEidProbe;
        joinPayload.resize(4);  // not-name-text: a 4-byte wire field
        std::memcpy(joinPayload.data(), &selfEidWire, 4);
        // What the human typed, never a suffix the host handed us last time, or a reconnect would
        // ratchet the number up. Capped on a character boundary: a raw resize manufactures the
        // ill-formed tail the receive boundary refuses, and our own name would reach the host as
        // the placeholder.
        const std::string nickStr = coop::text::CapUtf8Bytes(
            coop::text::ToUtf8(RequestedNickname()), coop::text::kNickMaxBytes);
        std::vector<uint8_t> nickUtf8(nickStr.begin(), nickStr.end());
        UE_LOGI("handshake: Join to slot %d asks for '%ls' (%zu bytes)",
                slot, RequestedNickname().c_str(), nickUtf8.size());
        joinPayload.push_back(static_cast<uint8_t>(nickUtf8.size()));
        joinPayload.insert(joinPayload.end(), nickUtf8.begin(), nickUtf8.end());
        // No guid on the wire: the host keyed a peer's inventory file by a guid the peer named
        // itself, which the receive boundary could only check the shape of. The host derives it
        // from the public key the peer proved at admission (Session::ProvedGuidForSlot); with no
        // reader left the field cannot come back. The skin follows the nick as [uint8 skinlen][skin
        // ASCII], the at-join announce of local_body's choice (48 chars at most).
        const std::string& skin = coop::local_body::LocalSkinName();
        const uint8_t skinLen = static_cast<uint8_t>(skin.size() > 48 ? 48 : skin.size());
        joinPayload.push_back(skinLen);
        joinPayload.insert(joinPayload.end(), skin.begin(), skin.begin() + skinLen);
        // The prefs flags after the skin (bit 0 = nameplate visible), so a late joiner agrees with
        // a peer that hid its plate before they arrived.
        joinPayload.push_back(BuildLocalPrefsFlags());
        // The nick colour after the flags byte.
        AppendNickColorField(joinPayload, coop::nick_color::LocalPacked());
        // The game target, [u8 gamelen][game ASCII] after the colour: the other half of the version
        // pair (the build number rides the packet header as the protocol version). The receiver
        // checks it by byte equality at the top of HandleJoinMessage, the wire-level gate that also
        // covers direct connect and env boot, where no browser pre-flight ran. At most 23 chars.
        {
            const char* game = coop::version::kGameTarget;
            const size_t gameLen = std::min<size_t>(std::strlen(game), 23);
            joinPayload.push_back(static_cast<uint8_t>(gameLen));
            joinPayload.insert(joinPayload.end(), game, game + gameLen);
        }
        joinPayloadBuilt = true;
    }
    if (session.SendReliableToSlot(slot, net::ReliableKind::Join,
                                   joinPayload.data(),
                                   static_cast<int>(joinPayload.size()))) {
        g_joinSent[slot] = true;
    }
    // A failed send is retried next tick.
}

namespace {

// The person-state teardown, driven by the ledger's row transition rather than a disconnect
// callback: it fires on a replacement too (a recycled slot goes from one peer to the next with
// no absence between, and the next would inherit the skin, colour, plate and bubble), and it
// receives snapshots, so it no longer depends on running before the nickname is cleared.
void OnSlotReplaced_TearDownPerson(int slot, const coop::roster_ledger::Row& outgoing,
                                   const coop::roster_ledger::Row& /*incoming*/) {
    if (!outgoing.occupied()) return;  // nothing was there; nothing to tear down
    coop::nameplate::OnSlotDisconnected(slot);   // plate pref back to visible
    coop::nick_color::OnSlotDisconnected(slot);  // nick colour back to default
    coop::chat_bubbles::OnSlotDisconnected(slot);  // no inherited bubble
    coop::hand_item::OnSlotDisconnected(static_cast<uint8_t>(slot));  // drop the hand mirror
}

}  // namespace

void InstallLedgerSubscribers() {
    // Idempotent inside the ledger (deduped by function pointer), so a Stop and Start in one
    // process cannot double-fire every teardown.
    coop::roster_ledger::SubscribeSlotReplaced(&OnSlotReplaced_TearDownPerson);
    InstallRosterPulseSubscriber();  // player_handshake_roster.cpp
}


bool IsValidGuid(const std::string& guid) {
    if (guid.size() != 32) return false;
    for (char c : guid) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

bool HandleJoinMessage(net::Session& session,
                       const net::Session::ReliableMessage& msg) {
    // Writes the sender's ledger row. Dispatched from event_feed::Update on the game thread, which
    // reconciles the ledger before draining reliables, so on the host the sender's row exists by
    // the time its Join lands (the Join arrives over a configured lane, the reconcile's birth
    // condition).
    UE_ASSERT_GAME_THREAD("ledger row (HandleJoinMessage)");
    const int senderSlot = msg.senderPeerSlot;
    if (senderSlot < 0 || senderSlot >= net::kMaxPeers) {
        UE_LOGW("player_handshake: Join has invalid senderPeerSlot=%d -- dropping",
                senderSlot);
        return true;
    }
    // An undersized Join is dropped rather than degraded to no-mirror routing, which would leave
    // the peer without the stale-generation defence for the session; the sender's retry produces a
    // well-formed one. The header's protocol version keeps an older layout from misaligning here.
    if (msg.payloadLen < 4) {
        UE_LOGW("player_handshake: Join payload %zu B too short for v16 prefix "
                "(senderSlot=%d) -- dropping",
                static_cast<size_t>(msg.payloadLen), senderSlot);
        return true;
    }
    // The wire version gate first, before any identity side effect (player_handshake_version.cpp):
    // byte equality on the peer's game target (the build number is the header's protocol version,
    // equal by construction); a mismatch or a malformed chain refuses and closes (a host Kick with
    // a feed line, a client Fail popup). A refused joiner is the already-handled "connected, never
    // Joined, disconnected" lifecycle.
    if (ValidateJoinVersionOrRefuse(session, senderSlot, msg.payload, msg.payloadLen))
        return true;
    // On a client this Join is the host introducing itself, and row 0 is born here: the host's
    // player number is a role constant, so no wire field and no dependence on the roster pulse. On
    // the host the sender's row was born by the reconcile.
    if (session.role() == net::Role::Client && senderSlot == 0) {
        coop::roster_ledger::InstallRow(0, coop::roster_ledger::kHostPlayerNo,
                                        /*bornGeneration=*/0);
    }
    uint32_t senderElementId = 0;
    std::memcpy(&senderElementId, msg.payload, 4);
    const uint8_t* nickStart = msg.payload + 4;
    size_t nickRemaining = msg.payloadLen - 4;
    std::wstring nick = coop::roster_ledger::Get(senderSlot).nick;
    size_t nickFieldLen = 0;  // bytes the nick field [u8 len][bytes] occupies (0 if malformed/absent)
    if (nickRemaining > 0) {
        const int len = nickStart[0];
        if (1 + len <= static_cast<int>(nickRemaining)) {
            nickFieldLen = 1 + static_cast<size_t>(len);
            if (len > 0) nick = FromUtf8(nickStart + 1, len);
        }
    }
    // The inventory identity is hex(SHA-256(pubkey)[0..16]) of the key the peer proved at
    // admission, published by the net thread and read here on the game thread. The 32-hex-chars
    // validation it used to need (the value is a host filesystem path component under
    // coop_players/) is structural now, and no peer can name a row that is not its own. Only the
    // host can answer; a client's store is empty.
    if (session.role() == coop::net::Role::Host) {
        const std::string proved = session.ProvedGuidForSlot(senderSlot);
        if (IsValidGuid(proved)) {
            coop::roster_ledger::SetGuid(senderSlot, proved);
        } else {
            // Unreachable on the admission path (a seat is spent only after a verified proof); if
            // it fires, the exchange changed shape and this peer's stored inventory would read as a
            // new player's.
            UE_LOGW("handshake: slot %d has no PROVED identity guid -- its stored "
                    "inventory cannot be found (admission changed shape?)", senderSlot);
        }
    }
    // The skin follows the nick. Tolerated absent: a malformed field leaves the slot's skin empty
    // (the native body) until a SkinChange lands.
    size_t skinFieldLen = 0;
    if (nickFieldLen > 0 && nickFieldLen < nickRemaining) {
        std::string skin;
        skinFieldLen = ParseSkinField(nickStart + nickFieldLen,
                                      nickRemaining - nickFieldLen, &skin);
        if (skinFieldLen > 0 && !skin.empty()) {
            StoreSkinForSlot(senderSlot, std::move(skin));
        }
    }
    // The prefs flags follow the skin; absent, the defaults (visible) stand.
    if (skinFieldLen > 0 && nickFieldLen + skinFieldLen < nickRemaining) {
        StorePrefsFlagsForSlot(senderSlot, nickStart[nickFieldLen + skinFieldLen]);
        // The nick colour follows the flags byte.
        const size_t colorOff = nickFieldLen + skinFieldLen + 1;
        if (colorOff < nickRemaining)
            ParseNickColorField(nickStart + colorOff, nickRemaining - colorOff,
                                senderSlot);
    }
    // A mirror Player element for this sender, so packets carrying its senderElementId resolve
    // here; 0 means no element yet, and routing falls back to the sender slot. Range-validated
    // against the sender's role first: a host must send a host-range eid and a client a peer-range
    // one, and a mismatch (a forged Join, a relay loop) drops the mirror install rather than
    // corrupt the per-slot mapping; the nickname still displays.
    if (senderElementId != 0u &&
        senderElementId != coop::element::kInvalidId) {
        const bool senderIsHost = (senderSlot == 0);
        if (!coop::element::Registry::IsAllowedSenderEid(
                senderIsHost, senderElementId)) {
            UE_LOGW("player_handshake: Join senderElementId=0x%08x out of "
                    "allowed %s range (senderSlot=%d) -- dropping mirror "
                    "install; nickname will still display",
                    senderElementId,
                    senderIsHost ? "host" : "peer",
                    senderSlot);
        } else {
            coop::players::Registry::Get().EstablishMirrorForSlot(
                static_cast<uint8_t>(senderSlot), senderElementId);
        }
    }
    // The nickname sanitiser at the trust boundary: the string came from a peer and lands in the
    // nameplate and the chat feed, where a newline or a right-to-left override would inject into
    // the widget text; the length cap bounds the widget overflow.
    nick = SanitizeNickname(nick);
    // The host is the canonical namer: two clients typing the same name cannot see each other's
    // choice, so uniqueness is decided by the one peer that sees every name, and the assignment
    // reaches everyone on the RosterRow, whose nick sits in the fixed prefix above the
    // applyDeclared gate for that reason. Unconditional: Assign is the identity when nothing
    // collides, so a name takes one path to a row.
    if (session.role() == net::Role::Host)
        nick = coop::nickname_arbiter::Assign(senderSlot, nick);
    coop::roster_ledger::SetNick(senderSlot, nick);
    // The nameplate of this sender's puppet.
    if (RemotePlayer* p = coop::players::Registry::Get().Puppet(
            static_cast<uint8_t>(senderSlot))) {
        p->SetNickname(nick);
    }
    // Two phases, phrased by role: the Join means the peer is connecting (it has not loaded the
    // world or spawned), so "connecting" here and "joined the game" later, when its puppet spawns
    // (AnnouncePeerSpawned); on the client the Join comes from the host, so "Connecting to <host>'s
    // game". Both transient: the joined line is the event history keeps. Once per join, latched
    // (g_connectAnnounced clears with the slot's occupant, so a peer that leaves and rejoins is
    // announced again).
    if (g_connectAnnounced[senderSlot]) {
        UE_LOGI("player_handshake: slot %d connect line already shown -- suppressing the "
                "repeat from a re-sent Join", senderSlot);
    } else {
        g_connectAnnounced[senderSlot] = true;
        if (session.role() == net::Role::Client) {
            coop::chat_feed::Push(L"Connecting to " + nick + L"'s game...",
                                  coop::chat_feed::Keep::Transient);
        } else {
            coop::chat_feed::Push(nick + L" is connecting to the game...",
                                  coop::chat_feed::Keep::Transient);
        }
        UE_LOGI("player_handshake: slot %d connect line shown ('%ls')", senderSlot, nick.c_str());
    }
    // On the host this Join came from a client: the two-way roster broadcast (MTA's initial data
    // stream) tells every client about the joiner and the joiner about every client. Skipped for
    // the 0 sentinel; the retry Join with a real eid triggers it.
    if (session.role() == net::Role::Host &&
        senderElementId != 0u &&
        senderElementId != coop::element::kInvalidId) {
        BroadcastRosterFromHost(session, senderSlot, senderElementId, nick);
    }
    // The seen-players registry records the peer's durable identity (guid, nick, IP, last seen) on
    // the host, after the guid and nick stores so it reads the landed values.
    if (session.role() == net::Role::Host)
        coop::seen_players::TouchOnJoin(session, senderSlot);
    return true;
}

namespace {

// The one door for "<nick> joined the game", latched once per join (cleared on slot disconnect),
// so the puppet-spawn and world-ready seams can both call it in either order without a repeat and
// a mid-session respawn stays silent. Pushed immediately: the line's point is to coincide with the
// body appearing.
void AnnounceJoinerOnce(int slot) {
    UE_ASSERT_GAME_THREAD("ledger row joinAnnounced (AnnounceJoinerOnce)");
    if (slot < 1 || slot >= net::kMaxPeers) return;  // slot 0 = host self; never "joins"
    if (coop::roster_ledger::Get(slot).joinAnnounced) return;
    coop::roster_ledger::SetJoinAnnounced(slot, true);
    coop::chat_feed::Push(NicknameForSlot(static_cast<uint8_t>(slot)) + L" joined the game",
                          coop::chat_feed::Keep::History);
    UE_LOGI("player_handshake: slot %d joined the game (announced at puppet appearance)", slot);
}

}  // namespace

void AnnouncePeerSpawned(net::Role role, int slot) {
    // From net_pump the moment a remote peer's puppet spawns, the visible appearance (a world-ready
    // plus 5 s announce ran about 6 s before the puppet in a measured join). On the host net_pump
    // gates the call on IsSlotWorldReady, so a pre-world menu pose cannot spawn the puppet early;
    // in that order OnClientWorldReady announces instead.
    if (role == net::Role::Client && slot == 0) {
        // The self-join line, delayed 5 s: our own loading screen still covers the world at the
        // spawn moment. Transient.
        coop::chat_feed::PushDelayed(L"Joined " + NicknameForSlot(0) + L"'s game", 5000,
                                     coop::chat_feed::Keep::Transient);
        return;
    }
    AnnounceJoinerOnce(slot);
}

void OnClientWorldReady(int slot) {
    // The host's reverse-order cover: a puppet that spawned before its world-ready (a pre-world
    // loading pose) skipped the spawn-seam announce, so world-ready announces it; in the normal
    // order this is a no-op.
    if (slot < 1 || slot >= net::kMaxPeers) return;
    if (coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot)) != nullptr)
        AnnounceJoinerOnce(slot);
}

bool HandleAssignPeerSlot(net::Session& session,
                          const net::Session::ReliableMessage& msg) {
    // The host's slot assignment. Without it a client would self-stamp slot 1, and a second client
    // with the same local id would drop the first's ItemActivate as its own loopback.
    if (msg.payloadLen < sizeof(net::AssignPeerSlotPayload)) {
        UE_LOGW("player_handshake: AssignPeerSlot payload too short (%zu < %zu)",
                static_cast<size_t>(msg.payloadLen), sizeof(net::AssignPeerSlotPayload));
        return true;
    }
    net::AssignPeerSlotPayload p{};
    std::memcpy(&p, msg.payload, sizeof(p));
    // Only the host sends this; rejected on the host.
    if (session.role() == net::Role::Host) {
        UE_LOGW("player_handshake: AssignPeerSlot received on host -- dropping "
                "(host self-assigns slot 0; no inbound from client)");
        return true;
    }
    // The sender must be the host connection, independent of the relay topology.
    if (msg.senderPeerSlot != 0) {
        UE_LOGW("player_handshake: AssignPeerSlot from non-host "
                "senderPeerSlot=%d -- dropping",
                msg.senderPeerSlot);
        return true;
    }
    // A valid client slot; slot 0 is the host's own.
    if (p.slot < 1 || p.slot >= net::kMaxPeers) {
        UE_LOGW("player_handshake: AssignPeerSlot slot=%u out of range [1..%u) -- dropping",
                p.slot, static_cast<unsigned>(net::kMaxPeers));
        return true;
    }
    coop::players::Registry::Get().SetLocalPeerId(p.slot);
    UE_LOGI("player_handshake: host assigned us peer slot %u (Registry::LocalPeerId now %u)",
            p.slot, coop::players::Registry::Get().LocalPeerId());
    // Roster rows that arrived before this stamp were parked, since a row about our own slot was
    // indistinguishable from one about a remote peer; applied in arrival order now.
    OnLocalPeerIdStamped(session);
    // The host's Player element id, if included, installs a mirror in slot 0 so the host's packets
    // resolve here; 0 or invalid means the host had no element yet, and the receivers route by
    // slot.
    if (p.hostElementId != 0u &&
        p.hostElementId != coop::element::kInvalidId) {
        // Through the one validator every receiver uses.
        if (!coop::element::Registry::IsAllowedHostAllocatedEid(p.hostElementId)) {
            UE_LOGW("player_handshake: AssignPeerSlot hostElementId=0x%08x is "
                    "not in host range -- dropping mirror install",
                    p.hostElementId);
        } else {
            coop::players::Registry::Get().EstablishMirrorForSlot(
                coop::players::kPeerIdHost, p.hostElementId);
        }
    } else {
        UE_LOGI("player_handshake: AssignPeerSlot host had no Element id yet "
                "(boot/seed race) -- routing will use senderPeerSlot");
    }
    return true;
}

// The live display-pref changes (SkinChange, NameplateChange, NickColorChange) live in
// player_handshake_prefs.cpp; the shared internals in player_handshake_detail.h.

}  // namespace coop::player_handshake
