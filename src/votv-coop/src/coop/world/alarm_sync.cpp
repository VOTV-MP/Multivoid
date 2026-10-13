// coop/world/alarm_sync.cpp -- see coop/world/alarm_sync.h.
//
// Two facts from the blueprint shape this lane:
//   - trigger_alarm_C.runTrigger(owner, index) is natively IDEMPOTENT: it compares
//     IntToBool(index) against `active` and returns when they already agree. Redundant
//     applies are free, and that property is what breaks every echo loop this lane could
//     otherwise create.
//   - Every native call site dispatches it EX_LocalVirtualFunction, which is
//     ProcessEvent-invisible, so the lane POLLS the `active` field -- the device-layer
//     pattern -- and never hooks the verb. The callers are the screen test (both on and
//     off), the radar panel's lever (off) and crafting a snusk loaf (on).

#include "coop/world/alarm_sync.h"

#include "coop/net/latest_wins.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers
#include "coop/player/roster_ledger.h"     // PerSlotState

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"   // ForEachInstance -- only the trigger_alarm_C instances, not a walk
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/actors/save_record.h"  // ReadFNameAt

#include <atomic>
#include <chrono>
#include <cstdint>

namespace coop::alarm_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

std::atomic<coop::net::Session*> g_session{nullptr};

// ---- resolution (game thread; lazy, 2 s retry throttle; capped LOUD latch -- the
// event_active_sync pattern) --------------------------------------------------------------------
void* g_cls = nullptr;           // trigger_alarm_C UClass
int32_t g_offActive = -1;        // trigger_alarm_C.active byte offset...
uint8_t g_maskActive = 0;        // ...and its FBoolProperty bit mask (real-bit, never assume 0x01)
void* g_runTriggerFn = nullptr;  // runTrigger(owner: UObject*, index: int)
int32_t g_offKey = -1;           // triggerBase_C.key -- selects WHICH trigger_alarm_C is ours
std::chrono::steady_clock::time_point g_nextResolve{};
int g_postClassAttempts = 0;
bool g_resolveLatched = false;
constexpr int kMaxPostClassAttempts = 5;

bool Resolved() {
    return g_offActive >= 0 && g_maskActive != 0 && g_runTriggerFn != nullptr && g_offKey >= 0;
}

void ResolvePass() {
    if (g_resolveLatched) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return;
    g_nextResolve = now + std::chrono::seconds(2);
    if (!g_cls) g_cls = R::FindClass(L"trigger_alarm_C");
    if (!g_cls) return;  // world not loaded yet -- keep trying
    if (g_offActive < 0) R::FindBoolProperty(g_cls, L"active", g_offActive, g_maskActive);
    if (!g_runTriggerFn) g_runTriggerFn = R::FindFunction(g_cls, L"runTrigger");
    if (g_offKey < 0) g_offKey = R::FindPropertyOffset(g_cls, L"key");
    if (Resolved()) {
        g_resolveLatched = true;
        UE_LOGI("alarm_sync: resolved (active=0x%X mask=0x%02X key=0x%X runTrigger=yes)",
                g_offActive, g_maskActive, g_offKey);
        return;
    }
    if (++g_postClassAttempts >= kMaxPostClassAttempts) {
        g_resolveLatched = true;  // class IS loaded; a member lookup that failed 5x never succeeds
        UE_LOGW("alarm_sync: resolution INCOMPLETE after %d passes on a loaded trigger_alarm_C "
                "(active=0x%X key=0x%X runTrigger=%s) -- latched OFF; game version mismatch?",
                g_postClassAttempts, g_offActive, g_offKey, g_runTriggerFn ? "yes" : "no");
    }
}

// ---- the live trigger instance, cached and revalidated by internal index so a recycled slot
// cannot be mistaken for it.
//
// SELECTED BY KEY. The map carries more than one trigger_alarm_C: the wired one keeps the
// class default key 'alarmTrigger' and owns the lamp/grate wiring, and a second instance
// carries its own key and no wiring. Taking the first match out of the object array would bind
// whichever the array happened to yield, so this matches the key the game's own callers look
// the trigger up by. Binding nothing is the right answer when no instance carries that key:
// driving the wrong trigger is worse than driving none.
void* g_trigger = nullptr;
int32_t g_triggerIdx = -1;
// A search that bound nothing is not repeated for a second (the poll's period): a client's requests while the trigger
// is unbound cost a clock read each, not a search each.
std::chrono::steady_clock::time_point g_nextTriggerSearch{};

