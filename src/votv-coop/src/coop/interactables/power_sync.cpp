// coop/power_sync.cpp -- see coop/power_sync.h. Base POWER PANEL (ApowerControl_C) breaker
// mirror: poll the 5 press bools (packed into a 5-bit mask) -> broadcast on a change; the
// receiver writes the bools + refreshes the panel visual through ue_wrap::power_control.
//
// Structure borrows the proven keypad_sync / interactable_sync patterns (key->actor index with
// IsLiveByIndex self-heal, throttled rebuild, deferred-apply retry, silent first-sight prime,
// echo-suppress via priming g_lastKnown to the applied value) but with a 5-bit MASK state, which
// is why it is a separate module rather than another toggle Adapter (the generic Channel is one
// bool per key; 5 bools per actor don't fit -- RULE 2, same call the keypad made).

#include "coop/interactables/power_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"  // WireKeyFromString / StringFromWireKey / FnvKey (shared)
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers

#include "ue_wrap/log.h"
#include "ue_wrap/power_control.h"
#include "ue_wrap/reflection.h"
#include "coop/util/incremental_object_scan.h"  // L5: scan only NEW objects (no 237k walk)
#include "ue_wrap/walk_timer.h"           // L5: [WALK-TIME] profiling

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::power_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace PC = ue_wrap::power_control;

constexpr auto kRetryRebuildThrottle = std::chrono::seconds(2);
constexpr auto kPendingTTL = std::chrono::seconds(25);
constexpr uint8_t kMaskBits = 0x1F;  // 5 breakers (bit0=coord..bit4=light)

std::atomic<coop::net::Session*> g_session{nullptr};

struct Ref { void* actor; int32_t idx; };

std::mutex g_mutex;  // guards the maps below (all access is game-thread-serial; defensive)
std::unordered_map<std::wstring, Ref>     g_index;      // key -> live panel
std::unordered_map<std::wstring, uint8_t> g_lastKnown;  // key -> last broadcast/applied mask (change-detect + echo-suppress)
struct Pending { uint8_t want; std::chrono::steady_clock::time_point deadline; };
std::unordered_map<std::wstring, Pending> g_pending;    // key -> deferred incoming apply

std::chrono::steady_clock::time_point g_lastRetry{};
size_t g_lastLogCount = SIZE_MAX;
uint64_t g_lastLogHash = 0;
bool g_indexed = false;
std::vector<std::pair<std::wstring, Ref>> g_pollScratch;  // GT-only: reused per-tick poll snapshot

using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;
using coop::net::FnvKey;

void MaskToPayload(const std::wstring& key, uint8_t mask, coop::net::PowerPanelPayload& p) {
    std::memset(&p, 0, sizeof(p));
    WireKeyFromString(key, p.key);
    p.pressMask = static_cast<uint8_t>(mask & kMaskBits);
}

void* ResolveFast(const std::wstring& key) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_index.find(key);
    if (it != g_index.end() && R::IsLiveByIndex(it->second.actor, it->second.idx)) return it->second.actor;
    return nullptr;
}

