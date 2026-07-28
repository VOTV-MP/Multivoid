// coop/session/player_handshake_roster.cpp -- the RosterRow wire family: the
// host ASSERTING who occupies each peer slot, and a client conforming to it.
//
// Extracted from player_handshake.cpp 2026-07-27 (arc A; modular file-size rule
// -- the file was at 847 LOC before this family was widened). Design of record:
// research/findings/join-identity/votv-nickname-arbitration-roster-id-DESIGN-2026-07-27.md
//
// WHAT CHANGED FROM `PlayerJoined` (same wire value 19, renamed in v130):
//
//   1. It is STATE, not an event. The host re-asserts rows on a repair pulse, so
//      the receiver must APPLY IDEMPOTENTLY -- stores conditioned on an actual
//      change, edge effects (mirror install, skin, prefs, colour, toast) gated on
//      a change or an existing latch. Treated as an event, a pulse would spam a
//      "joined the game" toast every second and reinstall the mirror for nothing.
//
//   2. It carries `playerNo` -- the occupant's session-unique ID, and `0` means
//      the slot is EMPTY. That is how a DEPARTURE reaches a client at all: there
//      is no PlayerLeft kind anywhere in the tree, and a recycled slot goes
//      X -> Y with no absence in between, so "watch for a disconnect" cannot
//      work. A receiver never observes absence; it conforms to the current token.
//
//   3. It may describe SLOT 0 (the host describing itself) and the RECEIVER'S
//      OWN slot. Those rows carry eid = 0 (the existing "no Element" sentinel),
//      so the receiver skips mirror install for them -- slot 0's eid still
//      arrives via AssignPeerSlot, which remains its author. Before this, a
//      client's TAB could only ever list itself and the host, because the roster
//      was derived from connection state a client does not have.
//
// PER-FIELD IGNORE RULE: for a row about slot 0 or about the receiver's own
// slot, the DECLARED fields (skin / prefs / colour) are NOT applied -- those are
// peer-authored and the host merely relays them, so applying the host's cached
// copy of our own skin back onto us is an authority inversion. `playerNo` is
// ALWAYS applied: it is host-issued and unlearnable any other way.

#include "coop/session/player_handshake.h"

#include "player_handshake_detail.h"

#include "coop/config/config.h"
#include "coop/element/player.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/nick_color.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <array>
#include <cstring>
#include <vector>

