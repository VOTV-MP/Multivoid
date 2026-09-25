// coop/interactables/window_sync.cpp -- see coop/interactables/window_sync.h. Base-window dirt
// scalar sync.
//
// A window's dirt is one scalar, `clean`, lowered only by its cleanSponge (clean -= strength*0.01,
// floored at 0, then setClean repaints) and set wholesale by loadData from the save. Each peer sends
// a fall at the verb that made it: a PRE/POST watch on baseWindow_C's cleanSponge reads the value on
// both sides of the body. A receiver applies MIN-WINS (monotone cooperative clean), so a live wipe
// never raises a window another peer cleaned further; the apply writes the field and repaints through
// setClean, never the verb, so it is never heard back. The host's connect snapshot is an adopt: the
// joiner takes the host's world unchanged. The Key->actor index rides the shared scan hub, and a
// value for a window this peer has not indexed yet waits in a pending entry until a pass finds it.

#include "coop/interactables/window_sync.h"

#include "coop/config/config.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"  // WireKeyFromString / StringFromWireKey / FnvKey (shared)
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers

#include "ue_wrap/devices/base_window.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/world_identity.h"     // gen-stamped index (dead-world guard)
#include "coop/element/object_scan_hub.h"      // the shared sliced scan pass

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
namespace sg = ue_wrap::script_gate;

using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;
using coop::net::FnvKey;

constexpr auto kRetryRebuildThrottle = std::chrono::seconds(2);
constexpr auto kPendingTTL = std::chrono::seconds(25);
// `clean` changes in wipe steps (cleanSponge: clean -= strength*0.01), and a step can be smaller
// than this epsilon (a water-shooter droplet, a slow bucket pour), so a fall is measured against the
// value last sent and small ones add up (see Sent). It also skips an idempotent apply.
constexpr float kCleanEps = 0.0005f;

bool ProbeLog() {
    static const bool s_enabled = ::coop::config::ResolveFlag(::coop::config_registry::rows::window_log);
    return s_enabled;
}

std::atomic<coop::net::Session*> g_session{nullptr};

struct Ref { void* actor; int32_t idx; };
std::mutex g_indexMutex;
std::unordered_map<std::wstring, Ref> g_byKey;

struct Pending { float clean; bool adopt; uint8_t fromSlot; std::chrono::steady_clock::time_point deadline; };
std::unordered_map<std::wstring, Pending> g_pending;  // GT-only: deferred applies (window not streamed in yet)

std::chrono::steady_clock::time_point g_lastRetry{};
size_t g_lastLogCount = SIZE_MAX;  // GT-only: dedup the rebuilt log
uint64_t g_lastLogHash = 0;        // GT-only

// World generation of the last completed hub pass; a stale-gen index is treated as EMPTY on every
// read path (dead-world guard).
uint32_t g_indexGen = 0;
bool IndexCurrent() { return g_indexGen == ue_wrap::world_identity::Generation(); }

void* ResolveFast(const std::wstring& key) {
    if (!IndexCurrent()) return nullptr;  // stale-gen index = another world's actors (R-1 class)
    std::lock_guard<std::mutex> lk(g_indexMutex);
    auto it = g_byKey.find(key);
    if (it != g_byKey.end() && R::IsLiveByIndex(it->second.actor, it->second.idx))
        return it->second.actor;
    return nullptr;
}

// ---- shared-scan hub consumer -------------------------------------------------------------
// There is no per-module walk; the hub's shared sliced pass drives these callbacks. settleScans=2:
// a window break/repair changes the count and would otherwise re-arm 15 passes of full demand for
// a routine event. Only 4 windows exist, so 2 stable passes suffice and the hub's 60 s backstop
// covers recycled-slot stragglers.
std::vector<std::pair<std::wstring, Ref>> g_scanFound;  // pass scratch (GT-only)

void HubPassBegin(void*, bool) { g_scanFound.clear(); }

void HubMatch(void*, void* obj) {
    const std::wstring nm = R::ToString(R::NameOf(obj));
    if (nm.rfind(L"Default__", 0) == 0) return;  // skip CDO
    if (!R::IsLive(obj)) return;
    std::wstring key = BW::GetKeyString(obj);
    if (key.empty() || key == L"None") return;
    g_scanFound.emplace_back(std::move(key), Ref{ obj, R::InternalIndexOf(obj) });
}

