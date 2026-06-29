// coop/window_sync.cpp -- see coop/window_sync.h. Base-window dirt scalar sync.
//
// A trimmed sibling of interactable_sync's Channel: the same proven Key->actor index
// (IsLiveByIndex self-heal), throttled rebuild, silent first-sight prime, deferred-apply
// retry, and echo-suppress-via-priming. The differences that make it its own module rather
// than another toggle Adapter (RULE 2): the state is a continuous FLOAT (clean), not a bool,
// and the apply rule is MIN-WINS (monotone cooperative clean), not verbatim toggle -- so it
// needs none of the door Channel's HostAuth machinery (hold register, settling bridge,
// autonomy suppression) and forcing it into the bool Channel would mean templatizing that
// battle-tested door code. clean is monotone-decreasing + inert (nothing re-raises it
// locally), so a SYMMETRIC min-wins poll converges with no oscillation -- the simplest
// correct model (Agent A's design verdict).

#include "coop/interactables/window_sync.h"

#include "coop/session/ini_config.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"  // WireKeyFromString / StringFromWireKey / FnvKey (shared)
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers

#include "ue_wrap/base_window.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "coop/util/incremental_object_scan.h"  // L5: scan only NEW objects (no 237k walk)
#include "ue_wrap/walk_timer.h"                  // L5: [WALK-TIME] profiling (logs only the rare full safety)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::window_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace BW = ue_wrap::base_window;

using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;
using coop::net::FnvKey;

constexpr auto kRetryRebuildThrottle = std::chrono::seconds(2);
constexpr auto kPendingTTL = std::chrono::seconds(25);
// `clean` changes in discrete wipe steps (cleanSponge: clean -= strength*0.01); this epsilon
// is smaller than the smallest real step, so it only filters float-equality noise -- never a
// genuine wipe. Used both to detect a decrease (broadcast) and to skip an idempotent apply.
constexpr float kCleanEps = 0.0005f;

bool ProbeLog() {
    static const bool s_enabled = ::coop::ini_config::IsIniKeyTrue("window_log");
    return s_enabled;
}

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_echo{false};  // belt-and-suspenders: suppress the poll mid-apply (GT-serial anyway)

struct Ref { void* actor; int32_t idx; };
std::mutex g_indexMutex;
std::unordered_map<std::wstring, Ref> g_byKey;

std::mutex g_stateMutex;
std::unordered_map<std::wstring, float> g_lastKnown;  // key -> last broadcast/applied clean (change-detect + echo-suppress)

struct Pending { float clean; bool adopt; uint8_t fromSlot; std::chrono::steady_clock::time_point deadline; };
std::unordered_map<std::wstring, Pending> g_pending;  // GT-only: deferred applies (window not streamed in yet)

std::chrono::steady_clock::time_point g_lastRetry{};
size_t g_lastLogCount = SIZE_MAX;  // GT-only: dedup the rebuilt log
uint64_t g_lastLogHash = 0;        // GT-only
std::vector<std::pair<std::wstring, Ref>> g_pollScratch;  // GT-only: reused per-tick poll snapshot (capacity retained across clear() -> no realloc in steady state; the few short keys are SSO)

void* ResolveFast(const std::wstring& key) {
    std::lock_guard<std::mutex> lk(g_indexMutex);
    auto it = g_byKey.find(key);
    if (it != g_byKey.end() && R::IsLiveByIndex(it->second.actor, it->second.idx))
        return it->second.actor;
    return nullptr;
}

