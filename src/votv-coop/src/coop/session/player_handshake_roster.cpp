// coop/session/player_handshake_roster.cpp -- the RosterRow wire family: the host asserting
// who occupies each peer slot, and a client conforming to it. A row is state, not an event:
// the host re-asserts rows on a repair pulse, so the receiver applies idempotently, with every
// store conditioned on a change and every edge effect (the mirror install, the skin, the
// prefs, the colour, the toast) gated on a change or an existing latch. It carries the
// occupant's session-unique playerNo, where 0 means the slot is empty; that is how a departure
// reaches a client, since there is no leave kind and a recycled slot goes from one person to
// the next with no absence between, so a receiver never observes absence and conforms to the
// current token. It may describe slot 0 (the host itself) and the receiver's own slot; those
// rows carry eid 0, so the receiver skips the mirror install for them, and slot 0's eid still
// arrives through AssignPeerSlot. For a row about slot 0 or about our own slot the declared
// fields (skin, prefs, colour) are not applied: they are peer-authored and merely relayed, and
// applying the host's cached copy of our own choice back onto us is an authority inversion.
// playerNo is always applied.

#include "coop/session/player_handshake.h"

#include "player_handshake_detail.h"

#include "coop/config/config.h"
#include "coop/element/player.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/nick_color.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/text/utf8_codec.h"
#include "coop/player/roster_ledger.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <array>
#include <cstring>
#include <vector>