size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    const size_t added = g_scanFound.size();
    uint64_t keysHash = 0;
    size_t   total;
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        if (isFull) g_byKey.clear();                           // full pass: rebuild from scratch
        for (auto& f : g_scanFound) g_byKey[f.first] = f.second;
        if (!isFull) {                                         // tail pass: prune dead entries (cheap, O(index))
            for (auto it = g_byKey.begin(); it != g_byKey.end(); ) {
                if (R::IsLiveByIndex(it->second.actor, it->second.idx)) ++it;
                else it = g_byKey.erase(it);
            }
        }
        for (auto& kv : g_byKey) keysHash ^= FnvKey(kv.first);  // recompute over the index (cheap, O(index))
        total = g_byKey.size();
        g_indexGen = worldGen;
    }
    if (total != g_lastLogCount || keysHash != g_lastLogHash) {
        g_lastLogCount = total;
        g_lastLogHash = keysHash;
        UE_LOGI("window: index now %zu live keyed window(s), keysHash=0x%016llX (%s pass, +%zu new) "
                "(compare host vs client for cross-peer Key stability)",
                total, static_cast<unsigned long long>(keysHash), isFull ? "full" : "tail", added);
    }
    if (ProbeLog())
        for (auto& f : g_scanFound)
            UE_LOGI("window[probe]: key='%ls' idx=%d actor=%p", f.first.c_str(), f.second.idx, f.second.actor);
    g_scanFound.clear();
    return total;
}

void RegisterWithScanHub() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        "window", nullptr, &BW::EnsureResolved, &BW::IsBaseWindow,
        &HubPassBegin, &HubMatch, &HubPassComplete, /*settleScans*/ 2});
}

// Per window, the value this peer last sent, seeded at the entry of its first wipe and lowered by what
// it applies: a fall is measured against it, so falls smaller than the epsilon -- a water-shooter
// droplet, a slow bucket pour -- add up and go out once they pass it. Per world, since the save's
// load sets every window anew.
std::unordered_map<std::wstring, float> g_sent;  // GT-only
uint32_t g_sentGen = 0;
std::unordered_map<std::wstring, float>& Sent() {
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen != g_sentGen) {
        g_sent.clear();
        g_sentGen = gen;
    }
    return g_sent;
}

// Apply a remote clean value. An adopt takes it unchanged (connect-snapshot: the joiner adopts the
// host's world); a live wipe is MIN-WINS, so a value above ours is ignored.
void ApplyResolved(void* actor, const std::wstring& key, float wireClean, bool adopt, unsigned fromSlot) {
    float cur = 0.f;
    if (!BW::ReadClean(actor, cur)) return;
    const float target = adopt ? wireClean : std::min(cur, wireClean);
    if (std::fabs(target - cur) < kCleanEps) {
        if (ProbeLog())
            UE_LOGI("window: apply key='%ls' already %.3f -- idempotent skip", key.c_str(), target);
        return;
    }
    const bool ok = BW::WriteCleanAndApply(actor, target);
    if (ok) Sent()[key] = target;  // a local wipe after it is measured from what this peer now shows
    if (ProbeLog() || !ok)
        UE_LOGI("window: applied %s clean=%.3f (wire=%.3f) ok=%d key='%ls' (from slot %u)",
                adopt ? "an adopt" : "a live wipe", target, wireClean, ok ? 1 : 0, key.c_str(), fromSlot);
}

// ---- the sender: the wipe verb ---------------------------------------------------------------
constexpr int kTagWindowWipe = 0x57495045;  // 'WIPE'
constexpr const wchar_t* kWindowClass = L"baseWindow_C";  // one literal each: the gate keys watches on the pointer
constexpr const wchar_t* kWipeVerb = L"cleanSponge";
bool g_wipeWatched = false;

// The window a cleanSponge body runs on and its clean before the body. The body (ubergraph entry 29)
// calls only native helpers and setClean, so no watched body runs inside it and one slot holds it.
void* g_wiping = nullptr;
float g_before = 0.f;

sg::Verdict OnWipePre(const sg::Call& call) {
    g_wiping = nullptr;
    if (call.object && BW::ReadClean(call.object, g_before)) g_wiping = call.object;
    return sg::Verdict::Run;
}

// After the body: the clean it left, sent when it fell. A session with nobody connected has no one
// to send to; the joiner's snapshot is the connect half.
void OnWipePost(const sg::Call& call) {
    if (!g_wiping || call.object != g_wiping) return;
    g_wiping = nullptr;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    float cur = 0.f;
    if (!BW::ReadClean(call.object, cur) || cur >= g_before) return;  // at 0 the body lowers nothing
    const std::wstring key = BW::GetKeyString(call.object);
    if (key.empty() || key == L"None") return;
    auto& sent = Sent();
    const auto it = sent.find(key);
    if (cur >= (it != sent.end() ? it->second : g_before) - kCleanEps) {
        if (it == sent.end()) sent[key] = g_before;
        return;
    }
    sent[key] = cur;
    coop::net::KeyedScalarPayload p{};
    WireKeyFromString(key, p.key);
    p.value = cur;
    p.adopt = 0;  // a live wipe: receivers apply the minimum
    if (!s->SendReliable(coop::net::ReliableKind::WindowCleanState, &p, sizeof(p)))
        UE_LOGW("window: SendReliable failed key='%ls'", key.c_str());
    else if (ProbeLog())
        UE_LOGI("window: sent clean=%.3f key='%ls'", cur, key.c_str());
}