// Full GUObjectArray walk -> rebuild the key->actor index. Game thread. Logs a keys-hash
// (cross-peer Key stability signal) only on change.
size_t RebuildIndex() {
    if (!PC::EnsureResolved()) return 0;
    // L5: scan only the range NextRange gives -- the array TAIL (new objects) every call, a FULL
    // re-scan every ~5min (the slot-reuse backstop). A power panel is a level-loaded ApowerControl_C,
    // so the tail-scan catches it at load; static actors rarely reuse slots, so the rare full safety
    // covers any drift.
    static coop::util::IncrementalObjectScan sScan;
    const auto r = coop::util::NextRange(sScan);
    std::vector<std::pair<std::wstring, Ref>> found;
    found.reserve(4);  // there are typically 1-2 panels in a base
    for (int32_t i = r.begin; i < r.end; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || !PC::IsPowerControl(obj)) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;  // skip CDO
        if (!R::IsLive(obj)) continue;
        std::wstring key = PC::GetKeyString(obj);
        if (key.empty() || key == L"None") continue;  // unkeyed template
        found.emplace_back(std::move(key), Ref{ obj, R::InternalIndexOf(obj) });
    }
    uint64_t keysHash = 0;
    size_t   total;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (r.isFull) g_index.clear();                         // full re-scan: rebuild from scratch
        for (auto& f : found) g_index[f.first] = f.second;     // add the new (or all, on a full scan)
        if (!r.isFull) {                                       // tail scan: prune dead entries (cheap, O(index))
            for (auto it = g_index.begin(); it != g_index.end(); ) {
                if (R::IsLiveByIndex(it->second.actor, it->second.idx)) ++it;
                else it = g_index.erase(it);
            }
        }
        for (auto& kv : g_index) keysHash ^= FnvKey(kv.first);  // recompute over the index (cheap, O(index))
        total = g_index.size();
    }
    if (total != g_lastLogCount || keysHash != g_lastLogHash) {
        g_lastLogCount = total;
        g_lastLogHash = keysHash;
        UE_LOGI("power: index rebuilt -- %zu live power panel(s), keysHash=0x%016llX (%s scan, +%zu new) "
                "(compare host vs client for cross-peer Key stability)",
                total, static_cast<unsigned long long>(keysHash), r.isFull ? "full" : "tail", found.size());
    }
    return total;
}

// RECEIVER apply: drive `actor`'s 5 breaker bools to `mask` (write + panel-visual refresh via
// ue_wrap::power_control). Idempotent: a panel already matching is primed + skipped (no verb
// call). Updates g_lastKnown[key] so this peer's poll never echoes the applied value.
void ApplyMask(void* actor, const std::wstring& key, uint8_t mask, unsigned fromSlot) {
    mask &= kMaskBits;
    uint8_t cur = 0;
    if (PC::ReadPress(actor, cur) && (cur & kMaskBits) == mask) {
        { std::lock_guard<std::mutex> lk(g_mutex); g_lastKnown[key] = mask; }
        return;  // idempotent -- already at the target
    }
    const bool ok = PC::ApplyPress(actor, mask);
    { std::lock_guard<std::mutex> lk(g_mutex); g_lastKnown[key] = mask; }
    UE_LOGI("power: applied key='%ls' mask=0x%02X ok=%d (from slot %u)",
            key.c_str(), mask, ok ? 1 : 0, fromSlot);
}

// SENDER: poll every indexed panel for a breaker change and broadcast deltas. First sight of a
// key primes the baseline SILENTLY (initial divergence is the connect-snapshot's job). Echo is
// impossible: ApplyMask primes g_lastKnown to the applied value, so the next poll sees no delta.
// Game thread.
void PollAndBroadcast() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;

    auto& refs = g_pollScratch;  // reused buffer (GT-serial) -- no per-tick heap alloc
    refs.clear();
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_index.empty()) return;
        refs.reserve(g_index.size());
        for (auto& kv : g_index) refs.emplace_back(kv.first, kv.second);
    }
    for (auto& r : refs) {
        if (!R::IsLiveByIndex(r.second.actor, r.second.idx)) continue;
        uint8_t cur = 0;
        if (!PC::ReadPress(r.second.actor, cur)) continue;
        cur &= kMaskBits;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_lastKnown.find(r.first);
            if (it == g_lastKnown.end()) { g_lastKnown[r.first] = cur; continue; }  // prime silently
            if (it->second == cur) continue;                                         // no change
        }
        coop::net::PowerPanelPayload p{};
        MaskToPayload(r.first, cur, p);
        if (s->SendReliable(coop::net::ReliableKind::PowerControlState, &p, sizeof(p))) {
            { std::lock_guard<std::mutex> lk(g_mutex); g_lastKnown[r.first] = cur; }
            UE_LOGI("power: sent key='%ls' mask=0x%02X", r.first.c_str(), cur);
        } else {
            UE_LOGW("power: SendReliable failed key='%ls'", r.first.c_str());
        }
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_indexed && PC::EnsureResolved()) {
        UE_LOGI("power: indexed %zu power panel(s)", RebuildIndex());
        g_indexed = true;
    }
}

