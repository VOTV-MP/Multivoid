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

#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/power_control.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"     // R-2: gen-stamped index (dead-world guard)
#include "coop/element/object_scan_hub.h"      // R-2: the shared sliced scan pass

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
std::vector<std::pair<std::wstring, Ref>> g_pollScratch;  // GT-only: reused per-tick poll snapshot

using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;
using coop::net::FnvKey;

void MaskToPayload(const std::wstring& key, uint8_t mask, coop::net::PowerPanelPayload& p) {
    std::memset(&p, 0, sizeof(p));
    WireKeyFromString(key, p.key);
    p.pressMask = static_cast<uint8_t>(mask & kMaskBits);
}

// R-2: world generation of the last completed hub pass; a stale-gen index is treated as EMPTY
// on every read path (dead-world guard -- see the hub-consumer block below).
uint32_t g_indexGen = 0;
bool IndexCurrent() { return g_indexGen == ue_wrap::world_identity::Generation(); }

void* ResolveFast(const std::wstring& key) {
    if (!IndexCurrent()) return nullptr;  // stale-gen index = another world's actors (R-1 class)
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_index.find(key);
    if (it != g_index.end() && R::IsLiveByIndex(it->second.actor, it->second.idx)) return it->second.actor;
    return nullptr;
}

// ---- R-2 shared-scan hub consumer (design: votv-shared-scan-hub-R2-DESIGN-2026-08-23.md).
// The per-module SettledObjectScan full walk is RETIRED; the hub's shared sliced pass drives
// these callbacks. Behavior preserved verbatim from the old RebuildIndex.
std::vector<std::pair<std::wstring, Ref>> g_scanFound;  // pass scratch (GT-only)

void HubPassBegin(void*, bool) { g_scanFound.clear(); }

void HubMatch(void*, void* obj) {
    const std::wstring nm = R::ToString(R::NameOf(obj));
    if (nm.rfind(L"Default__", 0) == 0) return;  // skip CDO
    if (!R::IsLive(obj)) return;
    std::wstring key = PC::GetKeyString(obj);
    if (key.empty() || key == L"None") return;  // unkeyed template
    g_scanFound.emplace_back(std::move(key), Ref{ obj, R::InternalIndexOf(obj) });
}

size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    const size_t added = g_scanFound.size();
    uint64_t keysHash = 0;
    size_t   total;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (isFull) g_index.clear();                           // full pass: rebuild from scratch
        for (auto& f : g_scanFound) g_index[f.first] = f.second;
        if (!isFull) {                                         // tail pass: prune dead entries (cheap, O(index))
            for (auto it = g_index.begin(); it != g_index.end(); ) {
                if (R::IsLiveByIndex(it->second.actor, it->second.idx)) ++it;
                else it = g_index.erase(it);
            }
        }
        for (auto& kv : g_index) keysHash ^= FnvKey(kv.first);  // recompute over the index (cheap, O(index))
        total = g_index.size();
        g_indexGen = worldGen;
    }
    g_scanFound.clear();
    if (total != g_lastLogCount || keysHash != g_lastLogHash) {
        g_lastLogCount = total;
        g_lastLogHash = keysHash;
        UE_LOGI("power: index rebuilt -- %zu live power panel(s), keysHash=0x%016llX (%s pass, +%zu new) "
                "(compare host vs client for cross-peer Key stability)",
                total, static_cast<unsigned long long>(keysHash), isFull ? "full" : "tail", added);
    }
    return total;
}

void RegisterWithScanHub() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        "power", nullptr, &PC::EnsureResolved, &PC::IsPowerControl,
        &HubPassBegin, &HubMatch, &HubPassComplete, /*settleScans*/ 15});
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
    RegisterWithScanHub();  // the hub builds the index on its own cadence
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
    // R-2: the forced sync rebuild is gone -- the hub keeps the index <=1 pass (~2 s) fresh
    // (power panels are static level actors; the staleness window is empty in practice).
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
    RegisterWithScanHub();  // safety net for any order where Tick precedes Install
    if (!IndexCurrent()) return;  // index belongs to a dead world -- wait for the hub's next pass

    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastRetry >= kRetryRebuildThrottle) {
        g_lastRetry = now;
        // RECEIVER: retry deferred applies for panels that have now streamed in (the hub
        // refreshed the index on its own cadence; this throttle now paces only the retries).
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
    // Also drop the key->actor index: a new session starts clean (the hub's next pass
    // rebuilds it), so we never carry a prior session's (possibly world-reloaded) actor
    // pointers across. IsLiveByIndex + the gen stamp would catch stale ones, but a clean
    // rebuild per session is the correct posture.
    g_index.clear();
    if (n > 0) UE_LOGI("power: OnDisconnect cleared %zu last-known", n);
}

}  // namespace coop::power_sync
