// coop/world/sound_judge.cpp -- see coop/world/sound_judge.h.

#include "coop/world/sound_judge.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"  // the sound drill's red arm of the judge
#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/protocol.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/world/sound_catalog.h"

#include <chrono>

namespace coop::sound_judge {
namespace {

namespace C = coop::sound_catalog;
using coop::sound_relay::Refusal;

// The longest reach of a swing in the game's `list_weapons` table (a row's `length`, which the attack's line trace is as
// long as): the sound is played where the trace struck, so a sender's body is within this of the place, and the
// authority's own allowance for the puppet's pose lagging the sender and for the camera's height above the body is added.
constexpr float kAttackReachCm = 500.0f;

// A swing is about one impact, and a swing comes about once a second.
constexpr coop::net::IntentBudget kBudget{8.0f, 4.0f};

coop::roster_ledger::PerSlotState<coop::net::IntentBucket> g_buckets;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The dev red arm of the drill's far and host-world arms: the host judges nothing and relays what it is asked.
bool JudgeOff() {
    static const bool s = coop::config::ResolveFlag(coop::config_registry::rows::sound_judge_off);
    return s;
}

}  // namespace

bool Allow(coop::net::Session& session, uint8_t slot, const C::Entry& entry, const coop::net::SoundAtPayload& p,
           Refusal& why) {
    if (JudgeOff()) return true;
    if (!(entry.callers & C::kAttack)) {
        why = Refusal::Caller;
        return false;
    }
    if (!g_buckets[slot].Take(kBudget, NowMs())) {
        why = Refusal::Budget;
        return false;
    }
    const ue_wrap::FVector at{p.loc[0], p.loc[1], p.loc[2]};
    const coop::element::IntentSubject subject =
        coop::element::IntentTarget::ForClientIntent(session, slot, kAttackReachCm).AuthorizeSegment(at, at);
    if (subject.outcome == coop::element::IntentOutcome::NoBody) {
        why = Refusal::NoBody;
        return false;
    }
    if (!subject) {
        why = Refusal::Reach;
        return false;
    }
    return true;
}

}  // namespace coop::sound_judge