// The key the wired trigger keeps (also the class-default key the CDO carries -- see the callback).
struct TriggerSearch { int32_t offKey; void* found; };

void* Trigger() {
    if (!g_trigger || !R::IsLiveByIndex(g_trigger, g_triggerIdx)) {
        g_trigger = nullptr;
        g_triggerIdx = -1;
        if (g_offKey < 0) return nullptr;  // unresolved layout -- cannot tell the two apart
        const auto now = std::chrono::steady_clock::now();
        if (now < g_nextTriggerSearch) return nullptr;
        // Resolve the class fresh from the index for the instance search, not off a cached pointer,
        // matching the old FindObjectsByClass(name) that re-resolved by name too (a cached UClass
        // could in principle be unlisted and re-created at another address). O(1). The property
        // offsets ResolvePass latches are a separate matter, left as they were.
        void* cls = ue_wrap::object_index::ClassByName(L"trigger_alarm_C");
        if (!cls) return nullptr;  // class not loaded/indexed yet -- keep trying
        // Only the trigger_alarm_C instances, from the object index, not a whole-array
        // FindObjectsByClass per call: an AlarmState flood in a world whose trigger is unbound no
        // longer walks ~250k objects each message. The loop's own checks stay in the
        // callback -- the index lists the class default object, which carries the default key
        // 'alarmTrigger', and the index's tenant check is weaker than IsLive.
        TriggerSearch srch{g_offKey, nullptr};
        ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t index) {
            auto& s = *static_cast<TriggerSearch*>(ctx);
            if (s.found || !obj) return;                                      // keep the first
            if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;      // the CDO's default key
            if (!R::IsLive(obj)) return;                                      // PendingKill/Dying
            if (R::SlotFlags(index) & R::slot_flags::NotYetReadable) return;  // still loading
            if (ue_wrap::save_record::ReadFNameAt(obj, s.offKey) != L"alarmTrigger") return;
            s.found = obj;
        }, &srch);
        if (srch.found) {
            g_trigger = srch.found;
            g_triggerIdx = R::InternalIndexOf(srch.found);
        } else {
            g_nextTriggerSearch = now + std::chrono::seconds(1);
        }
    }
    return g_trigger;
}

// ---- poll state (game thread) -----------------------------------------------------------------
void* g_polledTrigger = nullptr;  // the instance the baseline belongs to (reload re-primes)
int g_lastPolled = -1;            // -1 = prime on the next read (prime NEVER sends)
long long g_lastPollMs = 0;
constexpr long long kPollIntervalMs = 1000;  // the klaxon runs minutes; 1 s edge latency is fine

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool ReadActive(void* trig, bool& out) {
    if (!trig || g_offActive < 0 || g_maskActive == 0) return false;
    const uint8_t b = *(reinterpret_cast<const uint8_t*>(trig) + g_offActive);
    out = (b & g_maskActive) != 0;
    return true;
}

// A wire state that arrived before the trigger/resolution was ready (a join snapshot can land
// while ResolvePass is still inside its 2 s retry throttle) is QUEUED, never dropped -- the
// recurring "op applied before the state it reads is ready" class; Tick drains it.
int g_pendingApply = -1;  // -1 = none; 0/1 = desired state awaiting a ready trigger
bool g_pendingSuppress = false;

