// coop/props/scrap_pile_intent.cpp -- see coop/props/scrap_pile_intent.h.

#include "coop/props/scrap_pile_intent.h"

#include "coop/element/element.h"
#include "coop/element/intent_authority.h"
#include "coop/element/registry.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/props/active_drive.h"
#include "coop/props/remote_prop.h"
#include "coop/props/trash_channel.h"
#include "coop/requester_feedback/requester_feedback.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/dig_tool.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>

namespace coop::scrap_pile_intent {
namespace {

namespace sg = ue_wrap::script_gate;

// The pile's dig verb. Declared on actorChipPile_C with no variant overriding it, so the row covers the
// erie, leaves and wet-concrete piles too; the dig calls it from mainPlayer_C's ubergraph as a script
// function, which is the script-body gate's to watch.
UE_FACT_FN(kRowTurnIntoScrap, "scrap_pile_intent", L"actorChipPile_C", L"turnIntoScrap", Declared);
constexpr int kTagScrap = 0x53435250;  // 'SCRP'

// The dig traces from the holder's arm, a reach shorter than a pack's; the pack's bound is kept so a
// sender at the edge of its own trace is never refused for the latency between its press and our check.
constexpr float kDigReachUU = 400.0f;

// One press is one dig, and a dig's montage is about a second, so an honest sender stays far inside the
// pack's budget; a stalled client's backlog waits its turn rather than being refused or run in one frame.
constexpr coop::net::IntentBudget kDigBudget{3.0f, 3.0f};
constexpr size_t   kMaxPending   = 8;
constexpr uint64_t kRefusalSayMs = 10000;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchInstalled = false;
bool g_watchLive = false;

// ---- client side -----------------------------------------------------------------------------
uint64_t g_sent = 0, g_refusedGhost = 0;

// ---- host side -------------------------------------------------------------------------------
struct Slot {
    coop::net::IntentQueue<coop::net::ScrapPileIntentPayload> queue;
    uint64_t nextSayMs = 0;  // a full queue is said once in kRefusalSayMs
};
coop::roster_ledger::PerSlotState<Slot> g_slots;
uint64_t g_dug = 0, g_denied = 0;

// The host's dig for `slot`: the pile's own body, inside that client's feedback scope, so its 2D dig
// sound plays on the digger's machine and not here. The item's birth and the pile's death ride the
// prop seams, as the host's own dig's do.
void PerformDig(void* pile, uint8_t slot, coop::element::ElementId eid) {
    bool ran = false;
    {
        coop::requester_feedback::Scope feedback(slot, 0);
        ran = ue_wrap::dig_tool::TurnIntoScrap(pile);
    }
    if (!ran) {
        ++g_denied;
        UE_LOGW("[SCRAP-PILE] the pile's turnIntoScrap did not dispatch for eid=%u slot=%u", eid,
                static_cast<unsigned>(slot));
        return;
    }
    ++g_dug;
    UE_LOGI("[SCRAP-PILE] DUG eid=%u for slot=%u", eid, static_cast<unsigned>(slot));
}

// One queued dig. Wait leaves it queued: the feedback seams are not up yet, so running it now would
// play the dig sound on the host.
coop::net::Served Execute(coop::net::Session& s, const coop::net::ScrapPileIntentPayload& p, uint8_t slot) {
    if (!coop::requester_feedback::Ready() && !coop::requester_feedback::Refused())
        return coop::net::Served::Wait;
    const coop::element::ElementId eid = p.eid;
    coop::RemotePlayer* puppet = coop::players::Registry::Get().Puppet(slot);
    if (!puppet || !puppet->valid() || !puppet->GetActor()) {
        ++g_denied;
        UE_LOGI("[SCRAP-PILE] DENY eid=%u slot=%u -- no live body", eid, static_cast<unsigned>(slot));
        return coop::net::Served::Done;
    }
    // The tool, on this machine: the dig's own gate is the tool in the presser's hand, and cancelling
    // the body on the client took that gate with it. The host asks its mirror of that peer's hand, as it
    // does for a bag or a broom.
    if (!ue_wrap::dig_tool::IsDigTool(coop::hand_item::MirrorActorForSlot(slot))) {
        ++g_denied;
        UE_LOGI("[SCRAP-PILE] DENY eid=%u slot=%u -- the sender holds no shovel or rake here", eid,
                static_cast<unsigned>(slot));
        return coop::net::Served::Done;
    }
    const auto token = coop::element::IntentTarget::ForClientIntent(s, slot, kDigReachUU);
    const coop::element::IntentSubject sub = token.Resolve(eid, coop::element::ElementType::Prop);
    if (sub.outcome != coop::element::IntentOutcome::Ok) {
        ++g_denied;
        UE_LOGI("[SCRAP-PILE] DENY eid=%u slot=%u -- %s", eid, static_cast<unsigned>(slot),
                coop::element::OutcomeName(sub.outcome));
        return coop::net::Served::Done;
    }
    if (!ue_wrap::prop::IsChipPile(sub.actor)) {
        ++g_denied;
        UE_LOGI("[SCRAP-PILE] DENY eid=%u slot=%u -- the target is not a chip pile", eid,
                static_cast<unsigned>(slot));
        return coop::net::Served::Done;
    }
    // A pile under an open carry or a pending land is mid-transition on the trash channel; the dig is
    // refused rather than destroying an entity that channel still drives.
    if (coop::trash_channel::IsCarrying(eid) || coop::trash_channel::HasPendingSettle(eid)) {
        ++g_denied;
        UE_LOGI("[SCRAP-PILE] DENY eid=%u slot=%u -- carried or settling", eid, static_cast<unsigned>(slot));
        return coop::net::Served::Done;
    }
    PerformDig(sub.actor, slot, eid);
    return coop::net::Served::Done;
}

// ---- the client's gate -----------------------------------------------------------------------
sg::Verdict OnTurnIntoScrapPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    void* pile = call.object;
    if (!pile) return sg::Verdict::Run;
    coop::element::ElementId eid = coop::remote_prop::ResolveMirrorEidByActor(pile);
    if (eid == 0u || eid == coop::element::kInvalidId) eid = coop::element::Registry::Get().EidForActor(pile);
    if (eid == 0u || eid == coop::element::kInvalidId) {
        // An identity-less pile: digging it here would author an item the host never sees and destroy a pile
        // the host still has. Refuse the dig instead.
        ++g_refusedGhost;
        if (g_refusedGhost <= 3 || g_refusedGhost % 20 == 0)
            UE_LOGW("[SCRAP-PILE] CLIENT dig on an identity-less pile %p -- refused (#%llu)", pile,
                    static_cast<unsigned long long>(g_refusedGhost));
        return sg::Verdict::Cancel;
    }
    coop::net::ScrapPileIntentPayload p{};
    p.eid = eid;
    if (!s->SendReliable(coop::net::ReliableKind::ScrapPileIntent, &p, sizeof(p)))
        return sg::Verdict::Cancel;  // a dig the host never heard of changed nothing anywhere
    ++g_sent;
    if (g_sent <= 3 || g_sent % 50 == 0)
        UE_LOGI("[SCRAP-PILE] CLIENT SENT eid=%u (#%llu)", eid, static_cast<unsigned long long>(g_sent));
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_watchInstalled)
        g_watchInstalled = sg::WatchName(kRowTurnIntoScrap, kTagScrap, &OnTurnIntoScrapPre, nullptr);
}