// DEV-ONLY synthetic wipe (`window_synth=1`). One-shot, host-only, armed when the joiner's world is
// ready: it runs cleanSponge through the verb itself, as a sponge would, on the first indexed window
// with dirt left (the body returns at 0), in falls too small to send alone, so the watch above sends
// their sum and the client applies it -- the live-wipe chain end to end without a hand at the
// keyboard. Run it with window_log=1, which logs the send and the apply. NOT shipped behavior.
void MaybeSyntheticWipe() {
    static const bool s_on = ::coop::config::ResolveFlag(::coop::config_registry::rows::window_synth);
    if (!s_on) return;
    static bool s_done = false;
    if (s_done) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host || !s->IsSlotWorldReady(1)) return;
    if (!sg::ClassNameWatchLive(kWindowClass, kWipeVerb, kTagWindowWipe)) return;  // the send rides it
    void* actor = nullptr;
    std::wstring key;
    float before = 0.f;
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        for (const auto& kv : g_byKey) {
            float clean = 0.f;
            if (!R::IsLiveByIndex(kv.second.actor, kv.second.idx) || !BW::ReadClean(kv.second.actor, clean) ||
                clean <= 0.f)
                continue;
            key = kv.first;
            actor = kv.second.actor;
            before = clean;
            break;
        }
    }
    if (!actor) return;
    s_done = true;
    // Ten dabs of 0.0001, each under the epsilon and twice it together: the watch must send once they
    // have added up past it, and the client apply that value.
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(actor), kWipeVerb);
    int ran = 0;
    for (int i = 0; fn && i < 10; ++i) {
        ue_wrap::ParamFrame f(fn);
        if (f.valid() && f.Set<float>(L"clean", 0.01f) && ue_wrap::Call(actor, f)) ++ran;
    }
    float after = before;
    BW::ReadClean(actor, after);
    UE_LOGW("window[SYNTH]: %d of 10 dabs of 0.0001 through cleanSponge -- key='%ls' clean %.4f -> %.4f",
            ran, key.c_str(), before, after);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWithScanHub();  // the hub builds the index on its own cadence
    if (!g_wipeWatched)
        g_wipeWatched = sg::WatchClassName(kWindowClass, kWipeVerb, kTagWindowWipe, &OnWipePre, &OnWipePost);
}

void OnReliable(const coop::net::KeyedScalarPayload& payload, uint8_t senderPeerSlot) {
    std::wstring key = StringFromWireKey(payload.key);
    if (key.empty()) { UE_LOGW("window: OnReliable empty key -- dropping"); return; }
    if (!BW::EnsureResolved()) {
        UE_LOGW("window: apply -- class not resolved, dropping key='%ls'", key.c_str());
        return;
    }
    // Trust boundary: an unchanged adopt (which bypasses min-wins and so CAN raise a window's
    // clean) is host-only. A client claiming one would otherwise be able to scrub the world clean.
    const bool adopt = (payload.adopt != 0) && (senderPeerSlot == 0);
    if (void* actor = ResolveFast(key)) { ApplyResolved(actor, key, payload.value, adopt, senderPeerSlot); return; }
    // Not streamed in yet -- defer + retry on the throttled tick. Merge with any existing pending
    // entry: an adopt (host baseline) takes the value unchanged; a live wipe keeps the lower of the
    // two, which is the same min-wins rule the direct apply uses.
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
    // No forced rebuild here: the hub keeps the index within one pass (~2 s) of fresh, and windows
    // are static level actors, so the staleness window is empty in practice.
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
        p.adopt = 1;  // connect-snapshot -> the joiner adopts the host's world unchanged
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::WindowCleanState, &p, sizeof(p));
        ++sent;
    }
    UE_LOGI("window: connect-snapshot -- sent %d window clean(s) to slot %d (of %zu indexed)",
            sent, peerSlot, items.size());
}

void Tick() {
    if (!BW::EnsureResolved()) return;
    RegisterWithScanHub();  // safety net for any order where Tick precedes Install
    if (!IndexCurrent()) return;  // index belongs to a dead world -- wait for the hub's next pass
    MaybeSyntheticWipe();  // dev-only, flag-gated one-shot (no-op unless window_synth=1)
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastRetry < kRetryRebuildThrottle) return;
    g_lastRetry = now;
    // RECEIVER: retry deferred applies for windows that have now streamed in (the hub refreshed the
    // index on its own cadence; this throttle paces only the retries).
    if (g_pending.empty()) return;
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

void OnDisconnect() {
    g_pending.clear();
    g_wiping = nullptr;
    g_sent.clear();
}

}  // namespace coop::window_sync