// Full GUObjectArray walk -> rebuild the key->actor index. Game thread. NOT a per-frame cost
// (Tick throttles it to every kRetryRebuildThrottle); the per-tick poll iterates only the
// built index. Logs an order-independent Key hash so host vs client logs reveal cross-peer
// Key stability (AbaseWindow_C is an Aactor_save_C -- its Key should be stable like doors).
size_t RebuildIndex() {
    if (!BW::EnsureResolved()) return 0;
    // L5: scan only the range NextRange gives -- the array TAIL (new objects) every call, a FULL re-scan
    // every ~5min (the slot-reuse backstop). A window is a level-loaded AbaseWindow_C, so the tail-scan
    // catches it at load; static actors rarely reuse slots, so the rare full safety covers any drift.
    static coop::util::IncrementalObjectScan sScan;
    const auto r = coop::util::NextRange(sScan);
    std::vector<std::pair<std::wstring, Ref>> found;
    found.reserve(8);
    for (int32_t i = r.begin; i < r.end; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (!BW::IsBaseWindow(obj)) continue;  // cheap class-descendant filter (no alloc)
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;  // skip CDO
        if (!R::IsLive(obj)) continue;
        std::wstring key = BW::GetKeyString(obj);
        if (key.empty() || key == L"None") continue;
        found.emplace_back(std::move(key), Ref{ obj, R::InternalIndexOf(obj) });
    }
    uint64_t keysHash = 0;
    size_t   total;
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        if (r.isFull) g_byKey.clear();                         // full re-scan: rebuild from scratch
        for (auto& f : found) g_byKey[f.first] = f.second;     // add the new (or all, on a full scan)
        if (!r.isFull) {                                       // tail scan: prune dead entries (cheap, O(index))
            for (auto it = g_byKey.begin(); it != g_byKey.end(); ) {
                if (R::IsLiveByIndex(it->second.actor, it->second.idx)) ++it;
                else it = g_byKey.erase(it);
            }
        }
        for (auto& kv : g_byKey) keysHash ^= FnvKey(kv.first);  // recompute over the index (cheap, O(index))
        total = g_byKey.size();
    }
    if (total != g_lastLogCount || keysHash != g_lastLogHash) {
        g_lastLogCount = total;
        g_lastLogHash = keysHash;
        UE_LOGI("window: index now %zu live keyed window(s), keysHash=0x%016llX (%s scan, +%zu new) "
                "(compare host vs client for cross-peer Key stability)",
                total, static_cast<unsigned long long>(keysHash), r.isFull ? "full" : "tail", found.size());
    }
    if (ProbeLog())
        for (auto& f : found)
            UE_LOGI("window[probe]: key='%ls' idx=%d actor=%p", f.first.c_str(), f.second.idx, f.second.actor);
    return total;
}

// Apply a remote clean value. adopt -> VERBATIM (connect-snapshot: the joiner adopts the
// host's world); else MIN(local, wire) (a live wipe can only make the window cleaner, never
// re-dirty it -> concurrent wipes converge, no regression). Idempotent if already at target.
// Primes g_lastKnown to the applied value so the next poll sees no delta (echo guard).
void ApplyResolved(void* actor, const std::wstring& key, float wireClean, bool adopt, unsigned fromSlot) {
    float cur = 0.f;
    if (!BW::ReadClean(actor, cur)) return;
    const float target = adopt ? wireClean : std::min(cur, wireClean);
    if (std::fabs(target - cur) < kCleanEps) {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        g_lastKnown[key] = target;  // converge the poll baseline; no write needed
        if (ProbeLog())
            UE_LOGI("window: apply key='%ls' already %.3f -- idempotent skip", key.c_str(), target);
        return;
    }
    g_echo.store(true, std::memory_order_release);
    const bool ok = BW::WriteCleanAndApply(actor, target);
    g_echo.store(false, std::memory_order_release);
    { std::lock_guard<std::mutex> lk(g_stateMutex); g_lastKnown[key] = target; }
    UE_LOGI("window: applied clean=%.3f (wire=%.3f adopt=%d) ok=%d key='%ls' (from slot %u)",
            target, wireClean, adopt ? 1 : 0, ok ? 1 : 0, key.c_str(), fromSlot);
}