namespace coop::player_handshake {
namespace {

// Minimum payload: slot + playerNo + eid + linkKind + pingMs + an empty nick
// length byte. v131 widened the FIXED PREFIX rather than appending at the tail:
// the tail's offset arithmetic lives inside `if (applyDeclared)` below, which is
// false for exactly the host row and the receiver's own row -- the two rows this
// lane exists to populate -- so a tail-appended field would have been
// unreachable on precisely them.
constexpr size_t kRosterRowMinLen = 1 + 2 + 4 + 1 + 2 + 1;
constexpr size_t kRosterRowPrefixLen = 1 + 2 + 4 + 1 + 2;  // where the nick field starts

// [dev] roster_drop_empty_rows -- see the use site. Latched: a fault injection
// that could be switched mid-session would make a failure unattributable.
bool DropEmptyRowsForTest() {
    static const bool s =
        coop::config::ResolveFlag(::coop::config_registry::rows::roster_drop_empty_rows);
    return s;
}

// Build a RosterRow payload describing peer `slot`. Wire layout (parsed
// field-by-field, same discipline as Join):
//   [u8 slot][u16 playerNo][u32 eid][u8 linkKind][i16 pingMs]
//   [u8 nicklen][nick UTF-8]
//   [u8 skinlen][skin ASCII][u8 prefsFlags][u8 hasColor][r][g][b]
std::vector<uint8_t> BuildRosterRowPayload(uint8_t slot, uint16_t playerNo, uint32_t eid,
                                           coop::net::LinkKind linkKind, int16_t pingMs,
                                           const std::wstring& nick,
                                           const std::string& skin,
                                           uint8_t prefsFlags) {
    std::vector<uint8_t> out;
    out.resize(kRosterRowPrefixLen);
    out[0] = slot;
    std::memcpy(out.data() + 1, &playerNo, 2);
    std::memcpy(out.data() + 3, &eid, 4);
    out[7] = static_cast<uint8_t>(linkKind);
    std::memcpy(out.data() + 8, &pingMs, 2);
    std::vector<uint8_t> nickUtf8 = ToUtf8(nick);
    if (nickUtf8.size() > 200) nickUtf8.resize(200);
    out.push_back(static_cast<uint8_t>(nickUtf8.size()));
    out.insert(out.end(), nickUtf8.begin(), nickUtf8.end());
    const uint8_t skinLen = static_cast<uint8_t>(skin.size() > 48 ? 48 : skin.size());
    out.push_back(skinLen);
    out.insert(out.end(), skin.begin(), skin.begin() + skinLen);
    out.push_back(prefsFlags);
    AppendNickColorField(out, coop::nick_color::PackedForSlot(slot));
    return out;
}

// The row describing `slot` as the host currently knows it. An EMPTY slot still
// produces a row (playerNo 0) -- that row is the only way a client learns of a
// departure, so it must be sent, not skipped.
std::vector<uint8_t> BuildRowForSlot(int slot) {
    const coop::roster_ledger::Row& row = coop::roster_ledger::Get(slot);
    if (!row.occupied())
        return BuildRosterRowPayload(static_cast<uint8_t>(slot), 0, 0,
                                     coop::net::LinkKind::Unknown, -1, L"", "", 0);

    // The eid is the peer's mirror Player Element on the host. Rows about slot 0
    // carry the 0 sentinel: the host's own eid reaches a client via
    // AssignPeerSlot, and two authors for one identity is how the join-window
    // races were born.
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

// --- the repair pulse --------------------------------------------------------
//
// A reliable row can be LOST at enqueue (measured 2026-07-26: silent, and in a
// contiguous band), and the measured window where that happens is exactly a
// joiner's first seconds -- a ~500 KB save-transfer burst against a ~512 KB
// buffer. That is also precisely when a joiner presses TAB. So the host
// re-asserts the whole roster: fast while it matters, slow forever after.
//
// Cost, counted rather than asserted: a row is ~40 B, so four rows are ~160 B/s
// in the fast window and ~32 B/s in steady state. Against the measured
// backpressure context the pulse cannot CAUSE congestion -- it can only be a
// victim of it, which is the thing it heals.
constexpr uint64_t kPulseFastMs = 1000;   // first ~10 s after a roster change
constexpr uint64_t kPulseSlowMs = 5000;   // steady state
constexpr uint64_t kPulseFastWindowMs = 10000;

uint64_t g_lastPulseMs = 0;
uint64_t g_lastRosterChangeMs = 0;

// Parked inbound rows (T4b): a row about our own slot can arrive before
// AssignPeerSlot has stamped LocalPeerId, and applying it then would misfile it
// as a row about a remote peer. At most kMaxPeers rows are held -- the host
// sends one per slot, so a longer queue would only mean the same slots twice.
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
    // Called the moment AssignPeerSlot stamps our slot: everything we parked is
    // now unambiguous. Ordering (AssignPeerSlot is sent first, same lane) makes
    // this rare, but "rare" is not "never" and the failure is silent misfiling.
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
// Any occupancy change re-arms the fast pulse, so a departure reaches the other
// clients at once rather than up to five seconds later.
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

    // v131: conform the connection facts IMMEDIATELY BEFORE serializing, so the
    // bytes are exactly fresh and the fill has no clock of its own to drift
    // against this one. It runs only when a pulse is actually due -- which is
    // also why the GNS lock it takes is not a hot-path cost. NOTE for anyone
    // retuning kPulseSlowMs: it now has TWO consumers, roster repair AND the
    // freshness of every board's ping column.
    coop::roster_ledger::RefreshLinkFacts(session);

    // EVERY slot, INCLUDING empty ones (playerNo 0) and including slot 0 -- the
    // host describes itself. Skipping empties would mean absence never heals.
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

    // Same rule as the pulse: fill the connection facts immediately before these
    // rows are serialized, so a joiner's very first roster carries real values
    // instead of Unknown/-1 waiting on the next pulse.
    coop::roster_ledger::RefreshLinkFacts(session);

    // MTA InitialDataStream, two-way (reference/mtasa-blue/Server/.../CGame.cpp:1422
    // BroadcastOnlyJoined + :1435 the per-existing-peer send to the joiner):
    //   (1) announce the joiner to every OTHER connected client, and
    //   (2) tell the joiner about every slot the host knows -- now including
    //       slot 0 (the host itself) and the joiner's OWN row, which is how the
    //       joiner learns its host-issued player number.
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

// Apply one row. IDEMPOTENT: every store is conditioned on an actual change and
// every edge effect on a change or an existing latch, because the pulse delivers
// this same row over and over.
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
    // THE ROW HAS TWO AUTHORS, and this flag is the line between them:
    //
    //   ALWAYS APPLIED -- host-authored or host-ARBITRATED, unlearnable any
    //     other way: slot, playerNo, eid, linkKind, pingMs, and `nick` (the host
    //     may rename you for uniqueness, which is why it already sits above this
    //     gate). These are parsed from the FIXED PREFIX, so they are reachable
    //     on every row including our own and the host's.
    //   DECLARED -- peer-authored and merely RELAYED by the host: skin, prefs,
    //     colour. Applying the host's cached copy of OUR OWN choice back onto us
    //     is an authority inversion, hence the suppression below.
    //
    // Do NOT move a host-authored field into the declared block "for symmetry":
    // that block is skipped for exactly the host row and our own row, which is
    // how a client used to see nothing about itself.
    const bool applyDeclared = !aboutSelf && !aboutHost;

    // --- occupancy, always applied (the host issues it; it is unlearnable else)
    if (playerNo == 0) {
        // [dev] fault injection: pretend every EMPTYING row was lost on the wire.
        // The arc's central claim is that a receiver never needs to observe
        // absence -- it conforms to the current token -- and this flag is the only
        // way to test it, because the pulse re-asserts the empty row until it
        // lands. With it on, the departure can reach this peer ONLY as the
        // successor's row, which must then read as death-then-birth.
        if (DropEmptyRowsForTest()) {
            UE_LOGI("roster: [dev] DROPPED the empty row for slot %u (loss injection)",
                    static_cast<unsigned>(describedSlot));
            return true;
        }
        coop::roster_ledger::ClearRow(describedSlot);
        return true;
    }
    coop::roster_ledger::InstallRow(describedSlot, playerNo, /*bornGeneration=*/0);

    // --- the connection facts (v131), ALWAYS applied. Host-measured for EVERY
    // player including itself, so a client's board answers "how is this player
    // connected to the session" identically to every other board -- rather than
    // synthesising "VIA HOST" for peers whose link it structurally cannot see.
    coop::roster_ledger::SetLinkFacts(describedSlot, linkKind, pingMs);

    // --- the nick
    std::wstring nick = coop::roster_ledger::Get(describedSlot).nick;
    size_t nickFieldLen = 0;
    if (nickRemaining > 0) {
        const int len = nickStart[0];
        if (1 + len <= static_cast<int>(nickRemaining)) {
            nickFieldLen = 1 + static_cast<size_t>(len);
            if (len > 0) nick = FromUtf8(nickStart + 1, len);
        }
    }
    nick = SanitizeNickname(nick);
    if (nick != coop::roster_ledger::Get(describedSlot).nick) {
        coop::roster_ledger::SetNick(describedSlot, nick);
        if (RemotePlayer* p = coop::players::Registry::Get().Puppet(describedSlot))
            p->SetNickname(nick);
    }

    // --- the mirror Element. Rows about slot 0 / our own slot carry the eid 0
    // sentinel and skip this: slot 0's identity is AssignPeerSlot's to author.
    if (applyDeclared && describedEid != 0u &&
        describedEid != coop::element::kInvalidId) {
        if (!coop::element::Registry::IsAllowedPeerAllocatedEid(describedEid)) {
            UE_LOGW("roster: row slot=%u eid=0x%08x not in peer range -- dropping "
                    "mirror install", static_cast<unsigned>(describedSlot), describedEid);
        } else if (!coop::players::Registry::Get().GetPlayerElement(describedSlot)) {
            // Log only on the ACTUAL install, never on a pulse re-assert -- this
            // line is a smoke signal (mp.py's xpeer_identity counter) and a
            // per-second repeat would make it worthless as one.
            coop::players::Registry::Get().EstablishMirrorForSlot(describedSlot, describedEid);
            UE_LOGI("roster: client installed cross-peer identity slot=%u eid=0x%08x nick='%ls'",
                    static_cast<unsigned>(describedSlot), describedEid, nick.c_str());
        } else {
            coop::players::Registry::Get().EstablishMirrorForSlot(describedSlot, describedEid);
        }
    }

    // --- the declared display fields (skin / prefs / colour)
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
    // The host ASSERTS rows; it must never receive one. On a client, require the
    // sender to be the host -- a client peer crafting rows could otherwise inject
    // a forged occupant, or evict a real one by asserting playerNo 0.
    if (session.role() == net::Role::Host) {
        UE_LOGW("roster: row received on host -- dropping");
        return true;
    }
    if (msg.senderPeerSlot != 0) {
        UE_LOGW("roster: row from non-host senderPeerSlot=%d -- dropping",
                msg.senderPeerSlot);
        return true;
    }
    // PARK GATE (T4b): until AssignPeerSlot stamps our slot we cannot tell a row
    // about US from a row about a remote peer, and misfiling it would apply the
    // host's relayed copy of our own skin/colour back onto us.
    if (coop::players::Registry::Get().LocalPeerId() == coop::players::kPeerIdUnknown) {
        ParkRow(msg.payload, msg.payloadLen);
        return true;
    }
    return ApplyRosterRow(session, msg.payload, msg.payloadLen);
}

}  // namespace coop::player_handshake
