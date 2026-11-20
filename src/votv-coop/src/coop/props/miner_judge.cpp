// coop/props/miner_judge.cpp -- the host's judge of a client's use of an item on a rig and of its claim (MinerIntent), and the
// client's hearing of the answer. See coop/props/miner_sync.h.
//
// MTA checks a client's shot as it reads it (CBulletsyncPacket.cpp:193-201); the host has no sight of a client's inventory, so
// the evidence that an item was paid is the hand lane's record of the slot's hold (coop/player/hand_spend, which cites both).
// Residual: the hold is the holder's own word, bounded by the hand lane's announce budget and the one use a hold has; a client
// that announces a GPU it does not hold gets one use per announce, and a refusal of it gives the item back, at most as often
// as the slot's refund budget allows.
//
// The money is the host's: a claim is asked and run on the host's rig, which pays the shared balance, as MTA's server owns a
// player's money and clamps what it gives (MTA CStaticFunctionDefinitions.cpp:3232-3250). A refunded
// item has no MTA counterpart: MTA's server owns the weapon table, so nothing a client spends is ever the client's word.

#include "coop/props/miner_sync.h"

#include "miner_internal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/hand_spend.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/requester_feedback/requester_feedback.h"
#include "ue_wrap/actors/miner.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <cwchar>

namespace coop::miner_sync {
namespace {

namespace M  = ue_wrap::miner;
namespace EL = coop::element;
namespace N  = coop::net;

// How far from a rig a client may be and still use it or claim it: the game's own reach for a use on a prop is a few metres;
// this is the intent target's own measure with its pads, as the other intent lanes ask it.
constexpr float kReachCm = 600.f;

// A client's budgets, per slot and reset with its occupant: four uses at once and two a second, two claims at once and one a
// second (an honest player uses an item a second at the most).
constexpr N::IntentBudget kUseBudget{4.0f, 2.0f};
constexpr N::IntentBudget kClaimBudget{2.0f, 1.0f};
// The items a refusal gives back to one sender: two at once and one a minute, so a forged hold buys no more than that.
constexpr N::IntentBudget kRefundBudget{2.0f, 1.0f / 60.0f};
// A refund whose sender's body does not read is placed this far above the rig.
constexpr float kRefundLiftCm = 120.f;
struct Budgets {
    N::IntentBucket uses;
    N::IntentBucket claims;
    N::IntentBucket refunds;
};
coop::roster_ledger::PerSlotState<Budgets> g_budgets;

struct Tally {
    uint32_t byReason[N::kMinerReasons] = {};
};
coop::roster_ledger::PerSlotState<Tally> g_useTally;
coop::roster_ledger::PerSlotState<Tally> g_claimTally;
coop::roster_ledger::PerSlotState<Answer> g_last;
uint32_t g_answerSeq = 0;
Answer g_heard;
uint32_t g_granted = 0, g_claimAsks = 0, g_claimsRan = 0, g_refunded = 0, g_refundLost = 0, g_refundDropped = 0;

const char* ReasonName(uint8_t r) {
    static const char* const kNames[N::kMinerReasons] = {"granted", "noHold", "notReady", "noRig", "noBody", "outOfReach",
                                                         "gameRefused", "budget"};
    return r < N::kMinerReasons ? kNames[r] : "?";
}

bool BucketOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::miner_bucket_off);
    return off;
}
bool HoldJudgeOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::miner_hold_judge_off);
    return off;
}
bool RefusalStateOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::miner_refusal_state_off);
    return off;
}
bool RefundOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::miner_refund_off);
    return off;
}

void Answer1(int slot, uint32_t eid, uint8_t op, uint8_t item, uint8_t reason) {
    N::Session* const s = detail::SessionPtr();
    if (!s) return;
    N::MinerPayload p{};
    p.eid = eid;
    p.op = N::kMinerOpAnswer;
    p.item = item;
    p.granted = reason == N::kMinerReasonGranted ? 1 : 0;
    p.pad = reason;
    if (!s->SendReliableToSlot(static_cast<uint8_t>(slot), N::ReliableKind::MinerState, &p, sizeof(p)))
        detail::CountSendFailed();
    g_last[slot] = Answer{++g_answerSeq, eid, op, item, reason};
}

// One decision, tallied for the sender; said at the first and each power of two of its count for the reason (a grant is said
// each time, a budget drop never: it is counted). A budget drop is not answered either, as MTA drops a refused packet with no
// reply (MTA CBulletsyncPacket.cpp:193-201): the state owed behind it tells the asker its
// copy is wrong, and a flood costs the host no packet per ask.
void Decide(int slot, uint32_t eid, uint8_t op, uint8_t item, uint8_t reason) {
    Tally& t = (op == N::kMinerOpAdd ? g_useTally : g_claimTally)[slot];
    const uint32_t n = ++t.byReason[reason < N::kMinerReasons ? reason : 0];
    if (reason == N::kMinerReasonBudget) return;
    Answer1(slot, eid, op, item, reason);
    if (reason != N::kMinerReasonGranted && (n & (n - 1)) != 0) return;
    UE_LOGI("miner_sync: HOST slot %d's %s on rig eid %u (%ls): %s (#%u)", slot, op == N::kMinerOpAdd ? "use" : "claim", eid,
            item < N::kMinerItemClaim ? M::ItemHoldName(item) : L"claim", ReasonName(reason), n);
}

