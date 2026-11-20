// coop/props/dingus_judge.cpp -- the host's judge of a client's report that it found a dingus (DingusReport). See
// coop/props/dingus_sync.h.
//
// MTA's server validates a client's change request before it writes the element's data, and the data is the server's (its
// custom data rides the add packet, CEntityAddPacket.cpp:178-181; a client's change is judged in Packet_CustomData,
// CGame.cpp:2715-2798, and a client sends only a changed value, CStaticFunctionDefinitions.cpp:1032). Here the datum is the
// host's found set, and a report is the request: the host reads the sender's standing, a budget, and a live dingus copy under
// the key within the sender's reach before the set takes the key.
//
// Divergence from CGame.cpp:2779-2796, where a cancelled change sends the authoritative value back to its source: only the
// `known` refusal answers (the key, to the sender alone), because a finder whose set lacks a key the host holds is the one
// honest case it covers. The other refusals stay silent. An honest finder is refused by `budget` alone, since a client sends no
// report for a key its set holds and a burst of four is never spent by one player's grabs; a forged report has no effect.

#include "coop/props/dingus_sync.h"

#include "dingus_internal.h"

#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_key_index.h"
#include "ue_wrap/actors/dingus.h"
#include "ue_wrap/core/log.h"

#include <cstdint>

namespace coop::dingus_sync {
namespace {

namespace D  = ue_wrap::dingus;
namespace EL = coop::element;
namespace N  = coop::net;

// How far from a dingus a client may be and still have found it: the intent target's own measure with its pads, as the mining
// rig's use and claim ask it.
constexpr float kReachCm = 600.f;

// A client's reports, per slot, reset with its occupant: four at once and one every two seconds (a grab or an action each).
constexpr N::IntentBudget kReportBudget{4.0f, 0.5f};
coop::roster_ledger::PerSlotState<N::IntentBucket> g_budgets;

enum Reason : uint8_t { kNotReady, kDead, kBudget, kKnown, kNoCopy, kNotDingus, kUnreachable, kReasons };
const char* ReasonName(uint8_t r) {
    static const char* const kNames[kReasons] = {"notReady", "dead", "budget", "known", "noCopy", "notDingus", "unreachable"};
    return r < kReasons ? kNames[r] : "?";
}

uint32_t g_judged = 0, g_accepted = 0;
uint32_t g_refused[kReasons] = {};
uint64_t g_firstMs = 0, g_lastMs = 0;   // the net clock at the first and the latest judged report, for the drill's view

// One refusal, counted by its reason and said at the first and each power of two of its count. A budget refusal is counted
// only: a flood costs the host no line.
void Refuse(int slot, uint8_t reason, const std::wstring& key) {
    const uint32_t n = ++g_refused[reason];
    if (reason == kBudget || (n & (n - 1)) != 0) return;
    UE_LOGI("dingus_sync: HOST refused slot %d's report of '%ls': %s (#%u)", slot, key.c_str(), ReasonName(reason), n);
}

}  // namespace

namespace detail {

void JudgeReport(int senderSlot, const std::wstring& key) {
    N::Session* const s = SessionPtr();
    if (!s) return;
    ++g_judged;
    const uint64_t now = N::NowMs();
    if (!g_firstMs) g_firstMs = now;
    g_lastMs = now;
    // A slot outside the table has no standing: counted notReady.
    if (senderSlot <= 0 || senderSlot >= static_cast<int>(N::kMaxPeers) || !s->IsSlotWorldReady(senderSlot))
        return Refuse(senderSlot, kNotReady, key);
    if (coop::players::Registry::Get().LifeOf(static_cast<uint8_t>(senderSlot)) != coop::players::PuppetLife::Alive)
        return Refuse(senderSlot, kDead, key);
    if (!g_budgets[senderSlot].Take(kReportBudget, now)) return Refuse(senderSlot, kBudget, key);
    if (Has(key)) {
        // The sender's own set lacks the key (two finders at once, the first word still on its way): it hears the key again.
        SayToSlot(senderSlot, key);
        return Refuse(senderSlot, kKnown, key);
    }
    void* const actor = coop::prop_key_index::Find(key);
    if (!actor) return Refuse(senderSlot, kNoCopy, key);
    if (!D::IsDingus(actor)) return Refuse(senderSlot, kNotDingus, key);
    const EL::IntentSubject subject =
        EL::IntentTarget::ForClientIntent(*s, static_cast<uint8_t>(senderSlot), kReachCm).Authorize(actor);
    // The authorizer's outcomes, by what they say of the report: no row or a dead copy is no live copy under the key (noCopy), a
    // live element of another type is not a dingus (notDingus), no puppet that took a pose is no body to measure from (dead), and
    // a copy out of reach or with no readable place cannot be shown within reach (unreachable).
    switch (subject.outcome) {
        case EL::IntentOutcome::Ok: break;
        case EL::IntentOutcome::NoRow:
        case EL::IntentOutcome::StaleDead: return Refuse(senderSlot, kNoCopy, key);
        case EL::IntentOutcome::WrongType: return Refuse(senderSlot, kNotDingus, key);
        case EL::IntentOutcome::NoBody: return Refuse(senderSlot, kDead, key);
        case EL::IntentOutcome::OutOfReach:
        case EL::IntentOutcome::NoTarget: return Refuse(senderSlot, kUnreachable, key);
    }
    ++g_accepted;
    AcceptFound(senderSlot, key, actor);
}

void ClearJudge() {
    if (g_judged)
        UE_LOGI("dingus_sync: session end -- reports judged %u, accepted %u, refused: notReady %u, dead %u, budget %u, known %u, noCopy %u, "
                "notDingus %u, unreachable %u", g_judged, g_accepted, g_refused[kNotReady], g_refused[kDead], g_refused[kBudget],
                g_refused[kKnown], g_refused[kNoCopy], g_refused[kNotDingus], g_refused[kUnreachable]);
    g_judged = g_accepted = 0;
    g_firstMs = g_lastMs = 0;
    for (uint32_t& n : g_refused) n = 0;
    for (int slot = 0; slot < g_budgets.size(); ++slot) g_budgets[slot] = N::IntentBucket{};
}

}  // namespace detail

namespace dev {
JudgeView Judge() {
    static_assert(kJudgeReasons == kReasons, "the drill view carries one count for each reason");
    JudgeView v;
    v.judged = g_judged;
    v.accepted = g_accepted;
    for (int i = 0; i < kJudgeReasons; ++i) v.refused[i] = g_refused[i];
    v.dropped = detail::DroppedCount();
    v.firstMs = g_firstMs;
    v.lastMs = g_lastMs;
    return v;
}
void ResetJudgeSpan() { g_firstMs = g_lastMs = 0; }
}  // namespace dev

}  // namespace coop::dingus_sync
