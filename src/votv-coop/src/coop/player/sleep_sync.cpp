// coop/player/sleep_sync.cpp -- see coop/player/sleep_sync.h.

#include "coop/player/sleep_sync.h"

#include "coop/comms/chat_feed.h"
#include "coop/net/latest_wins.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/roster_ledger.h"  // PerSlotState, DisplayName
#include "coop/text/utf8_codec.h"
#include "l10n/l10n.h"

#include "ue_wrap/actors/sleep.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/world/world_singleton.h"

#include <atomic>
#include <cstring>
#include <string>

namespace coop::sleep_sync {
namespace {

namespace SLP = ue_wrap::sleep;
using coop::net::SleepOp;
using coop::net::SleepPeerState;
using coop::net::SleepStatePayload;
using coop::net::WireKey;

std::atomic<coop::net::Session*> g_session{nullptr};

// ---- this peer's own state, as polled -------------------------------------------------------------
SleepPeerState g_localState = SleepPeerState::Awake;
WireKey        g_localBed{};              // the bed's class name while not awake, else empty
bool           g_localSleeping = false;   // isSleep
bool           g_dreamProbSuppressed = false;  // a client holds gamemode.dreamProbability at 0
void*          g_gmInst = nullptr;        // the gamemode the polled state belongs to

// ---- what is known of every peer ------------------------------------------------------------------
// The host: each slot's state as it applied it (slot 0 the host's own, published). A client: as the host
// relayed it.
SleepPeerState g_state[coop::net::kMaxPeers] = {};
WireKey        g_bed[coop::net::kMaxPeers] = {};
bool           g_hostSleeping = false;    // the host: its isSleep as last published. A client: the host's last flag.
// A client: the state last sent to the host.
SleepPeerState g_sentState = SleepPeerState::Awake;
WireKey        g_sentBed{};

bool IsHost(coop::net::Session* s) { return s->role() == coop::net::Role::Host; }

// Two beds are the same name: the length and the bytes inside it, whatever sits past them.
bool SameBed(const WireKey& a, const WireKey& b) {
    return a.len == b.len && a.len <= sizeof(a.data) && std::memcmp(a.data, b.data, a.len) == 0;
}

// A bed name is a class name: identifier characters inside `len` and zero past it. WireKey's contract says so
// and nothing on the wire enforces it (coop/items/coingun_arbiter.cpp:145-155 clamps the length and stops at
// the first NUL, where this refuses); a name with a line break would forge a line in every peer's log, and a
// tail byte would pass for a change. True with the first fault found.
bool BedFaulty(const WireKey& k, BedRefusal* why) {
    if (k.len > sizeof(k.data)) { *why = BedRefusal::Length; return true; }
    for (size_t i = 0; i < sizeof(k.data); ++i) {
        const unsigned char c = static_cast<unsigned char>(k.data[i]);
        if (i >= k.len) {
            if (c != 0) { *why = BedRefusal::Tail; return true; }
        } else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            *why = BedRefusal::Charset;
            return true;
        }
    }
    return false;
}

// One warning per refused field, at most one every five seconds each: a hostile sender cannot fill the log, and
// a flood on one field does not hide another's refusal. The fields are the literals this file passes; a table
// that ran out would share its last row, which no caller here can reach.
void Refuse(const char* field, int senderSlot) {
    struct Said { const char* field; bool said; uint64_t lastMs; };
    static Said table[12] = {};
    const uint64_t now = coop::net::NowMs();
    Said* row = nullptr;
    for (Said& e : table) {
        if (e.field && std::strcmp(e.field, field) == 0) { row = &e; break; }
        if (!e.field) { row = &e; e.field = field; break; }
    }
    if (!row) row = &table[11];
    if (row->said && now - row->lastMs < 5000) return;
    row->said = true;
    row->lastMs = now;
    UE_LOGW("sleep_sync: SleepState refused -- %s (sender slot %d)", field, senderSlot);
}

// The bed refusals, counted per sender and reason for the hostile drill (coop/dev/hostile_drill_world).
struct BedRefusals { uint32_t n[4] = {}; };
coop::roster_ledger::PerSlotState<BedRefusals> g_bedRefused;

void RefuseBed(BedRefusal why, int senderSlot) {
    if (senderSlot > 0 && senderSlot < static_cast<int>(coop::net::kMaxPeers))
        ++g_bedRefused[senderSlot].n[static_cast<int>(why)];
    Refuse("bed", senderSlot);
}

// The host's name for the chat line; the ledger owns the fallback when the host's row has no nick.
void PushHostAsleepLine() {
    const std::string name = coop::text::ToUtf8(coop::roster_ledger::DisplayName(0));
    char buf[256];
    if (l10n::Fmt(buf, sizeof(buf), l10n::T("%1$s is asleep -- the world runs faster"), name.c_str()) < 0)
        return;
    // A status readout of a state that ends: reading it back hours later would be noise, not history.
    coop::chat_feed::Push(std::string(buf), coop::chat_feed::Keep::Transient);
}

// To one slot, or (toSlot < 0) to every client but `exceptSlot` (-1: none).
void SendPeerRest(coop::net::Session* s, int toSlot, int slot, int exceptSlot = -1) {
    SleepStatePayload p{};
    p.op = static_cast<uint8_t>(SleepOp::PeerRest);
    p.state = static_cast<uint8_t>(g_state[slot]);
    p.slot = static_cast<uint8_t>(slot);
    p.bed = g_bed[slot];
    if (toSlot >= 0) {
        s->SendReliableToSlot(toSlot, coop::net::ReliableKind::SleepState, &p, sizeof(p));
        return;
    }
    for (int to = 1; to < static_cast<int>(coop::net::kMaxPeers); ++to)
        if (to != exceptSlot) s->SendReliableToSlot(to, coop::net::ReliableKind::SleepState, &p, sizeof(p));
}

void SendHostAsleep(coop::net::Session* s, int toSlot) {
    SleepStatePayload p{};
    p.op = static_cast<uint8_t>(SleepOp::HostAsleep);
    p.state = g_hostSleeping ? 1 : 0;
    if (toSlot < 0) s->SendReliable(coop::net::ReliableKind::SleepState, &p, sizeof(p));
    else s->SendReliableToSlot(toSlot, coop::net::ReliableKind::SleepState, &p, sizeof(p));
}

// ---- the host's side of a client's reports --------------------------------------------------------
// The host's applications of a client's rest report, per sender (an instrument: the world_latest drill counts
// them).
coop::roster_ledger::PerSlotState<uint32_t> g_reportsApplied;

// A client's rest reports, the newest kept at a pace (coop/net/latest_wins.h). The state on the wire is a level,
// sent on its edge; a player's entries and exits peak at 1 a second and 3 in ten in the archived client logs, and
// a changed state is relayed to every client, so a sender past its burst costs every peer a message. A state still
// owed when the player leaves is dropped with the slot; one still owed otherwise is paid like any other: only a
// sender past its burst is ever owed, and it could send the same state again. MTA applies a client's element data
// at once (CGame.cpp:2757); paced here for the relay's cost.
constexpr coop::net::IntentBudget kRestBudget{4.0f, 1.0f};
struct PendingRest {
    SleepPeerState state = SleepPeerState::Awake;
    WireKey bed{};
    coop::net::LatestWins pace;
};
coop::roster_ledger::PerSlotState<PendingRest> g_pending;

bool HoldsRest(int slot, SleepPeerState state, const WireKey& bed) {
    return g_state[slot] == state && SameBed(g_bed[slot], bed);
}

// HOST: a client's changed rest state, stored and relayed to every client but the reporter, as MTA's element-data
// relay skips its source (CGame.cpp:2761, the skip at :2768). MTA relays to joined players only; ours is the same
// through SendReliableToSlot's pre-world gate (coop/net/session.cpp:99).
void ApplyRest(coop::net::Session* s, int slot, SleepPeerState state, const WireKey& bed) {
    g_state[slot] = state;
    g_bed[slot] = bed;
    ++g_reportsApplied[slot];
    SendPeerRest(s, -1, slot, slot);
    UE_LOGI("sleep_sync: slot %d is now %d (bed '%.*s')", slot, static_cast<int>(state),
            static_cast<int>(bed.len), bed.data);
}

// ---- this peer's own state ------------------------------------------------------------------------
// The state of the gamemode now: dreaming over resting over awake. False while the flags do not resolve.
bool ReadLocal(SleepPeerState* state, bool* sleeping) {
    float asleep = 0.f, dreaming = 0.f;
    namespace V = ue_wrap::vitals;
    if (!V::Read(V::Field::Sleeping, &asleep) || !V::Read(V::Field::Dreaming, &dreaming)) return false;
    *sleeping = asleep != 0.f;
    *state = dreaming != 0.f ? SleepPeerState::Dreaming
                             : (*sleeping ? SleepPeerState::Resting : SleepPeerState::Awake);
    return true;
}

// A CLIENT: this peer's polled state to the host. Recorded as sent only when the transport took it.
void SendClientRest(coop::net::Session* s) {
    SleepStatePayload p{};
    p.op = static_cast<uint8_t>(SleepOp::ClientRest);
    p.state = static_cast<uint8_t>(g_localState);
    p.bed = g_localBed;
    if (s->SendReliableToSlot(0, coop::net::ReliableKind::SleepState, &p, sizeof(p))) {
        g_sentState = g_localState;
        g_sentBed = g_localBed;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!SLP::EnsureResolved()) return;
    void* gm = ue_wrap::world_singleton::Gamemode();
    if (!gm) return;

    // A fresh gamemode (boot, level reload) starts awake with its own dreamProbability; what was
    // published stays until the poll below compares it with the new one.
    if (gm != g_gmInst) {
        g_gmInst = gm;
        g_localState = SleepPeerState::Awake;
        std::memset(&g_localBed, 0, sizeof(g_localBed));
        g_localSleeping = false;
        g_dreamProbSuppressed = false;
    }

    SleepPeerState state = SleepPeerState::Awake;
    bool sleeping = false;
    if (!ReadLocal(&state, &sleeping)) return;
    if (state != g_localState || sleeping != g_localSleeping) {
        // The bed is read at the edge only: the gamemode sets it before isSleep rises, and a read per tick
        // would allocate. Into a dream the bed is kept as published: the non-ariral wakeup() clears
        // gamemode.bed before the dream, so a read there finds none.
        WireKey bed{};
        if (state == SleepPeerState::Dreaming && g_localState != SleepPeerState::Awake) bed = g_localBed;
        else if (state != SleepPeerState::Awake) coop::net::WireKeyFromString(SLP::BedClassName(), bed);
        g_localState = state;
        g_localSleeping = sleeping;
        g_localBed = bed;
        UE_LOGI("sleep_sync: local state %d (sleeping %d, bed '%.*s')", static_cast<int>(state),
                sleeping ? 1 : 0, static_cast<int>(bed.len), bed.data);
    }

    if (IsHost(s)) {
        // The host's own state is published to every client on a change; while nobody is connected the
        // tables still follow it, and a joiner is sent them (QueueConnectBroadcastForSlot).
        if (sleeping != g_hostSleeping) {
            g_hostSleeping = sleeping;
            if (s->connected()) SendHostAsleep(s, -1);
        }
        if (!HoldsRest(0, g_localState, g_localBed)) {
            g_state[0] = g_localState;
            g_bed[0] = g_localBed;
            if (s->connected()) SendPeerRest(s, -1, 0);
        }
        return;
    }

    if (!s->connected()) return;

    // A client holds its nightmare rolls off for the session: a nightmare is the host's machine's roll alone.
    if (!g_dreamProbSuppressed && SLP::SetDreamProbability(0.f)) g_dreamProbSuppressed = true;

    if (g_localState != g_sentState || !SameBed(g_localBed, g_sentBed)) SendClientRest(s);
}

void OnReliable(const uint8_t* payload, size_t len, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (len != sizeof(SleepStatePayload)) return Refuse("length", senderSlot);
    SleepStatePayload p{};
    std::memcpy(&p, payload, sizeof(p));
    if (p.op > static_cast<uint8_t>(SleepOp::PeerRest)) return Refuse("op", senderSlot);
    if (p.state > static_cast<uint8_t>(p.op == static_cast<uint8_t>(SleepOp::HostAsleep)
                                           ? SleepPeerState::Resting
                                           : SleepPeerState::Dreaming))
        return Refuse("state", senderSlot);
    BedRefusal fault{};
    if (BedFaulty(p.bed, &fault)) return RefuseBed(fault, senderSlot);
    if (p.op == static_cast<uint8_t>(SleepOp::HostAsleep)) {
        if (p.slot != 0) return Refuse("slot", senderSlot);
        if (p.bed.len != 0) return Refuse("bed", senderSlot);
    }

    if (IsHost(s)) {
        // The host takes a client's own state only, from the transport's sender slot.
        if (p.op != static_cast<uint8_t>(SleepOp::ClientRest)) return Refuse("op for the host", senderSlot);
        if (senderSlot == 0 || senderSlot >= coop::net::kMaxPeers) return Refuse("sender", senderSlot);
        if (p.slot != 0) return Refuse("slot", senderSlot);
        // MTA acts on a joined player's reports only (CGame.cpp:2376; coop/interactables/dish_calib_sync.cpp).
        if (!s->IsSlotWorldReady(senderSlot)) return Refuse("slot", senderSlot);
        const SleepPeerState state = static_cast<SleepPeerState>(p.state);
        if (state == SleepPeerState::Awake && p.bed.len != 0) return RefuseBed(BedRefusal::AwakeWithBed, senderSlot);
        PendingRest& pend = g_pending[senderSlot];
        if (HoldsRest(senderSlot, state, p.bed)) {
            pend.pace.Withdraw();
            return;
        }
        pend.state = state;
        pend.bed = p.bed;
        if (pend.pace.Offer(kRestBudget, coop::net::NowMs())) ApplyRest(s, senderSlot, state, p.bed);
        return;
    }

    // A client takes the host's word only (the transport's slot 0).
    if (senderSlot != 0) return Refuse("sender", senderSlot);
    switch (static_cast<SleepOp>(p.op)) {
    case SleepOp::HostAsleep: {
        const bool now = p.state != 0;
        if (now && !g_hostSleeping) PushHostAsleepLine();
        if (now != g_hostSleeping) UE_LOGI("sleep_sync: the host is %s", now ? "asleep" : "awake");
        g_hostSleeping = now;
        // The host refuses a rest from a slot it does not yet hold world-ready, so a rest sent early is sent
        // again here (the connect broadcast reaches this slot only once it is); a held one is dropped there.
        if (g_localState != SleepPeerState::Awake) SendClientRest(s);
        break;
    }
    case SleepOp::PeerRest: {
        if (p.slot >= coop::net::kMaxPeers) return Refuse("slot", senderSlot);
        const SleepPeerState state = static_cast<SleepPeerState>(p.state);
        if (!HoldsRest(p.slot, state, p.bed)) {
            g_state[p.slot] = state;
            g_bed[p.slot] = p.bed;
            UE_LOGI("sleep_sync: peer %u is now %d (bed '%.*s')", static_cast<unsigned>(p.slot),
                    static_cast<int>(state), static_cast<int>(p.bed.len), p.bed.data);
        }
        break;
    }
    default:
        Refuse("op for a client", senderSlot);
        break;
    }
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !IsHost(s)) return;
    if (peerSlot <= 0 || peerSlot >= static_cast<int>(coop::net::kMaxPeers)) return;
    SendHostAsleep(s, peerSlot);
    for (int slot = 0; slot < static_cast<int>(coop::net::kMaxPeers); ++slot)
        if (g_state[slot] != SleepPeerState::Awake) SendPeerRest(s, peerSlot, slot);
}

void OnDisconnectForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !IsHost(s)) return;
    if (slot <= 0 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    g_pending[slot].pace.Withdraw();
    if (g_state[slot] == SleepPeerState::Awake) return;  // every client already holds it awake
    g_state[slot] = SleepPeerState::Awake;
    std::memset(&g_bed[slot], 0, sizeof(WireKey));
    SendPeerRest(s, -1, slot);
}

SleepPeerState PeerState(int slot) {
    return (slot >= 0 && slot < static_cast<int>(coop::net::kMaxPeers)) ? g_state[slot]
                                                                         : SleepPeerState::Awake;
}

bool HostAsleepNow() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (s && !IsHost(s)) return g_hostSleeping;
    return SLP::IsSleeping();
}

void PayOwed(coop::net::Session& session) {
    if (session.role() != coop::net::Role::Host) return;
    uint64_t now = 0;  // read once, and only when a report is owed
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        PendingRest& pend = g_pending[slot];
        if (!pend.pace.Owed()) continue;
        if (HoldsRest(slot, pend.state, pend.bed)) {  // the slot's own arrival or leaving got there first
            pend.pace.Withdraw();
            continue;
        }
        if (now == 0) now = coop::net::NowMs();
        if (pend.pace.Due(kRestBudget, now)) ApplyRest(&session, slot, pend.state, pend.bed);
    }
}

uint32_t ReportsApplied(int slot) { return g_reportsApplied[slot]; }

uint32_t BedRefusalsFrom(int slot, BedRefusal why) {
    return g_bedRefused[slot].n[static_cast<int>(why)];
}

bool LocalInBed() { return g_localState != SleepPeerState::Awake; }

void OnDisconnect() {
    // Restore the SP nightmare sentinel. A host still asleep keeps sleeping: its timelapse was the game's own
    // all along. A client that loses its session leaves to the menu; its world stays at 1, which is what it ran.
    if (g_dreamProbSuppressed) {
        SLP::SetDreamProbability(-1.f);
        g_dreamProbSuppressed = false;
    }
    g_localState = SleepPeerState::Awake;
    std::memset(&g_localBed, 0, sizeof(g_localBed));
    g_localSleeping = false;
    g_gmInst = nullptr;
    for (auto& st : g_state) st = SleepPeerState::Awake;
    std::memset(g_bed, 0, sizeof(g_bed));
    g_hostSleeping = false;
    g_sentState = SleepPeerState::Awake;
    std::memset(&g_sentBed, 0, sizeof(g_sentBed));
}

}  // namespace coop::sleep_sync