void OnReliable(const coop::net::PowerPanelPayload& payload, uint8_t senderPeerSlot) {
    std::wstring key = StringFromWireKey(payload.key);
    if (key.empty()) { UE_LOGW("power: OnReliable empty key -- dropping"); return; }
    if (!PC::EnsureResolved()) { UE_LOGW("power: apply -- class not resolved, dropping key='%ls'", key.c_str()); return; }
    const uint8_t want = static_cast<uint8_t>(payload.pressMask & kMaskBits);
    if (void* actor = ResolveFast(key)) { ApplyMask(actor, key, want, senderPeerSlot); return; }
    // Not streamed in yet -- defer + retry on the throttled tick.
    std::lock_guard<std::mutex> lk(g_mutex);
    g_pending[key] = Pending{ want, std::chrono::steady_clock::now() + kPendingTTL };
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;  // host-only snapshot
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    RebuildIndex();
    std::vector<std::pair<std::wstring, Ref>> items;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        items.reserve(g_index.size());
        for (auto& kv : g_index) items.emplace_back(kv.first, kv.second);
    }
    int sent = 0;
    for (auto& d : items) {
        if (!R::IsLiveByIndex(d.second.actor, d.second.idx)) continue;
        uint8_t cur = 0;
        if (!PC::ReadPress(d.second.actor, cur)) continue;
        cur &= kMaskBits;
        coop::net::PowerPanelPayload p{};
        MaskToPayload(d.first, cur, p);
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::PowerControlState, &p, sizeof(p));
        { std::lock_guard<std::mutex> lk(g_mutex); g_lastKnown[d.first] = cur; }
        ++sent;
    }
    UE_LOGI("power: connect-snapshot -- sent %d panel(s) to slot %d (of %zu indexed)", sent, peerSlot, items.size());
}

void Tick() {
    if (!PC::EnsureResolved()) return;
    if (!g_indexed) { UE_LOGI("power: indexed %zu power panel(s)", RebuildIndex()); g_indexed = true; }

    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastRetry >= kRetryRebuildThrottle) {
        g_lastRetry = now;
        ue_wrap::ScopedWalkTimer _wt("power:RebuildIndex");  // logs only the rare ~5min full safety
        RebuildIndex();   // L5: INCREMENTAL -- tail-scan + a rare full safety (no 237k walk)
        // RECEIVER: retry deferred applies for panels that have now streamed in.
        std::vector<std::pair<std::wstring, uint8_t>> ready;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            for (auto it = g_pending.begin(); it != g_pending.end();) {
                auto idxIt = g_index.find(it->first);
                if (idxIt != g_index.end() && R::IsLiveByIndex(idxIt->second.actor, idxIt->second.idx)) {
                    ready.emplace_back(it->first, it->second.want);
                    it = g_pending.erase(it);
                } else if (now >= it->second.deadline) {
                    it = g_pending.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& rdy : ready)
            if (void* actor = ResolveFast(rdy.first)) ApplyMask(actor, rdy.first, rdy.second, 0xFF);
    }
    PollAndBroadcast();
}

void OnDisconnect() {
    std::lock_guard<std::mutex> lk(g_mutex);
    const size_t n = g_lastKnown.size();
    g_lastKnown.clear();
    g_pending.clear();
    // Also drop the key->actor index: a new session re-indexes from scratch (Install ->
    // RebuildIndex once g_indexed is false), so we never carry a prior session's (possibly
    // world-reloaded) actor pointers across. IsLiveByIndex would catch stale ones, but a clean
    // rebuild per session is the correct posture (don't lean on the 2s throttle to self-heal).
    g_index.clear();
    g_indexed = false;
    if (n > 0) UE_LOGI("power: OnDisconnect cleared %zu last-known", n);
}

}  // namespace coop::power_sync
