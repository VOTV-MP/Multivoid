// coop/interactables/oven_repair_judge.cpp -- see coop/interactables/oven_repair_judge.h.

#include "coop/interactables/oven_repair_judge.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"  // the oven drill's red arms
#include "coop/element/intent_authority.h"
#include "coop/interactables/interactable_sync.h"  // the oven lane's key for an oven, and its resolve
#include "coop/interactables/oven_progress_sync.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/session/net_pump.h"      // IsInAnnouncedWorld: a client's own world load runs natively

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/appliance.h"
#include "ue_wrap/devices/oven_screen.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace coop::oven_repair_judge {
namespace {

namespace A  = ue_wrap::appliance;
namespace IS = coop::interactable_sync;
namespace OS = ue_wrap::oven_screen;
namespace PS = coop::oven_progress_sync;
namespace sg = ue_wrap::script_gate;

// Watched by name: every class's `fix` fires (serverBox_C has one too), so the callback gates on the oven's class first.
UE_FACT_FN(kRowFix, "oven_repair_judge", nullptr, L"fix", AnyClass);
constexpr int kTagFix = 0x4F56464A;  // 'OVFJ'

// The reach a sender's oven use is judged against, measured from its body to the oven's switch box. The screen opens on
// a look-at trace of the player's reach (mainPlayer.armLength, 200 by default), so twice that is the bound, as the door
// verbs take it; the authority adds a 600 uu allowance for the puppet's pose lagging its sender
// (coop/element/intent_authority).
constexpr float kOvenReachCm = 400.0f;

// A sender's asks run from one ordered queue at a bounded rate: a repair is a handful of steps and one ask, so the bound
// is only felt by a stalled or flooding client.
constexpr coop::net::IntentBudget kBudget{8.0f, 4.0f};
constexpr size_t   kMaxPending = 8;
// How long a head waits for its sender's first pose before it is refused: a joiner's pose is seconds away, not a minute, so the
// wait is 10 s. While it waits, each tick re-reads the box's place (one engine call per waiting sender, bounded by this).
constexpr uint64_t kNoBodyBoundMs = 10000;
constexpr uint64_t kSayMs = 10000;
// The next time a refusal that no queue rate-limits (a repair message of action 0) is said.
uint64_t g_nextActionSayMs = 0;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchAsked = false;
bool g_settled = false;
Stats g_stats{};

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// One queued message of one sender: its steps (OvenProgressState) or its repair ask (OvenRepairState). Both kinds in one
// queue keep the order the client sent them in, so the lid is judged before the repair that needs it.
struct Item {
    bool         repair = false;
    std::wstring key;
    uint8_t      mask = 0;       // steps: the mask the sender holds
    uint64_t     waitSinceMs = 0;  // since when the head waits for its sender's body; 0 = not waiting
};

struct Slot {
    coop::net::IntentQueue<Item> queue;
    uint64_t nextSayMs = 0;  // the full queue's refusal, said once in kSayMs
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

// ---- host side -------------------------------------------------------------------------------
// The dev red arm of the reach, order and fixearly arms: the judge lets everything through. Resolved once.
bool JudgeOff() {
    static const bool s = coop::config::ResolveFlag(coop::config_registry::rows::oven_judge_off);
    return s;
}

const char* What(const Item& it) { return it.repair ? "repair" : "steps"; }

void Deny(const char* why, const Item& it, uint8_t slot, float distUU = -1.f) {
    UE_LOGI("[OVEN-JUDGE] DENY slot=%u %s key='%ls' mask=0x%02X -- %s (%.0f uu)", static_cast<unsigned>(slot), What(it),
            it.key.c_str(), it.mask, why, distUU);
}

// The steps of a granted message: a screw at any time, the lid only with all four screws in the host's record (the
// message's own screws count). Returns the bits granted; a refused lid counts and says so.
uint8_t JudgeSteps(const Item& it, uint8_t slot, uint8_t known, float distUU) {
    const uint8_t add = static_cast<uint8_t>(it.mask & OS::kAll & ~known);
    uint8_t grant = static_cast<uint8_t>(add & 0x0F);
    if (add & OS::kLid) {
        if (JudgeOff() || ((known | grant) & 0x0F) == 0x0F) {
            grant |= OS::kLid;
        } else {
            ++g_stats.refusedOrder;
            Deny("the lid with a screw still in", it, slot, distUU);
        }
    }
    return grant;
}

// False keeps the head at its sender's queue: the host has no body for the sender yet (its first pose has not arrived),
// so its reach cannot be measured, and the head waits for the body, up to a bound. Everything else is granted or
// refused, and consumed.
bool Execute(coop::net::Session& s, Item& it, uint8_t slot) {
    void* oven = it.key.empty() ? nullptr : IS::OvenActor(it.key);
    if (!oven) {
        ++g_stats.refusedNooven;
        Deny("the oven lane indexes no oven by that key", it, slot);
        return true;
    }
    float distUU = -1.f;
    if (!JudgeOff()) {
        ue_wrap::FVector box{};
        if (!A::TryReadOvenSwitchLocation(oven, box)) {
            ++g_stats.refusedReach;
            Deny("the oven's switch box does not read", it, slot);
            return true;
        }
        const coop::element::IntentSubject subject =
            coop::element::IntentTarget::ForClientIntent(s, slot, kOvenReachCm).AuthorizeSegment(box, box);
        if (subject.outcome == coop::element::IntentOutcome::NoBody) {
            const uint64_t now = NowMs();
            if (!it.waitSinceMs) {
                it.waitSinceMs = now;
                UE_LOGI("[OVEN-JUDGE] slot %u's oven %s waits: the host has no body for it yet", static_cast<unsigned>(slot),
                        What(it));
            }
            if (now - it.waitSinceMs <= kNoBodyBoundMs) return false;
            ++g_stats.refusedNobody;
            Deny("the sender took no pose in the wait", it, slot);
            return true;
        }
        distUU = subject.distUU;
        if (!subject) {
            ++g_stats.refusedReach;
            Deny(coop::element::OutcomeName(subject.outcome), it, slot, distUU);
            return true;
        }
    }
    if (!it.repair) {
        // A repaired oven has no steps to take: nothing is recorded or sent, so no client parks a mask for it.
        bool repaired = false;
        if (A::TryReadOvenFixed(oven, repaired) && repaired) {
            ++g_stats.already;
            Deny("steps on an oven already fixed", it, slot, distUU);
            return true;
        }
        const uint8_t grant = JudgeSteps(it, slot, PS::KnownMask(it.key), distUU);
        if (!grant) return true;  // nothing new, or only a refused lid (said)
        PS::GrantSteps(s, oven, it.key, grant, slot);
        ++g_stats.grantedSteps;
        UE_LOGI("[OVEN-JUDGE] GRANT slot=%u steps 0x%02X key='%ls' (%.0f uu)", static_cast<unsigned>(slot), grant,
                it.key.c_str(), distUU);
        return true;
    }
    bool fixed = false;
    if (!A::TryReadOvenFixed(oven, fixed)) {
        ++g_stats.fixFailed;
        Deny("the oven's repair state does not read", it, slot, distUU);
        return true;
    }
    if (fixed) {
        // A client whose grant was lost converges on the host's state: the asker alone is told.
        coop::net::KeyedTogglePayload p{};
        coop::net::WireKeyFromString(it.key, p.key);
        p.action = 1;
        s.SendReliableToSlot(slot, coop::net::ReliableKind::OvenRepairState, &p, sizeof(p));
        ++g_stats.already;
        UE_LOGI("[OVEN-JUDGE] slot=%u asked for an oven already fixed, key='%ls' -- told it alone", static_cast<unsigned>(slot),
                it.key.c_str());
        return true;
    }
    if (!JudgeOff() && !(PS::KnownMask(it.key) & OS::kLid)) {
        ++g_stats.refusedOrder;
        Deny("a repair with the lid not taken", it, slot, distUU);
        return true;
    }
    // The host's own fix(): its script-gate edge sends OvenRepairState to every peer, the asker included. A screen the
    // host has open is quit through the game's own exit, since fix() drops it from the oven without one.
    void* screen = OS::OpenScreenOf(oven);
    const bool ran = A::CallOvenFix(oven);
    if (screen && ran) OS::CallScreenQuit(screen);
    if (!ran) {
        ++g_stats.fixFailed;
        UE_LOGW("[OVEN-JUDGE] slot=%u's repair of key='%ls' was granted and the oven's fix() did not run",
                static_cast<unsigned>(slot), it.key.c_str());
        return true;
    }
    ++g_stats.grantedRepairs;
    UE_LOGI("[OVEN-JUDGE] GRANT slot=%u repair key='%ls' (%.0f uu)%s", static_cast<unsigned>(slot), it.key.c_str(), distUU,
            screen ? ", the host's open screen quit" : "");
    return true;
}

void Push(coop::net::Session& session, Item&& item, uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    Slot& sl = g_slots[senderSlot];
    if (sl.queue.Push(std::move(item), kMaxPending)) return;
    ++g_stats.refusedBucket;
    const uint64_t now = NowMs();
    if (now >= sl.nextSayMs) {
        sl.nextSayMs = now + kSayMs;
        UE_LOGW("[OVEN-JUDGE] slot %u queue full (%zu) -- refusing its oven messages until it drains",
                static_cast<unsigned>(senderSlot), sl.queue.size());
    }
}

// ---- the client's hold -----------------------------------------------------------------------
// The repair a client would run is asked of the host instead. The body is cancelled whatever the answer: it never moves
// this copy, and the host's OvenRepairState does. A kitchen with no key cannot be named to the host, so its repair is
// cancelled too, said once: running it here would fix this copy alone. A refused or lost ask gets no reply: the screen
// has already closed itself after the cancelled call, and the oven stays unfixed, so the player works the wiring again.
sg::Verdict OnFixPre(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    if (!c.object || !A::IsOven(c.object)) return sg::Verdict::Run;
    // A call on an object of a world this client has not announced ready is that world's load, the host's save: it runs
    // natively, and the host's state of what the lane owns follows the announce.
    if (!coop::net_pump::IsInAnnouncedWorld(c.object)) {
        ++g_stats.loadNative;
        return sg::Verdict::Run;
    }
    // The oven drill's repair arm's red: the client runs its own fix() and asks nobody. Resolved once.
    static const bool s_holdOff = coop::config::ResolveFlag(coop::config_registry::rows::oven_repair_hold_off);
    if (s_holdOff) {
        ++g_stats.ranHere;
        return sg::Verdict::Run;
    }
    const std::wstring key = IS::OvenKey(c.object);
    if (key.empty()) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGW("oven_repair_judge: a repair of an oven with no key is cancelled -- the host cannot be asked to repair "
                    "what it cannot name (said once)");
        }
        return sg::Verdict::Cancel;
    }
    // The steps first: a step the host never took (lost, refused for its pose) reaches it ahead of the ask in the same
    // lane, and the queue judges them in that order.
    PS::ResendCumulative(*s, c.object, key);
    coop::net::KeyedTogglePayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.action = 1;
    ++g_stats.held;
    if (!s->SendReliable(coop::net::ReliableKind::OvenRepairState, &p, sizeof(p))) {
        UE_LOGW("oven_repair_judge: the repair of '%ls' was not sent (the session refused it); the oven stays as the host "
                "has it", key.c_str());
        return sg::Verdict::Cancel;
    }
    ++g_stats.asked;
    UE_LOGI("oven_repair_judge: ASKED the host to repair '%ls' (#%llu)", key.c_str(),
            static_cast<unsigned long long>(g_stats.asked));
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watchAsked) return;
    g_watchAsked = true;
    if (!sg::WatchName(kRowFix, kTagFix, &OnFixPre, nullptr))
        UE_LOGE("oven_repair_judge: the script gate refused the watch on fix -- for the rest of this process a client's "
                "oven repair runs on its copy alone");
}