// The item the sender's hand paid, given back where the sender stands, or above the rig when its body does not read: the host's
// spawn watcher shows it to every peer. A sender whose refund budget is spent gets nothing back, counted; an item with no
// place or no class is lost, and it is said. Each refund is said, as the budget bounds them.
void Refund(int slot, uint32_t eid, uint8_t item) {
    if (RefundOff()) {
        UE_LOGI("miner_sync: HOST slot %d's refused use of %ls is not refunded (miner_refund_off)", slot, M::ItemHoldName(item));
        return;
    }
    if (!g_budgets[slot].refunds.Take(kRefundBudget, N::NowMs())) {
        const uint32_t n = ++g_refundDropped;
        if ((n & (n - 1)) == 0)
            UE_LOGW("miner_sync: HOST slot %d's refused use of %ls is not refunded -- its refund budget is spent (#%u)", slot,
                    M::ItemHoldName(item), n);
        return;
    }
    ue_wrap::FVector at{};
    const char* where = nullptr;
    coop::RemotePlayer* const rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    if (rp && rp->valid() && rp->GetActor() && ue_wrap::engine::TryGetActorLocation(rp->GetActor(), at)) {
        where = "where the sender stands";
    } else if (void* const rig = detail::RigOfEid(eid); rig && ue_wrap::engine::TryGetActorLocation(rig, at)) {
        at.Z += kRefundLiftCm;
        where = "above the rig";
    }
    void* const spawned = where ? M::SpawnItem(item, at) : nullptr;
    if (spawned) ++g_refunded;
    else ++g_refundLost;
    UE_LOGW("miner_sync: HOST slot %d's refused use of %ls refunded -- %s%s", slot, M::ItemHoldName(item),
            spawned ? "the item spawned " : "NOT spawned, the item is lost: ",
            spawned ? where : where ? "its class did not load or the world refused the spawn" : "no body and no rig to place it by");
}

// The sender's hand paid for `item`: the hand lane's record of its last announce holds that item, and the hold has a use left.
bool PaidByHand(N::Session& s, int slot, uint8_t item) {
    if (HoldJudgeOff()) return true;
    coop::hand_item::ApplyOwedNow(s, static_cast<uint8_t>(slot));
    const std::wstring held = coop::hand_item::HeldItem(static_cast<uint8_t>(slot));
    return !held.empty() && _wcsicmp(held.c_str(), M::ItemHoldName(item)) == 0 &&
           coop::hand_spend::Spend(static_cast<uint8_t>(slot), M::ItemClass(item), 1);
}

// The checks the use and the claim share, after the budget and the hand: the sender's world, its body and the rig within its
// reach. The rig is returned when they pass; the reason otherwise.
uint8_t Target(N::Session& s, int slot, uint32_t eid, void*& rig) {
    rig = nullptr;
    if (!s.IsSlotWorldReady(slot)) return N::kMinerReasonNotReady;
    if (coop::players::Registry::Get().LifeOf(static_cast<uint8_t>(slot)) != coop::players::PuppetLife::Alive)
        return N::kMinerReasonNoBody;
    const EL::IntentTarget target = EL::IntentTarget::ForClientIntent(s, static_cast<uint8_t>(slot), kReachCm);
    const EL::IntentSubject subject = target.Resolve(static_cast<EL::ElementId>(eid), EL::ElementType::Prop);
    switch (subject.outcome) {
        case EL::IntentOutcome::Ok: break;
        case EL::IntentOutcome::NoBody: return N::kMinerReasonNoBody;
        case EL::IntentOutcome::OutOfReach: return N::kMinerReasonOutOfReach;
        case EL::IntentOutcome::NoRow:
        case EL::IntentOutcome::StaleDead:
        case EL::IntentOutcome::WrongType:
        case EL::IntentOutcome::NoTarget: return N::kMinerReasonNoRig;
    }
    if (!M::IsMiner(subject.actor)) return N::kMinerReasonNoRig;
    rig = subject.actor;
    return N::kMinerReasonGranted;
}

