// coop/player/hand_item_applied.cpp -- the host's applied hand state of each slot, the pace it is applied at, and what
// the host's verbs ask of it; see coop/player/hand_item.h for the lane's design and hand_item_applied.h for what it
// shares with hand_item.cpp.

#include "hand_item_applied.h"  // src-local: the slot's state and the seams shared with hand_item.cpp

#include "coop/player/hand_item.h"

#include "coop/net/latest_wins.h"
#include "coop/net/net_clock.h"
#include "coop/player/hand_history.h"
#include "coop/player/roster_ledger.h"

#include <cstring>
#include <cwchar>
#include <vector>

namespace coop::hand_item {
namespace detail {

SlotHand g_hands[coop::players::kMaxPeers];  // GT-only

// The payload: slot, has, a length-prefixed class, name and item, and when held the measured
// view-relative position and rotation.
std::vector<uint8_t> BuildPayload(uint8_t slot, const SlotHand& h) {
    std::vector<uint8_t> out;
    const size_t cl = h.has ? (h.cls.size() > kMaxStr ? kMaxStr : h.cls.size()) : 0;
    const size_t nl = h.has ? (h.name.size() > kMaxStr ? kMaxStr : h.name.size()) : 0;
    const size_t il = h.has ? (h.item.size() > kMaxStr ? kMaxStr : h.item.size()) : 0;
    out.reserve(5 + cl + nl + il + (h.has ? 24 : 0));
    out.push_back(slot);
    out.push_back(h.has ? 1 : 0);
    out.push_back(static_cast<uint8_t>(cl));
    for (size_t i = 0; i < cl; ++i) out.push_back(static_cast<uint8_t>(h.cls[i]));
    out.push_back(static_cast<uint8_t>(nl));
    for (size_t i = 0; i < nl; ++i) out.push_back(static_cast<uint8_t>(h.name[i]));
    out.push_back(static_cast<uint8_t>(il));
    for (size_t i = 0; i < il; ++i) out.push_back(static_cast<uint8_t>(h.item[i]));
    if (h.has) {
        const float rel[6] = {h.relPos[0], h.relPos[1], h.relPos[2],
                              h.relRot[0], h.relRot[1], h.relRot[2]};
        const size_t base = out.size();
        out.resize(base + sizeof(rel));
        std::memcpy(out.data() + base, rel, sizeof(rel));
    }
    return out;
}

void SendState(coop::net::Session& session, uint8_t slot, const SlotHand& h) {
    const std::vector<uint8_t> p = BuildPayload(slot, h);
    if (session.role() == coop::net::Role::Host) {
        for (int x = 1; x < coop::net::kMaxPeers; ++x) {
            if (x == slot) continue;                 // never echo the originator
            if (!session.IsSlotReady(x)) continue;
            session.SendReliableToSlot(x, coop::net::ReliableKind::HandItem,
                                       p.data(), static_cast<int>(p.size()));
        }
    } else {
        session.SendReliableToSlot(0, coop::net::ReliableKind::HandItem,
                                   p.data(), static_cast<int>(p.size()));
    }
}

namespace {

// Host: the hand states applied for a slot's occupant (StatesApplied).
coop::roster_ledger::PerSlotState<uint32_t> g_statesApplied;

HandAppliedFn g_appliedSubs[kMaxHandSubscribers];  // GT-only, added once at install

// Host: per slot, the newest changed hand state and its pace (coop/net/latest_wins.h); one struct, so both clear with the
// occupant.
struct PendingHand { SlotHand value; coop::net::LatestWins pace; };
coop::roster_ledger::PerSlotState<PendingHand> g_pendingHand;

// Whether two states hold the same thing: emptiness, the class without case, the name and the item. The hold
// transform is not compared: the pose stream overwrites the applied one each sample.
bool SameHeld(const SlotHand& a, const SlotHand& b) {
    if (a.has != b.has) return false;
    return !a.has || (::_wcsicmp(a.cls.c_str(), b.cls.c_str()) == 0 && a.name == b.name && a.item == b.item);
}

// Host: one client's hand state applied, field by field: `lastCls` moves only while the hand holds
// something, since the pry gate reads it once the hand is empty (LastHeldClassIs). Then re-sent to
// every other ready client.
void ApplyHandState(coop::net::Session& session, uint8_t slot, const SlotHand& in) {
    SlotHand& h = g_hands[slot];
    h.has = in.has; h.cls = in.cls; h.name = in.name; h.item = in.item;
    if (in.has) { h.lastCls = in.cls; ++h.holdGen; }
    std::memcpy(h.relPos, in.relPos, sizeof(h.relPos));
    std::memcpy(h.relRot, in.relRot, sizeof(h.relRot));
    h.haveRel = in.has;
    coop::hand_history::Note(slot, in.has, in.cls, coop::net::NowMs());
    SendState(session, slot, h);
    ++g_statesApplied[slot];
    for (HandAppliedFn fn : g_appliedSubs) if (fn) fn(slot);
}

}  // namespace

bool Settles(uint8_t slot, const SlotHand& in) {
    if (!SameHeld(in, g_hands[slot])) return false;
    g_pendingHand[slot].pace.Withdraw();
    for (HandAppliedFn fn : g_appliedSubs) if (fn) fn(slot);   // an owed state that settles is an edge too
    return true;
}

void Offer(coop::net::Session& session, uint8_t slot, const SlotHand& in) {
    PendingHand& pend = g_pendingHand[slot];
    pend.value = in;
    if (pend.pace.Offer(kHandBudget, coop::net::NowMs())) ApplyHandState(session, slot, in);
}

}  // namespace detail

using namespace detail;

bool HeldClassIs(uint8_t slot, const wchar_t* cls) {
    if (slot >= coop::players::kMaxPeers || !cls) return false;
    const SlotHand& h = g_hands[slot];
    return h.has && ::_wcsicmp(h.cls.c_str(), cls) == 0;
}

bool LastHeldClassIs(uint8_t slot, const wchar_t* cls) {
    if (slot >= coop::players::kMaxPeers || !cls) return false;
    const SlotHand& h = g_hands[slot];
    return !h.lastCls.empty() && ::_wcsicmp(h.lastCls.c_str(), cls) == 0;
}

void PayOwed(coop::net::Session& session) {
    if (session.role() != coop::net::Role::Host) return;
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot) {
        PendingHand& pend = g_pendingHand[slot];
        if (pend.pace.Owed() && pend.pace.Due(kHandBudget, coop::net::NowMs()))
            ApplyHandState(session, static_cast<uint8_t>(slot), pend.value);
    }
}

