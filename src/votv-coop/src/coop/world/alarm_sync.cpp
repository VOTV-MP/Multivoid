// coop/world/alarm_sync.cpp -- see coop/world/alarm_sync.h.
//
// Bytecode ground truth (docs/events/alarm.md, disasm 2026-07-05):
//   - trigger_alarm_C.runTrigger(owner, index) is natively IDEMPOTENT (IntToBool(index) ==
//     active -> no-op) -- redundant applies are free; that property breaks every echo loop
//     this lane could otherwise create.
//   - Both native callers (analogDScreenTest scan ON, panel_radar stop-press OFF) dispatch it
//     EX_VirtualFunction -- ProcessEvent-INVISIBLE -- so the lane POLLS the `active` field
//     (the L2 device-layer pattern), never hooks the verb.

#include "coop/world/alarm_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

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
std::chrono::steady_clock::time_point g_nextResolve{};
int g_postClassAttempts = 0;
bool g_resolveLatched = false;
constexpr int kMaxPostClassAttempts = 5;

bool Resolved() { return g_offActive >= 0 && g_maskActive != 0 && g_runTriggerFn != nullptr; }

void ResolvePass() {
    if (g_resolveLatched) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return;
    g_nextResolve = now + std::chrono::seconds(2);
    if (!g_cls) g_cls = R::FindClass(L"trigger_alarm_C");
    if (!g_cls) return;  // world not loaded yet -- keep trying
    if (g_offActive < 0) R::FindBoolProperty(g_cls, L"active", g_offActive, g_maskActive);
    if (!g_runTriggerFn) g_runTriggerFn = R::FindFunction(g_cls, L"runTrigger");
    if (Resolved()) {
        g_resolveLatched = true;
        UE_LOGI("alarm_sync: resolved (active=0x%X mask=0x%02X runTrigger=yes)", g_offActive,
                g_maskActive);
        return;
    }
    if (++g_postClassAttempts >= kMaxPostClassAttempts) {
        g_resolveLatched = true;  // class IS loaded; a member lookup that failed 5x never succeeds
        UE_LOGW("alarm_sync: resolution INCOMPLETE after %d passes on a loaded trigger_alarm_C "
                "(active=0x%X runTrigger=%s) -- latched OFF; game version mismatch?",
                g_postClassAttempts, g_offActive, g_runTriggerFn ? "yes" : "no");
    }
}

// ---- the live trigger instance (ONE per map, key 'alarmTrigger'; cached, revalidated by
// internal index -- recycled-slot-safe) ---------------------------------------------------------
void* g_trigger = nullptr;
int32_t g_triggerIdx = -1;

void* Trigger() {
    if (!g_trigger || !R::IsLiveByIndex(g_trigger, g_triggerIdx)) {
        g_trigger = nullptr;
        g_triggerIdx = -1;
        if (!g_cls) return nullptr;
        for (void* obj : R::FindObjectsByClass(L"trigger_alarm_C")) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) {
                g_trigger = obj;
                g_triggerIdx = R::InternalIndexOf(obj);
                break;
            }
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
        // A client's local transition. Apply natively WITHOUT suppressing detection -- the
        // host's next poll sees the flip and broadcasts to everyone (incl. the originator,
        // whose re-apply is a native no-op).
        UE_LOGI("alarm_sync: client slot=%d requests active=%d", senderPeerSlot, active ? 1 : 0);
        Apply(active, /*suppressDetection=*/false);
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

void OnDisconnect() {
    g_trigger = nullptr;
    g_triggerIdx = -1;
    g_polledTrigger = nullptr;
    g_lastPolled = -1;
    g_lastPollMs = 0;
    g_pendingApply = -1;
    g_pendingSuppress = false;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::alarm_sync