void Tick(coop::net::Session& session) {
    sg::ResolvePendingNames();
    if (!g_settled && g_watchAsked) {
        if (sg::NameWatchLive(kRowFix, kTagFix)) {
            g_settled = true;
            UE_LOGI("oven_repair_judge: the repair gate is live");
        } else if (sg::PendingNameCount() == 0) {
            g_settled = true;
            UE_LOGE("oven_repair_judge: the repair gate is dead (refused, or resolved into a full table)");
        }
    }
    if (!session.running() || session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        g_slots[slot].queue.Serve(kBudget, NowMs, [&](Item& it) {
            return Execute(session, it, slot) ? coop::net::Served::Done : coop::net::Served::Wait;
        });
    }
}

void OnAsk(coop::net::Session& session, const coop::net::KeyedTogglePayload& payload, uint8_t senderSlot) {
    // The game never unfixes an oven, so a client's repair message is action 1 alone.
    if (payload.action != 1) {
        ++g_stats.refusedAction;
        // Said before any queue or bucket, so it is rate-limited here, as the queue-full line is.
        const uint64_t now = NowMs();
        if (now >= g_nextActionSayMs) {
            g_nextActionSayMs = now + kSayMs;
            UE_LOGW("[OVEN-JUDGE] slot %u sent a repair message of action %u -- refused: the game never unfixes an oven "
                    "(said once in %llu s)", static_cast<unsigned>(senderSlot), static_cast<unsigned>(payload.action),
                    static_cast<unsigned long long>(kSayMs / 1000));
        }
        return;
    }
    Item it;
    it.repair = true;
    it.key = coop::net::StringFromWireKey(payload.key);
    Push(session, std::move(it), senderSlot);
}