namespace coop::player_handshake {
namespace {

// The minimum payload: slot, playerNo, eid, link kind, ping and an empty nick length. The
// connection facts widened the fixed prefix rather than the tail: the tail's offset arithmetic
// lives inside the declared block, which is skipped for exactly the host row and the
// receiver's own row, the two rows this lane exists to populate.
constexpr size_t kRosterRowMinLen = 1 + 2 + 4 + 1 + 2 + 1;
constexpr size_t kRosterRowPrefixLen = 1 + 2 + 4 + 1 + 2;  // where the nick field starts

// The drop-empty-rows dev flag; latched, since a fault injection switchable mid-session would
// make a failure unattributable.
bool DropEmptyRowsForTest() {
    static const bool s =
        coop::config::ResolveFlag(::coop::config_registry::rows::roster_drop_empty_rows);
    return s;
}

// A RosterRow payload describing peer `slot`, parsed field by field like the Join: slot u8,
// playerNo u16, eid u32, link kind u8, ping i16, a length-prefixed UTF-8 nick, a
// length-prefixed skin, the prefs flags, and the colour field.
std::vector<uint8_t> BuildRosterRowPayload(uint8_t slot, uint16_t playerNo, uint32_t eid,
                                           coop::net::LinkKind linkKind, int16_t pingMs,
                                           const std::wstring& nick,
                                           const std::string& skin,
                                           uint8_t prefsFlags) {
    std::vector<uint8_t> out;
    out.resize(kRosterRowPrefixLen);  // not-name-text: a wire row prefix, in bytes
    out[0] = slot;
    std::memcpy(out.data() + 1, &playerNo, 2);
    std::memcpy(out.data() + 3, &eid, 4);
    out[7] = static_cast<uint8_t>(linkKind);
    std::memcpy(out.data() + 8, &pingMs, 2);
    // Capped on a character boundary: a raw resize would ship an ill-formed tail the receiver's
    // strict decoder refuses whole, and the host's own row would arrive as a placeholder.
    const std::string nickStr = coop::text::CapUtf8Bytes(
        coop::text::ToUtf8(nick), coop::text::kNickMaxBytes);
    std::vector<uint8_t> nickUtf8(nickStr.begin(), nickStr.end());
    out.push_back(static_cast<uint8_t>(nickUtf8.size()));
    out.insert(out.end(), nickUtf8.begin(), nickUtf8.end());
    const uint8_t skinLen = static_cast<uint8_t>(skin.size() > 48 ? 48 : skin.size());
    out.push_back(skinLen);
    out.insert(out.end(), skin.begin(), skin.begin() + skinLen);
    out.push_back(prefsFlags);
    AppendNickColorField(out, coop::nick_color::PackedForSlot(slot));
    return out;
}

// The row describing `slot` as the host knows it. An empty slot still produces a row, with
// playerNo 0: that row is the only way a client learns of a departure.
std::vector<uint8_t> BuildRowForSlot(int slot) {
    const coop::roster_ledger::Row& row = coop::roster_ledger::Get(slot);
    if (!row.occupied())
        return BuildRosterRowPayload(static_cast<uint8_t>(slot), 0, 0,
                                     coop::net::LinkKind::Unknown, -1, L"", "", 0);

    // The eid is the peer's mirror Player Element on the host. A row about slot 0 carries the 0
    // sentinel: the host's own eid reaches a client through AssignPeerSlot, and two authors for
    // one identity is how the join-window races were born.
    uint32_t eid = 0;
    if (slot != 0) {
        coop::element::Player* el =
            coop::players::Registry::Get().GetPlayerElement(static_cast<uint8_t>(slot));
        if (el && el->IsMirror()) eid = el->GetId();
    }
    return BuildRosterRowPayload(static_cast<uint8_t>(slot), row.playerNo, eid,
                                 row.linkKind, row.pingMs,
                                 row.nick, row.skin, PrefsFlagsForSlot(slot));
}

void SendRowTo(net::Session& session, int toSlot, int describedSlot) {
    const std::vector<uint8_t> p = BuildRowForSlot(describedSlot);
    session.SendReliableToSlot(toSlot, net::ReliableKind::RosterRow,
                               p.data(), static_cast<int>(p.size()));
}

// The repair pulse. A reliable row can be lost at enqueue, silently, and the window where that
// happens is a joiner's first seconds, when the save-transfer burst fills the send buffer,
// which is also when a joiner opens the player list. So the host re-asserts the whole roster:
// fast while it matters, slow forever after. A row is about 40 bytes, so the pulse cannot
// cause congestion; it can only be a victim of it, which is what it heals.
constexpr uint64_t kPulseFastMs = 1000;   // first ~10 s after a roster change
constexpr uint64_t kPulseSlowMs = 5000;   // steady state
constexpr uint64_t kPulseFastWindowMs = 10000;

uint64_t g_lastPulseMs = 0;
uint64_t g_lastRosterChangeMs = 0;

// Parked inbound rows: a row about our own slot can arrive before AssignPeerSlot has stamped
// our id, and applying it then would misfile it as a row about a remote peer. At most one row
// per slot is held; the host sends one per slot.
struct ParkedRow {
    bool used = false;
    std::array<uint8_t, 256> bytes{};
    size_t len = 0;
};
std::array<ParkedRow, coop::players::kMaxPeers> g_parked{};

bool ApplyRosterRow(net::Session& session, const uint8_t* payload, size_t payloadLen);

void ParkRow(const uint8_t* payload, size_t payloadLen) {
    if (payloadLen > 256) return;  // cannot be one of ours; drop rather than truncate
    for (ParkedRow& p : g_parked) {
        if (p.used) continue;
        std::memcpy(p.bytes.data(), payload, payloadLen);
        p.len = payloadLen;
        p.used = true;
        return;
    }
    UE_LOGW("roster: parked-row buffer full -- dropping a row (the pulse re-sends it)");
}

}  // namespace

void OnLocalPeerIdStamped(net::Session& session) {
    // Called the moment AssignPeerSlot stamps our slot: everything parked is now unambiguous. The
    // ordering (AssignPeerSlot first, same lane) makes this rare, and the failure is silent
    // misfiling.
    for (ParkedRow& p : g_parked) {
        if (!p.used) continue;
        p.used = false;
        ApplyRosterRow(session, p.bytes.data(), p.len);
    }
}

void MarkRosterChanged() {
    g_lastRosterChangeMs = ::GetTickCount64();
    g_lastPulseMs = 0;  // re-assert on the very next tick, then settle into the cadence
}

namespace {
// Any occupancy change re-arms the fast pulse, so a departure reaches the other clients at
// once rather than up to five seconds later.
void OnSlotReplaced_ArmPulse(int, const coop::roster_ledger::Row&,
                             const coop::roster_ledger::Row&) {
    MarkRosterChanged();
}
}  // namespace

void InstallRosterPulseSubscriber() {
    coop::roster_ledger::SubscribeSlotReplaced(&OnSlotReplaced_ArmPulse);
}

void PulseRosterRows(net::Session& session) {
    UE_ASSERT_GAME_THREAD("roster pulse");
    if (session.role() != net::Role::Host) return;
    if (!session.running()) return;

    const uint64_t now = ::GetTickCount64();
    const bool fast = (now - g_lastRosterChangeMs) < kPulseFastWindowMs;
    const uint64_t period = fast ? kPulseFastMs : kPulseSlowMs;
    if (g_lastPulseMs != 0 && (now - g_lastPulseMs) < period) return;
    g_lastPulseMs = now;

    // The connection facts are conformed immediately before serialising, so the bytes are exactly
    // fresh and the fill has no clock of its own; it runs only when a pulse is due, which is why
    // the GNS lock it takes is not a hot-path cost. The slow period has two consumers: roster
    // repair and the freshness of every board's ping column.
    coop::roster_ledger::RefreshLinkFacts(session);

    // Every slot, empty ones and slot 0 included; skipping empties would mean absence never
    // heals.
    for (int to = 1; to < net::kMaxPeers; ++to) {
        if (!session.IsSlotReady(to)) continue;
        for (int described = 0; described < net::kMaxPeers; ++described)
            SendRowTo(session, to, described);
    }
}

void BroadcastRosterFromHost(net::Session& session, int joinerSlot,
                                   uint32_t joinerEid,
                                   const std::wstring& joinerNick) {
    if (session.role() != net::Role::Host) return;
    if (joinerSlot < 1 || joinerSlot >= net::kMaxPeers) return;
    (void)joinerEid; (void)joinerNick;  // read from the ledger row instead (one source)

    // The same rule as the pulse: the connection facts are filled immediately before these rows
    // serialise, so a joiner's first roster carries real values.
    coop::roster_ledger::RefreshLinkFacts(session);

    // MTA's initial data stream, two-way (the join broadcast in the vendored CGame.cpp): announce
    // the joiner to every other connected client, and tell the joiner about every slot the host
    // knows, including slot 0 and the joiner's own row, which is how it learns its player number.
    for (int x = 1; x < net::kMaxPeers; ++x) {
        if (x == joinerSlot) continue;
        if (!session.IsSlotReady(x)) continue;
        SendRowTo(session, x, joinerSlot);
    }
    for (int described = 0; described < net::kMaxPeers; ++described)
        SendRowTo(session, joinerSlot, described);

    MarkRosterChanged();  // arm the fast pulse over the joiner's first seconds
    UE_LOGI("roster: host asserted the full roster to joiner slot %d "
            "(and the joiner to %d other client(s))", joinerSlot,
            session.connectedPeerCount() - 1);
}

namespace {

// Apply one row, idempotently: every store is conditioned on a change and every edge effect
// on a change or a latch, because the pulse delivers the same row over and over.
bool ApplyRosterRow(net::Session& session, const uint8_t* payload, size_t payloadLen) {
    if (payloadLen < kRosterRowMinLen) {
        UE_LOGW("roster: row payload %zu B too short -- dropping", payloadLen);
        return true;
    }
    const uint8_t describedSlot = payload[0];
    uint16_t playerNo = 0;
    std::memcpy(&playerNo, payload + 1, 2);
    uint32_t describedEid = 0;
    std::memcpy(&describedEid, payload + 3, 4);
    const coop::net::LinkKind linkKind = coop::net::LinkKindFromWire(payload[7]);
    int16_t pingMs = -1;
    std::memcpy(&pingMs, payload + 8, 2);
    const uint8_t* nickStart = payload + kRosterRowPrefixLen;
    const size_t nickRemaining = payloadLen - kRosterRowPrefixLen;

    if (describedSlot >= net::kMaxPeers) {
        UE_LOGW("roster: row slot=%u out of range -- dropping",
                static_cast<unsigned>(describedSlot));
        return true;
    }

    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
    const bool aboutSelf = (localSlot != coop::players::kPeerIdUnknown &&
                            describedSlot == localSlot);
    const bool aboutHost = (describedSlot == 0);
    // The row has two authors, and this flag is the line between them. Always applied,
    // host-authored or host-arbitrated and unlearnable any other way: the slot, playerNo, the
    // eid, the link facts and the nick (the host may rename for uniqueness, which is why it sits
    // above this gate), all parsed from the fixed prefix and so reachable on every row. Declared,
    // peer-authored and merely relayed: skin, prefs, colour, suppressed for our own row and the
    // host's. A host-authored field must not move into the declared block for symmetry: that
    // block is skipped for exactly those two rows.
    const bool applyDeclared = !aboutSelf && !aboutHost;

    // Occupancy, always applied: the host issues it, and it is unlearnable otherwise.
    if (playerNo == 0) {
        // The dev fault injection: pretend every emptying row was lost. The design's central claim
        // is that a receiver never needs to observe absence, and this flag is the only way to test
        // it, since the pulse re-asserts the empty row until it lands; with it on, the departure
        // can reach this peer only as the successor's row, which must read as death then birth.
        if (DropEmptyRowsForTest()) {
            UE_LOGI("roster: [dev] DROPPED the empty row for slot %u (loss injection)",
                    static_cast<unsigned>(describedSlot));
            return true;
        }
        coop::roster_ledger::ClearRow(describedSlot);
        return true;
    }
    coop::roster_ledger::InstallRow(describedSlot, playerNo, /*bornGeneration=*/0);

    // The connection facts, always applied: host-measured for every player, itself included, so a
    // client's board answers how a player is connected identically to every other board rather
    // than synthesising a value for links it cannot see.
    coop::roster_ledger::SetLinkFacts(describedSlot, linkKind, pingMs);

    // The nick.
    std::wstring nick = coop::roster_ledger::Get(describedSlot).nick;
    size_t nickFieldLen = 0;
    if (nickRemaining > 0) {
        const int len = nickStart[0];
        if (1 + len <= static_cast<int>(nickRemaining)) {
            nickFieldLen = 1 + static_cast<size_t>(len);
            if (len > 0) nick = FromUtf8(nickStart + 1, len);
        }
    }
    // An empty nick means not known yet, not called nothing. The host installs a row the moment a
    // slot is ready, before that peer's Join carries its name, so the first row about a joiner
    // legitimately has no nick; running the sanitizer on it would mint the placeholder and store
    // it as a name, and the joiner would then adopt the placeholder as canonical and persist it
    // over the one the person chose. The placeholder is a display fallback the ledger owns;
    // identity keeps unknown as empty. The sanitizer still runs on every non-empty peer-authored
    // string.
    if (!nick.empty()) nick = SanitizeNickname(nick);
    if (nick != coop::roster_ledger::Get(describedSlot).nick) {
        coop::roster_ledger::SetNick(describedSlot, nick);
        if (RemotePlayer* p = coop::players::Registry::Get().Puppet(describedSlot))
            p->SetNickname(nick);
    }
    // The handback: a row about us carries the name the host assigned, and the ledger is not where
    // our own name is read from (the roster's local row, chat authorship, the action feed and the
    // Join payload read the local-name store), so writing the ledger row alone would leave those
    // surfaces showing the name we asked for. Only a name the host authored from our request is
    // ours to keep; an empty field means the host has not heard our Join yet.
    if (aboutSelf && !nick.empty()) coop::player_handshake::AdoptCanonicalNickname(nick);

    // The mirror Element. Rows about slot 0 and our own slot carry the eid 0 sentinel and skip
    // this; slot 0's identity is AssignPeerSlot's to author.
    if (applyDeclared && describedEid != 0u &&
        describedEid != coop::element::kInvalidId) {
        if (!coop::element::Registry::IsAllowedPeerAllocatedEid(describedEid)) {
            UE_LOGW("roster: row slot=%u eid=0x%08x not in peer range -- dropping "
                    "mirror install", static_cast<unsigned>(describedSlot), describedEid);
        } else if (!coop::players::Registry::Get().GetPlayerElement(describedSlot)) {
            // Logged only on the actual install, never on a pulse re-assert: the line is a smoke
            // signal (mp.py counts it), and a per-second repeat would make it worthless.
            coop::players::Registry::Get().EstablishMirrorForSlot(describedSlot, describedEid);
            UE_LOGI("roster: client installed cross-peer identity slot=%u eid=0x%08x nick='%ls'",
                    static_cast<unsigned>(describedSlot), describedEid, nick.c_str());
        } else {
            coop::players::Registry::Get().EstablishMirrorForSlot(describedSlot, describedEid);
        }
    }

    // The declared display fields: skin, prefs, colour.
    if (applyDeclared) {
        size_t skinFieldLen = 0;
        if (nickFieldLen > 0 && nickFieldLen < nickRemaining) {
            std::string skin;
            skinFieldLen = ParseSkinField(nickStart + nickFieldLen,
                                          nickRemaining - nickFieldLen, &skin);
            if (skinFieldLen > 0 && !skin.empty())
                StoreSkinForSlot(describedSlot, std::move(skin));  // itself change-gated
        }
        if (skinFieldLen > 0 && nickFieldLen + skinFieldLen < nickRemaining) {
            StorePrefsFlagsForSlot(describedSlot, nickStart[nickFieldLen + skinFieldLen]);
            const size_t colorOff = nickFieldLen + skinFieldLen + 1;
            if (colorOff < nickRemaining)
                ParseNickColorField(nickStart + colorOff, nickRemaining - colorOff,
                                    describedSlot);
        }
    }
    (void)session;
    return true;
}

}  // namespace

bool HandleRosterRow(net::Session& session,
                        const net::Session::ReliableMessage& msg) {
    UE_ASSERT_GAME_THREAD("ledger row (HandleRosterRow)");
    // The host asserts rows and must never receive one; on a client the sender must be the host,
    // or a client peer could inject a forged occupant or evict a real one by asserting playerNo 0.
    if (session.role() == net::Role::Host) {
        UE_LOGW("roster: row received on host -- dropping");
        return true;
    }
    if (msg.senderPeerSlot != 0) {
        UE_LOGW("roster: row from non-host senderPeerSlot=%d -- dropping",
                msg.senderPeerSlot);
        return true;
    }
    // The park gate: until AssignPeerSlot stamps our slot we cannot tell a row about us from a row
    // about a remote peer, and misfiling it would apply the host's relayed copy of our own skin
    // and colour back onto us.
    if (coop::players::Registry::Get().LocalPeerId() == coop::players::kPeerIdUnknown) {
        ParkRow(msg.payload, msg.payloadLen);
        return true;
    }
    return ApplyRosterRow(session, msg.payload, msg.payloadLen);
}

}  // namespace coop::player_handshake