// SENDER: poll every indexed window for a clean DECREASE (a wipe) and broadcast it. The first
// sighting of a key primes the baseline SILENTLY (initial divergence is the connect-snapshot's
// job). A wire update only ever makes a window cleaner, so we never broadcast an increase
// (a save reload that re-dirties just resyncs the baseline silently). Game thread.
void PollAndBroadcast() {
    if (g_echo.load(std::memory_order_acquire)) return;
    auto* s = g_session.load(std::memory_order_acquire);
    // Gate on connected(): solo host has nobody to send to. We deliberately do NOT advance the
    // baseline here -- OnDisconnect clears the map when all peers leave, and the connect-snapshot
    // re-primes on the next join, so no stale baseline survives to cause a spurious reconnect edge.
    if (!s || !s->connected()) return;
    auto& refs = g_pollScratch;
    refs.clear();
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        if (g_byKey.empty()) return;
        refs.reserve(g_byKey.size());
        for (auto& kv : g_byKey) refs.emplace_back(kv.first, kv.second);
    }
    for (auto& r : refs) {
        if (!R::IsLiveByIndex(r.second.actor, r.second.idx)) continue;
        float cur = 0.f;
        if (!BW::ReadClean(r.second.actor, cur)) continue;
        float base = 0.f;
        bool firstSight = false;
        {
            std::lock_guard<std::mutex> lk(g_stateMutex);
            auto it = g_lastKnown.find(r.first);
            if (it == g_lastKnown.end()) { g_lastKnown[r.first] = cur; firstSight = true; }
            else base = it->second;
        }
        if (firstSight) continue;  // prime silently
        if (cur < base - kCleanEps) {
            coop::net::KeyedScalarPayload p{};
            WireKeyFromString(r.first, p.key);
            p.value = cur;
            p.adopt = 0;  // live wipe -> receivers apply MIN
            if (s->SendReliable(coop::net::ReliableKind::WindowCleanState, &p, sizeof(p))) {
                std::lock_guard<std::mutex> lk(g_stateMutex);
                g_lastKnown[r.first] = cur;
                UE_LOGI("window: sent clean=%.3f key='%ls'", cur, r.first.c_str());
            } else {
                UE_LOGW("window: SendReliable failed key='%ls'", r.first.c_str());
            }
        } else if (cur > base + kCleanEps) {
            // Got dirtier (save reload / reset) -- resync the baseline silently; we never
            // propagate a re-dirty (min-wins receivers would ignore it anyway).
            std::lock_guard<std::mutex> lk(g_stateMutex);
            g_lastKnown[r.first] = cur;
        }
    }
}

// DEV-ONLY synthetic wipe (ini `window_synth=1`). One-shot, host-only: ~5s after
// connect, decrement the first indexed window's `clean` via the SAME path a real
// soapy-sponge wipe drives (WriteCleanAndApply -> field write + setClean repaint),
// so the normal PollAndBroadcast below detects the decrease and broadcasts it. Used
// to PROVE the live-wipe sync chain (host detect -> WindowCleanState -> client apply)
// end-to-end autonomously, isolating any real-gesture bug to the clean@0x0260
// detection. NOT shipped behavior -- gated off by default; remove the ini key for play.
void MaybeSyntheticWipe() {
    static const bool s_on = ::coop::ini_config::IsIniKeyTrue("window_synth");
    if (!s_on) return;
    static bool s_done = false;
    static int  s_ticks = 0;
    if (s_done) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (++s_ticks < 300) return;  // ~5s settle after connect (60 Hz tick)
    void* actor = nullptr;
    std::wstring key;
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        if (g_byKey.empty()) return;
        auto& kv = *g_byKey.begin();
        key = kv.first;
        actor = kv.second.actor;
    }
    if (!actor || !R::IsLive(actor)) return;
    float cur = 0.f;
    if (!BW::ReadClean(actor, cur)) return;
    float target = cur - 0.4f;
    if (target < 0.f) target = 0.f;
    BW::WriteCleanAndApply(actor, target);  // field write + setClean (the real wipe path)
    s_done = true;
    UE_LOGW("window[SYNTH]: dev synthetic wipe -- key='%ls' clean %.3f -> %.3f "
            "(PollAndBroadcast should now send + the client should apply)",
            key.c_str(), cur, target);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (BW::EnsureResolved()) {
        static bool s_indexed = false;
        if (!s_indexed) { UE_LOGI("window: indexed %zu window(s)", RebuildIndex()); s_indexed = true; }
    }
}