void OnSteps(coop::net::Session& session, const coop::net::OvenProgressStatePayload& payload, uint8_t senderSlot) {
    Item it;
    it.key = coop::net::StringFromWireKey(payload.key);
    it.mask = static_cast<uint8_t>(payload.mask & OS::kAll);
    if (it.key.empty() || !it.mask) {
        ++g_stats.refusedEmpty;  // counted, said in the session's summary
        return;
    }
    Push(session, std::move(it), senderSlot);
}

Stats GetStats() { return g_stats; }

void OnDisconnect() {
    const Stats t = GetStats();
    if (t.asked || t.held || t.ranHere || t.loadNative || t.grantedSteps || t.grantedRepairs || t.already ||
        t.refusedReach || t.refusedOrder || t.refusedBucket || t.refusedNooven || t.refusedNobody || t.refusedAction || t.refusedEmpty ||
        t.fixFailed)
        UE_LOGI("[OVEN-JUDGE] session end -- asked=%llu held=%llu ran here=%llu own load run natively=%llu granted steps=%llu "
                "repairs=%llu already=%llu refused: reach=%llu order=%llu bucket=%llu nooven=%llu nobody=%llu action=%llu empty=%llu; "
                "fix failed=%llu",
                static_cast<unsigned long long>(t.asked), static_cast<unsigned long long>(t.held),
                static_cast<unsigned long long>(t.ranHere), static_cast<unsigned long long>(t.loadNative),
                static_cast<unsigned long long>(t.grantedSteps), static_cast<unsigned long long>(t.grantedRepairs),
                static_cast<unsigned long long>(t.already), static_cast<unsigned long long>(t.refusedReach),
                static_cast<unsigned long long>(t.refusedOrder), static_cast<unsigned long long>(t.refusedBucket),
                static_cast<unsigned long long>(t.refusedNooven), static_cast<unsigned long long>(t.refusedNobody),
                static_cast<unsigned long long>(t.refusedAction), static_cast<unsigned long long>(t.refusedEmpty),
                static_cast<unsigned long long>(t.fixFailed));
    g_stats = Stats{};
}

}  // namespace coop::oven_repair_judge