void ApplyOwedNow(uint8_t slot) {
    if (!g_session || g_session->role() != coop::net::Role::Host || slot == 0 || slot >= coop::players::kMaxPeers) return;
    PendingHand& pend = g_pendingHand[slot];
    if (!pend.pace.Owed()) return;
    pend.pace.Withdraw();
    ApplyHandState(*g_session, slot, pend.value);
}

bool HoldsItem(uint8_t slot) {
    return slot < coop::players::kMaxPeers && g_hands[slot].has;
}

uint32_t HoldGeneration(uint8_t slot) { return slot < coop::players::kMaxPeers ? g_hands[slot].holdGen : 0; }

bool SubscribeHandApplied(HandAppliedFn fn) {
    if (!fn) return false;
    for (HandAppliedFn& s : g_appliedSubs) {
        if (s == fn) return true;
        if (!s) { s = fn; return true; }
    }
    return false;
}

bool HandStateOwed(uint8_t slot) {
    return slot < coop::players::kMaxPeers && g_pendingHand[slot].pace.Owed();
}

uint32_t StatesApplied(int slot) {
    return (slot >= 0 && slot < static_cast<int>(coop::players::kMaxPeers)) ? g_statesApplied[slot] : 0;
}

std::wstring HeldItem(uint8_t slot) {
    if (slot >= coop::players::kMaxPeers) return {};
    const SlotHand& h = g_hands[slot];
    return h.has ? h.item : std::wstring();
}

}  // namespace coop::hand_item