// Reflected runTrigger(nullptr, active) on the local trigger -- the ONE call that replays the
// whole native fanout (lamps, klaxon, grate, ceiling flicker, setEvent). `suppressDetection`:
// the CLIENT updates its poll baseline so the lane-caused flip is not re-sent as a local change;
// the HOST leaves the baseline stale ON PURPOSE -- its next poll detects the flip and does the
// one canonical broadcast fanout (that IS the host relay path for client-originated changes).
void Apply(bool active, bool suppressDetection) {
    void* trig = Resolved() ? Trigger() : nullptr;
    if (!trig || !g_runTriggerFn) {
        g_pendingApply = active ? 1 : 0;
        g_pendingSuppress = suppressDetection;
        UE_LOGI("alarm_sync: apply active=%d QUEUED (trigger/resolution not ready yet -- "
                "Tick drains)",
                active ? 1 : 0);
        return;
    }
    ue_wrap::ParamFrame frame(g_runTriggerFn);
    if (!frame.valid()) return;
    void* owner = nullptr;  // the alarm graph never reads owner (bytecode: only index is used)
    frame.Set(L"owner", owner);
    frame.Set(L"index", static_cast<int32_t>(active ? 1 : 0));
    ue_wrap::Call(trig, frame);
    g_pendingApply = -1;
    UE_LOGI("alarm_sync: applied active=%d (native runTrigger replay)", active ? 1 : 0);
    if (suppressDetection) {
        g_polledTrigger = trig;
        g_lastPolled = active ? 1 : 0;  // ProcessEvent is synchronous -- the field already flipped
    }
}

// The host's applications of a client's request, per sender (an instrument: the world_latest drill counts them).
coop::roster_ledger::PerSlotState<uint32_t> g_clientApplies;

// The host applies a client's request natively WITHOUT suppressing detection: its next poll sees the flip and
// broadcasts to everyone, the originator included, whose re-apply is a native no-op while the host holds its newest
// request; a sender with a request owed is shown the host's older state until it is paid. MTA leaves the source out of
// its relay of a client's element data (CGame.cpp:2759-2771); this lane's one poll broadcast serves the host's own scan
// and every client's request alike.
void ApplyClientRequest(int slot, bool active) {
    UE_LOGI("alarm_sync: client slot=%d requests active=%d", slot, active ? 1 : 0);
    Apply(active, /*suppressDetection=*/false);
    ++g_clientApplies[slot];
}

// The trigger's state, or false when the lane is unresolved or no trigger is bound.
bool ReadCurrent(bool& out) {
    if (!Resolved()) return false;
    void* trig = Trigger();
    return trig && ReadActive(trig, out);
}

// A client's requests, the newest kept at a pace (coop/net/latest_wins.h). Its sender is one edge per 1 Hz poll
// (kPollIntervalMs), and a request that changes the trigger runs the alarm's whole fanout: two at once, then one a
// second. An unchanged request costs the trigger's byte. MTA applies a client's element data at once (CGame.cpp:2757);
// paced here for the fanout's cost.
constexpr coop::net::IntentBudget kRequestBudget{2.0f, 1.0f};
struct PendingAlarm {
    bool active = false;
    coop::net::LatestWins pace;
};
coop::roster_ledger::PerSlotState<PendingAlarm> g_pending;