void OnReliable(const coop::net::KeyedScalarPayload& payload, uint8_t senderPeerSlot) {
    std::wstring key = StringFromWireKey(payload.key);
    if (key.empty()) { UE_LOGW("window: OnReliable empty key -- dropping"); return; }
    if (!BW::EnsureResolved()) {
        UE_LOGW("window: apply -- class not resolved, dropping key='%ls'", key.c_str());
        return;
    }
    // Trust boundary: a VERBATIM adopt (which bypasses min-wins and so CAN make a window
    // DIRTIER) is legitimate ONLY for the host's connect-snapshot. The host is slot 0; a
    // client-originated edge is relayed carrying its true origin slot (>= 1). So honor adopt
    // only from slot 0 -- a buggy/relayed client packet stamped adopt=1 is downgraded to a
    // live wipe (min-wins) and can never force-dirty another peer's window. (Matches the
    // codebase's host-only trust gates on senderPeerSlot == 0.)
    const bool adopt = (payload.adopt != 0) && (senderPeerSlot == 0);
    if (void* actor = ResolveFast(key)) { ApplyResolved(actor, key, payload.value, adopt, senderPeerSlot); return; }
    // Not streamed in yet -- defer + retry on the throttled tick. Merge with any existing
    // pending entry: an adopt (host baseline) takes the value verbatim; a live wipe keeps the
    // LOWEST seen so a lower value from one sender is not lost behind a higher one from another.
    const auto deadline = std::chrono::steady_clock::now() + kPendingTTL;
    auto it = g_pending.find(key);
    if (it == g_pending.end()) {
        g_pending[key] = Pending{ payload.value, adopt, senderPeerSlot, deadline };
    } else {
        if (adopt) { it->second.clean = payload.value; it->second.adopt = true; }
        else       { it->second.clean = std::min(it->second.clean, payload.value); }
        it->second.fromSlot = senderPeerSlot;
        it->second.deadline = deadline;
    }
    if (ProbeLog())
        UE_LOGI("window: '%ls' not present yet -- deferring clean=%.3f adopt=%d (slot %u)",
                key.c_str(), payload.value, adopt ? 1 : 0, senderPeerSlot);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (s->role() != coop::net::Role::Host) return;  // host-only snapshot
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    RebuildIndex();
    std::vector<std::pair<std::wstring, Ref>> items;
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        items.reserve(g_byKey.size());
        for (auto& kv : g_byKey) items.emplace_back(kv.first, kv.second);
    }
    int sent = 0;
    for (auto& d : items) {
        if (!R::IsLiveByIndex(d.second.actor, d.second.idx)) continue;
        float clean = 0.f;
        if (!BW::ReadClean(d.second.actor, clean)) continue;
        coop::net::KeyedScalarPayload p{};
        WireKeyFromString(d.first, p.key);
        p.value = clean;
        p.adopt = 1;  // connect-snapshot -> the joiner adopts the host's world VERBATIM
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::WindowCleanState, &p, sizeof(p));
        { std::lock_guard<std::mutex> lk(g_stateMutex); g_lastKnown[d.first] = clean; }
        ++sent;
    }
    UE_LOGI("window: connect-snapshot -- sent %d window clean(s) to slot %d (of %zu indexed)",
            sent, peerSlot, items.size());
}

void Tick() {
    if (!BW::EnsureResolved()) return;
    MaybeSyntheticWipe();  // dev-only, ini-gated one-shot (no-op unless window_synth=1)
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastRetry >= kRetryRebuildThrottle) {
        g_lastRetry = now;
        ue_wrap::ScopedWalkTimer _wt("window:RebuildIndex");  // logs only the rare ~5min full safety
        RebuildIndex();   // L5: INCREMENTAL -- tail-scan only the new objects + a rare full safety (no 237k walk)
        // RECEIVER: retry deferred applies for windows that have now streamed in.
        if (!g_pending.empty()) {
            int applied = 0, expired = 0, still = 0;
            for (auto it = g_pending.begin(); it != g_pending.end();) {
                if (void* actor = ResolveFast(it->first)) {
                    ApplyResolved(actor, it->first, it->second.clean, it->second.adopt, it->second.fromSlot);
                    it = g_pending.erase(it);
                    ++applied;
                } else if (now >= it->second.deadline) {
                    if (ProbeLog())
                        UE_LOGI("window: deferred '%ls' expired (not present on this peer)", it->first.c_str());
                    it = g_pending.erase(it);
                    ++expired;
                } else { ++it; ++still; }
            }
            if (applied || expired)
                UE_LOGI("window: retry tick -- applied %d deferred, dropped %d expired, %d still pending",
                        applied, expired, still);
        }
    }
    PollAndBroadcast();
}

void OnDisconnect() {
    g_pending.clear();
    std::lock_guard<std::mutex> lk(g_stateMutex);
    const size_t n = g_lastKnown.size();
    g_lastKnown.clear();
    if (n > 0) UE_LOGI("window: OnDisconnect cleared %zu last-known", n);
}

}  // namespace coop::window_sync
