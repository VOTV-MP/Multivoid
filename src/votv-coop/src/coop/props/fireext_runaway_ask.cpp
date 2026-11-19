// coop/props/fireext_runaway_ask.cpp -- a client's damage to its copy of an extinguisher, asked of the host (FireExtIntent op 3).
//
// The client's `shoot` is refused (fireext_thrust_sync), so its damage would reach nobody; it asks instead, and the host runs
// the game's own verb on its copy, whose thrust then reaches every peer as the thrust lane's op 0.

#include "coop/props/fireext_sync.h"

#include "fireext_internal.h"

#include "coop/element/intent_authority.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/props/prop_hold.h"
#include "ue_wrap/actors/fire_extinguisher.h"
#include "ue_wrap/core/log.h"

#include <algorithm>

namespace coop::fireext_sync {
namespace {

namespace FX = ue_wrap::fire_extinguisher;
namespace EL = coop::element;
namespace N  = coop::net;

// How far from its body a client may be and still set off an extinguisher: the default blast radius of the game's explosion
// (its damage falls to 0 at it), which covers a blast at the client. A first bound, not a measured reach; the intent target
// adds the extinguisher's bounds and the pose pad.
constexpr float kRunawayReachUU = 1000.f;

// A client's asks: 3 at once, one a second.
constexpr N::IntentBudget kAskBudget{3.0f, 1.0f};

// HOST: the answer last given for each of the last 8 extinguishers, for the drill.
struct Answer {
    uint32_t eid = 0;
    detail::Reason reason = detail::Reason::Applied;
};
constexpr size_t kAnswers = 8;
Answer g_answers[kAnswers];
size_t g_nextAnswer = 0;

void Record(uint32_t eid, detail::Reason why) {
    for (Answer& a : g_answers)
        if (a.eid == eid) {
            a.reason = why;
            return;
        }
    g_answers[g_nextAnswer] = {eid, why};
    g_nextAnswer = (g_nextAnswer + 1) % kAnswers;
}

// HOST: one decision, tallied for the sender and kept as the eid's last answer. Logged at the first and each power of two
// of that sender's count for the reason; a budget refusal is counted and never logged per message.
void Decide(int slot, uint32_t eid, detail::Reason why) {
    uint32_t& n = detail::Ledger(slot).asks[static_cast<size_t>(why)];
    ++n;
    Record(eid, why);
    if (why == detail::Reason::Budget || (n & (n - 1)) != 0) return;
    UE_LOGI("fireext_sync: HOST slot %d's runaway ask for eid %u: %s (#%u)", slot, eid, detail::ReasonName(why), n);
}

// CLIENT: the copies asked about, each with the time of its last ask and the wait before it may be asked about again.
struct Asked {
    uint32_t eid = 0;
    uint64_t lastMs = 0;
    uint64_t nextMs = 0;
    uint32_t backoffMs = 1000;
};
constexpr size_t kMaxAsked = 8;
constexpr uint32_t kMaxBackoffMs = 30000;
// A ninth copy takes the place of the one asked about longest ago only when that ask is older than this: a cache eviction
// of a table of 8, never a verdict on either copy.
constexpr uint64_t kEvictAfterMs = 30000;
Asked g_asked[kMaxAsked];
size_t g_askedCount = 0;
uint32_t g_askedFull = 0;  // asks about a ninth copy, refused

}  // namespace

namespace detail {

const char* ReasonName(Reason r) {
    static const char* const kNames[kReasons] = {"applied", "budget", "notReady", "noRow", "wrongType", "noBody",
                                                 "outOfReach", "noTarget", "notExtinguisher", "heldByOther", "alreadyActive", "verbFailed"};
    const size_t i = static_cast<size_t>(r);
    return i < kReasons ? kNames[i] : "?";
}

// The judge, in order: the sender's ask budget, its world, the intent target (identity, body, reach), the extinguisher, who
// holds it, and the game's own guard (an extinguisher already `active`, spent ones included, does nothing). A refusal sends
// nothing: the client's copy did not thrust either, so the copies agree. An ask is a stateless one-shot: one refused or lost
// (a joiner not yet posed, a copy not yet spawned) leaves both copies still, and a damage that was a one-shot (a blast) is
// not asked again.
//
// MTA takes a damage claim on an element only from its occupant or syncer (CGame.cpp:2417-2426) and an explosion claim from
// any joined client (:2831); props have no simulator here, so the ask is judged on the sender's reach and the host's own
// holds (HeldByOther) instead.
// Residual: the host cannot see the client's damage, so a client within reach can set off any extinguisher near it, once
// each (the game never clears `active`).
void OnRunawayAsk(int senderSlot, const N::FireExtPayload& p) {
    N::Session* const session = SessionPtr();
    if (!IsHost() || !session || senderSlot < 1 || senderSlot >= static_cast<int>(N::kMaxPeers)) return;
    if (!Ledger(senderSlot).ask.Take(kAskBudget, N::NowMs())) {
        Decide(senderSlot, p.eid, Reason::Budget);
        return;
    }
    if (!session->IsSlotWorldReady(senderSlot)) {
        Decide(senderSlot, p.eid, Reason::NotReady);
        return;
    }
    const EL::IntentTarget target =
        EL::IntentTarget::ForClientIntent(*session, static_cast<uint8_t>(senderSlot), kRunawayReachUU);
    const EL::IntentSubject subject = target.Resolve(static_cast<EL::ElementId>(p.eid), EL::ElementType::Prop);
    Reason why = Reason::Applied;
    switch (subject.outcome) {
        case EL::IntentOutcome::Ok: break;
        case EL::IntentOutcome::NoRow:
        case EL::IntentOutcome::StaleDead: why = Reason::NoRow; break;
        case EL::IntentOutcome::WrongType: why = Reason::WrongType; break;
        case EL::IntentOutcome::NoBody: why = Reason::NoBody; break;
        case EL::IntentOutcome::OutOfReach: why = Reason::OutOfReach; break;
        case EL::IntentOutcome::NoTarget: why = Reason::NoTarget; break;
    }
    if (why != Reason::Applied) {
        Decide(senderSlot, p.eid, why);
        return;
    }
    void* const ext = subject.actor;
    bool active = false;
    if (!FX::IsExtinguisher(ext) || coop::hand_item::IsHandAxisActor(ext) || !FX::ReadThrusting(ext, active)) {
        Decide(senderSlot, p.eid, Reason::NotExtinguisher);
        return;
    }
    if (coop::prop_hold::HeldByOther(ext, senderSlot)) {
        Decide(senderSlot, p.eid, Reason::HeldByOther);
        return;
    }
    if (active) {
        Decide(senderSlot, p.eid, Reason::AlreadyActive);
        return;
    }
    if (!FX::StartRunaway(ext)) {
        Decide(senderSlot, p.eid, Reason::VerbFailed);
        return;
    }
    Decide(senderSlot, p.eid, Reason::Applied);
}

void ForgetAsk(uint32_t eid) {
    for (size_t i = 0; i < g_askedCount; ++i)
        if (g_asked[i].eid == eid) {
            g_asked[i] = g_asked[--g_askedCount];
            g_asked[g_askedCount] = Asked{};
            return;
        }
}

void ClearAsks() {
    if (g_askedFull > 0)
        UE_LOGI("fireext_sync: CLIENT %u runaway asks about a ninth copy were not sent over the session", g_askedFull);
    for (Asked& a : g_asked) a = Asked{};
    g_askedCount = 0;
    g_askedFull = 0;
}

void ClearAnswers() {
    for (Answer& a : g_answers) a = Answer{};
    g_nextAnswer = 0;
}

}  // namespace detail

void AskRunaway(void* copy) {
    N::Session* const session = detail::SessionPtr();
    if (!detail::IsClient() || !session->connected() || !copy) return;
    const EL::ElementId id = EL::Registry::Get().EidForActor(copy);
    if (id == EL::kInvalidId || id == 0) return;
    const uint32_t eid = static_cast<uint32_t>(id);
    const uint64_t now = N::NowMs();
    size_t i = 0;
    while (i < g_askedCount && g_asked[i].eid != eid) ++i;
    if (i == g_askedCount) {
        if (g_askedCount >= kMaxAsked) {
            size_t oldest = 0;
            for (size_t j = 1; j < g_askedCount; ++j)
                if (g_asked[j].lastMs < g_asked[oldest].lastMs) oldest = j;
            if (now - g_asked[oldest].lastMs <= kEvictAfterMs) {
                const uint32_t n = ++g_askedFull;
                if ((n & (n - 1)) == 0)
                    UE_LOGW("fireext_sync: CLIENT already waits on %zu copies' runaway asks -- the one for eid %u is not sent (#%u)",
                            kMaxAsked, eid, n);
                return;
            }
            i = oldest;
        } else {
            ++g_askedCount;
        }
        g_asked[i] = Asked{eid, now, 0, 1000};
    }
    Asked& a = g_asked[i];
    if (now < a.nextMs) return;
    a.lastMs = now;
    N::FireExtPayload p{};
    p.eid = eid;
    p.op = N::kFireExtOpRunawayAsk;
    const bool ok = session->SendReliable(N::ReliableKind::FireExtIntent, &p, sizeof(p));
    UE_LOGI("fireext_sync: CLIENT its copy eid %u took damage that starts a runaway -- asked the host (ok=%d, next ask in %u ms)",
            eid, ok ? 1 : 0, a.backoffMs);
    a.nextMs = now + a.backoffMs;
    a.backoffMs = std::min(a.backoffMs * 2, kMaxBackoffMs);
}

const char* LastRunawayAnswer(uint32_t eid) {
    for (const Answer& a : g_answers)
        if (a.eid == eid && eid != 0) return detail::ReasonName(a.reason);
    return nullptr;
}

}  // namespace coop::fireext_sync