// True when the trigger reads `active` already; an unreadable trigger counts as changed.
bool Holds(bool active) {
    bool cur = false;
    return ReadCurrent(cur) && cur == active;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    const long long now = NowMs();
    if (now - g_lastPollMs < kPollIntervalMs) return;
    g_lastPollMs = now;
    ResolvePass();
    if (!Resolved()) return;
    void* trig = Trigger();
    if (!trig) return;
    // Drain a queued wire state first (arrived before the trigger was ready). Apply updates
    // the baseline itself when suppressing; skip this round's detection either way.
    if (g_pendingApply >= 0) {
        Apply(g_pendingApply != 0, g_pendingSuppress);
        return;
    }
    bool active = false;
    if (!ReadActive(trig, active)) return;
    const int cur = active ? 1 : 0;
    // World/save reload minted a new trigger -> the old baseline is meaningless. Re-prime
    // silently (the join edge / the host's next real transition delivers state; a prime must
    // never masquerade as an edge).
    if (trig != g_polledTrigger || g_lastPolled < 0) {
        g_polledTrigger = trig;
        g_lastPolled = cur;
        return;
    }
    if (cur == g_lastPolled) return;
    g_lastPolled = cur;
    coop::net::AlarmStatePayload p{};
    p.active = static_cast<uint8_t>(cur);
    if (s->role() == coop::net::Role::Host) {
        // Host observed a transition (its own scan/press, or a client-request Apply) ->
        // the canonical broadcast fanout.
        if (s->SendReliable(coop::net::ReliableKind::AlarmState, &p, sizeof(p)))
            UE_LOGI("alarm_sync: host broadcast active=%d", cur);
        else
            UE_LOGW("alarm_sync: host broadcast active=%d send FAILED", cur);
    } else {
        // CLIENT observed a LOCAL transition (its own scan fired ON early / its player pressed
        // Stop) -- lane-caused flips updated the baseline in Apply, so this is genuinely local.
        // Send it to the host; the host applies natively and its poll broadcasts.
        if (s->SendReliable(coop::net::ReliableKind::AlarmState, &p, sizeof(p)))
            UE_LOGI("alarm_sync: client local transition active=%d -> host", cur);
        else
            UE_LOGW("alarm_sync: client local transition active=%d send FAILED", cur);
    }
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    if (!Resolved()) return;  // no world yet -> nothing to snapshot; the first transition delivers
    g_nextTriggerSearch = {};  // one-shot, no retry: a miss latched a moment ago must not skip a trigger now bound
    void* trig = Trigger();
    bool active = false;
    if (!trig || !ReadActive(trig, active)) return;
    coop::net::AlarmStatePayload p{};
    p.active = active ? 1 : 0;
    // Unconditional (even active=0): the joiner's transferred save can arrive already-ON --
    // the explicit current state + the natively idempotent apply absorb both orders.
    if (s->SendReliableToSlot(slot, coop::net::ReliableKind::AlarmState, &p, sizeof(p)))
        UE_LOGI("alarm_sync: connect-snapshot -- sent active=%d to slot %d", p.active, slot);
    else
        UE_LOGW("alarm_sync: connect-snapshot to slot %d send FAILED", slot);
}

void OnReliable(const coop::net::AlarmStatePayload& payload, int senderPeerSlot) {
    if (!GT::IsGameThread()) {
        UE_LOGW("alarm_sync: OnReliable off-game-thread -- dropping");
        return;
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    const bool active = payload.active != 0;
    ResolvePass();
    if (s->role() == coop::net::Role::Host) {
        // A client's local transition, the newest kept at the request budget's pace.
        if (senderPeerSlot < 1 || senderPeerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
        PendingAlarm& pend = g_pending[senderPeerSlot];
        if (Holds(active)) {
            pend.pace.Withdraw();
            return;
        }
        pend.active = active;
        if (pend.pace.Offer(kRequestBudget, coop::net::NowMs())) ApplyClientRequest(senderPeerSlot, active);
    } else {
        if (senderPeerSlot != 0) {
            UE_LOGW("alarm_sync: AlarmState from non-host senderPeerSlot=%d -- dropping",
                    senderPeerSlot);
            return;
        }
        Apply(active, /*suppressDetection=*/true);
    }
}

void DevForce(bool active) {
    // Test/dev seam (RULE-2-exempt diagnostics): drive the host's native trigger exactly like
    // the radar would -- the lane's poll then does the real detection + broadcast, so the e2e
    // exercises the SHIPPING path, not a shortcut.
    GT::Post([active] {
        ResolvePass();
        Apply(active, /*suppressDetection=*/false);
    });
}

void PayOwed(coop::net::Session& session) {
    if (session.role() != coop::net::Role::Host) return;
    uint64_t now = 0;  // read once, and only when a request is owed
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot) {
        PendingAlarm& pend = g_pending[slot];
        if (!pend.pace.Owed()) continue;
        if (Holds(pend.active)) {  // the host's own scan or another client got there first
            pend.pace.Withdraw();
            continue;
        }
        if (now == 0) now = coop::net::NowMs();
        if (pend.pace.Due(kRequestBudget, now)) ApplyClientRequest(slot, pend.active);
    }
}

uint32_t ClientApplies(int slot) { return g_clientApplies[slot]; }

bool ReadLocalActive(bool& out) { return ReadCurrent(out); }

void OnDisconnect() {
    g_trigger = nullptr;
    g_triggerIdx = -1;
    g_nextTriggerSearch = {};
    g_polledTrigger = nullptr;
    g_lastPolled = -1;
    g_lastPollMs = 0;
    g_pendingApply = -1;
    g_pendingSuppress = false;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::alarm_sync