void Tick(coop::net::Session& session) {
    sg::ResolvePendingNames();
    if (!g_watchInstalled) {
        g_watchInstalled = sg::WatchName(kRowTurnIntoScrap, kTagScrap, &OnTurnIntoScrapPre, nullptr);
        return;
    }
    if (!g_watchLive && sg::NameWatchLive(kRowTurnIntoScrap, kTagScrap)) {
        g_watchLive = true;
        UE_LOGI("[SCRAP-PILE] the dig gate is live");
    }
    if (!session.running() || session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        g_slots[slot].queue.Serve(kDigBudget, coop::active_drive::NowMs,
                                  [&](const coop::net::ScrapPileIntentPayload& p) {
            return Execute(session, p, slot);
        });
    }
}

void OnScrapPileIntent(coop::net::Session& session, const coop::net::ScrapPileIntentPayload& payload,
                       uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    Slot& sl = g_slots[senderSlot];
    coop::net::ScrapPileIntentPayload copy = payload;
    if (!sl.queue.Push(std::move(copy), kMaxPending)) {
        const uint64_t now = coop::active_drive::NowMs();
        if (now >= sl.nextSayMs) {
            sl.nextSayMs = now + kRefusalSayMs;
            UE_LOGW("[SCRAP-PILE] slot %u queue full (%zu) -- refusing digs until it drains",
                    static_cast<unsigned>(senderSlot), sl.queue.size());
        }
        ++g_denied;
    }
}

void OnDisconnect() {
    if (g_sent || g_dug || g_denied || g_refusedGhost)
        UE_LOGI("[SCRAP-PILE] session end -- sent=%llu dug=%llu denied=%llu ghost-refused=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_dug),
                static_cast<unsigned long long>(g_denied), static_cast<unsigned long long>(g_refusedGhost));
    g_sent = g_dug = g_denied = g_refusedGhost = 0;
}

}  // namespace coop::scrap_pile_intent