void JudgeUse(N::Session& s, int slot, const N::MinerPayload& p) {
    Budgets& b = g_budgets[slot];
    const bool token = BucketOff() || b.uses.Take(kUseBudget, N::NowMs());
    // The hand is judged for a dropped ask too: a use the client paid on its copy and the host never ran is given back, and
    // only an item the hand lane's record holds is, so a flood of forged asks buys at most the slot's refund budget.
    const bool paid = PaidByHand(s, slot, p.item);
    uint8_t reason = N::kMinerReasonGranted;
    void* rig = nullptr;
    if (!token) reason = N::kMinerReasonBudget;
    else if (!paid) reason = N::kMinerReasonNoHold;
    else reason = Target(s, slot, p.eid, rig);
    if (reason == N::kMinerReasonGranted) {
        const coop::requester_feedback::Scope scope(static_cast<uint8_t>(slot), 0);  // the rig's hints are the asker's
        if (!M::CallUpgrade(rig, p.item)) reason = N::kMinerReasonGameRefused;
    }
    Decide(slot, p.eid, p.op, p.item, reason);
    if (reason == N::kMinerReasonGranted) {
        ++g_granted;
        detail::SendState(rig, -1);
        return;
    }
    if (paid) Refund(slot, p.eid, p.item);  // paid and not run: the hand lane counted the use
    if (!RefusalStateOff()) detail::OweState(static_cast<uint8_t>(slot), p.eid);
}

void JudgeClaim(N::Session& s, int slot, const N::MinerPayload& p) {
    ++g_claimAsks;
    Budgets& b = g_budgets[slot];
    uint8_t reason = N::kMinerReasonGranted;
    void* rig = nullptr;
    if (!(BucketOff() || b.claims.Take(kClaimBudget, N::NowMs()))) reason = N::kMinerReasonBudget;
    else reason = Target(s, slot, p.eid, rig);
    if (reason == N::kMinerReasonGranted) {
        coop::RemotePlayer* const rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
        const coop::requester_feedback::Scope scope(static_cast<uint8_t>(slot), 0);
        // The game's claim pays only a powered rig with points; for any other it says a hint to the asker and pays nothing,
        // which is a refusal here (reason 6), not a grant.
        M::Fields before;
        const bool pays = M::Read(rig, before) && before.powered && before.points > 0;
        const bool ran = M::CallClaim(rig, rp ? rp->GetActor() : nullptr);
        if (ran && pays) ++g_claimsRan;
        else reason = N::kMinerReasonGameRefused;
    }
    Decide(slot, p.eid, p.op, N::kMinerItemClaim, reason);
    if (reason == N::kMinerReasonGranted) {
        detail::SendState(rig, -1);  // the points went to the balance, which every peer reads from the host
        return;
    }
    if (!RefusalStateOff()) detail::OweState(static_cast<uint8_t>(slot), p.eid);
}

}  // namespace

namespace detail {

void JudgeIntent(int senderSlot, const N::MinerPayload& p) {
    N::Session* const s = SessionPtr();
    if (!s || !IsHost() || senderSlot <= 0 || senderSlot >= static_cast<int>(N::kMaxPeers)) return;
    if (p.op == N::kMinerOpAdd) JudgeUse(*s, senderSlot, p);
    else if (p.op == N::kMinerOpClaim) JudgeClaim(*s, senderSlot, p);
}

void HeardAnswer(const N::MinerPayload& p) {
    g_heard = Answer{++g_answerSeq, p.eid, p.item == N::kMinerItemClaim ? N::kMinerOpClaim : N::kMinerOpAdd, p.item, p.pad};
    if (p.pad == N::kMinerReasonGranted)
        UE_LOGI("miner_sync: CLIENT the host's rig %u took this player's %ls", p.eid,
                p.item < N::kMinerItemClaim ? M::ItemHoldName(p.item) : L"claim");
    else
        UE_LOGI("miner_sync: CLIENT the host refused this player's %ls on rig %u: %s", p.item < N::kMinerItemClaim ? M::ItemHoldName(p.item) : L"claim", p.eid,
                ReasonName(p.pad));
}

void ClearJudge() {
    if (g_granted || g_claimAsks || g_refunded || g_refundLost || g_refundDropped)
        UE_LOGI("miner_sync: session end -- uses granted %u, claims asked %u (paid %u), refunds %u (lost %u, past the budget %u)",
                g_granted, g_claimAsks, g_claimsRan, g_refunded, g_refundLost, g_refundDropped);
    g_granted = g_claimAsks = g_claimsRan = g_refunded = g_refundLost = g_refundDropped = 0;
    g_answerSeq = 0;
    g_heard = Answer{};
    for (int slot = 0; slot < g_budgets.size(); ++slot) {
        g_budgets[slot] = Budgets{};
        g_useTally[slot] = Tally{};
        g_claimTally[slot] = Tally{};
        g_last[slot] = Answer{};
    }
}

}  // namespace detail

Answer HostLastAnswer(uint8_t slot) { return g_last[slot]; }
Answer ClientLastAnswer() { return g_heard; }
uint32_t HostUsesGranted() { return g_granted; }
uint32_t HostClaimAsks() { return g_claimAsks; }
uint32_t HostClaimsRan() { return g_claimsRan; }
uint32_t HostRefunds() { return g_refunded; }
uint32_t HostUsesJudged(uint8_t slot) {
    uint32_t n = 0;
    for (const uint32_t c : g_useTally[slot].byReason) n += c;
    return n;
}

}  // namespace coop::miner_sync
