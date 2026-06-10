// coop/trash_pile_sync.cpp -- see header. The grime_sync shape with an int pair +
// depletion-destroy semantics (the third keyed channel, deliberately NOT generalized
// yet: float-scalar x2 vs int-pair-with-destroy differ enough that a forced common
// engine would be speculative -- revisit if a FOURTH appears).

#include "coop/trash_pile_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/players_registry.h"
#include "coop/prop_element_tracker.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::trash_pile_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace UP = ue_wrap::prop;

using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

// GT-only state (poll/apply/rebuild all run from the net-pump tick + event_feed
// drain on the game thread -- the grime concurrency model).
struct Entry {
    void*   actor = nullptr;
    int32_t idx   = -1;       // InternalIndex for IsLiveByIndex (no deref of a freed ptr)
    int32_t lastA = -1;       // poll baseline (-1 = not yet primed)
    int32_t lastB = -1;
    float   x = 0, y = 0, z = 0;  // last live position (the death-watch proximity test)
};
std::unordered_map<std::wstring, Entry> g_index;
struct Pending { int16_t a; int16_t b; bool adopt; Clock::time_point deadline; };
std::unordered_map<std::wstring, Pending> g_pending;   // deferred applies (instance not streamed in)
std::unordered_set<std::wstring> g_depletedKeys;       // depleted THIS session (connect replay)
bool g_installed = false;
Clock::time_point g_nextPoll{};
Clock::time_point g_nextRebuild{};

constexpr auto kPollEvery    = std::chrono::milliseconds(50);
constexpr auto kRebuildEvery = std::chrono::seconds(2);
constexpr auto kPendingTtl   = std::chrono::seconds(25);
// Death-watch proximity gate: every writer (E-grab / vacuum / broom) acts at the
// player, so a genuine depletion dies NEAR the local camera. Far deaths are
// sublevel stream-outs. 8 m -- the grime super-sponge + v52 pile precedent.
constexpr float kDeathNearCm = 800.f;

uint64_t Fnv1a(const std::wstring& s, uint64_t h) {
    for (wchar_t c : s) { h ^= static_cast<uint64_t>(c); h *= 1099511628211ull; }
    return h;
}

// One GUObjectArray walk: (re)index every live trashBitsPile by key. Existing
// entries keep their poll baselines (re-primed only for NEW keys). Logs
// (count, keysHash) on population change -- host==client hash is the smoke
// signal for cross-peer key identity (~392 placed piles expected).
void RebuildIndex() {
    size_t before = g_index.size();
    std::unordered_set<std::wstring> seen;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (!UP::IsTrashBitsPile(obj)) continue;
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        const std::wstring key = UP::GetInteractableKeyString(obj);
        if (key.empty() || key == L"None") continue;
        seen.insert(key);
        auto& e = g_index[key];
        if (e.actor != obj) { e.actor = obj; e.idx = R::InternalIndexOf(obj); }
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(obj);
        e.x = loc.X; e.y = loc.Y; e.z = loc.Z;
    }
    // Drop index entries whose key vanished from the walk WITHOUT the death-watch
    // having adjudicated them (e.g. pre-install stale state). The death-watch path
    // below handles live-session deaths; this is rebuild hygiene only.
    for (auto it = g_index.begin(); it != g_index.end();) {
        if (!seen.count(it->first) && !R::IsLiveByIndex(it->second.actor, it->second.idx)) {
            it = g_index.erase(it);
        } else {
            ++it;
        }
    }
    if (g_index.size() != before) {
        // XOR-fold of per-key FNV hashes: ORDER-INDEPENDENT, so identical key sets
        // hash identically across peers regardless of map iteration order.
        uint64_t h = 0;
        for (const auto& [k, e] : g_index) h ^= Fnv1a(k, 1469598103934665603ull);
        UE_LOGI("trash_pile: indexed %zu pile(s), keysHash=0x%llX", g_index.size(),
                static_cast<unsigned long long>(h));
    }
}

void SendState(coop::net::Session* s, const std::wstring& key, int32_t a, int32_t b, bool adopt,
               int toSlot /* <0 = broadcast */) {
    coop::net::TrashPileStatePayload p{};
    p.key.len = 0;
    for (size_t i = 0; i < key.size() && i < 31; ++i)
        p.key.data[p.key.len++] = static_cast<char>(key[i]);
    p.amountA = static_cast<int16_t>(a < 0 ? 0 : (a > 10000 ? 10000 : a));
    p.amountB = static_cast<int16_t>(b < 0 ? 0 : (b > 10000 ? 10000 : b));
    p.adopt = adopt ? 1 : 0;
    if (toSlot >= 0) {
        s->SendReliableToSlot(toSlot, coop::net::ReliableKind::TrashPileState, &p, sizeof(p));
    } else {
        s->SendReliable(coop::net::ReliableKind::TrashPileState, &p, sizeof(p));
    }
}

// Apply wire counters to a live local pile + advance the poll baseline so the
// write is not re-broadcast as a local decrease (the echo guard -- poll and
// apply are GT-serial).
void ApplyToLive(Entry& e, const std::wstring& key, int16_t wa, int16_t wb, bool adopt,
                 uint8_t fromSlot) {
    int32_t la = 0, lb = 0;
    if (!UP::ReadTrashPileAmounts(e.actor, la, lb)) return;
    int32_t na, nb;
    if (adopt) {
        na = wa; nb = wb;  // host snapshot: adopt verbatim
    } else {
        na = (wa < la) ? wa : la;  // live edge: per-component MIN (monotone-down)
        nb = (wb < lb) ? wb : lb;
    }
    if (na == la && nb == lb) {
        e.lastA = na; e.lastB = nb;  // idempotent -- still prime the baseline
        return;
    }
    UP::WriteTrashPileAmounts(e.actor, na, nb);
    e.lastA = na; e.lastB = nb;
    UE_LOGI("trash_pile: applied A=%d B=%d key='%ls' (from slot %u%s)",
            na, nb, key.c_str(), fromSlot, adopt ? ", adopt" : "");
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_installed) return;
    // Readiness == the class cache resolving inside the first successful walk
    // (RebuildIndex finds instances only once trashBitsPile_C is loaded).
    RebuildIndex();
    if (!g_index.empty()) {
        g_installed = true;
        UE_LOGI("trash_pile: installed (%zu pile(s) indexed)", g_index.size());
    }
    // No piles yet (class not loaded / menu): retried by the next tick's Install call.
}

void OnReliable(const coop::net::TrashPileStatePayload& payload, uint8_t senderPeerSlot) {
    // adopt only from the HOST (slot 0) -- the grime trust boundary.
    const bool adopt = (payload.adopt != 0) && (senderPeerSlot == 0);
    std::wstring key;
    key.reserve(payload.key.len);
    for (uint8_t i = 0; i < payload.key.len && i < 31; ++i)
        key.push_back(static_cast<wchar_t>(static_cast<unsigned char>(payload.key.data[i])));
    if (key.empty()) return;
    auto it = g_index.find(key);
    if (it != g_index.end() && R::IsLiveByIndex(it->second.actor, it->second.idx)) {
        ApplyToLive(it->second, key, payload.amountA, payload.amountB, adopt, senderPeerSlot);
        return;
    }
    // Not streamed in / not yet indexed: defer (merge: adopt overrides, live keeps MIN).
    auto pit = g_pending.find(key);
    if (pit == g_pending.end()) {
        g_pending[key] = Pending{payload.amountA, payload.amountB, adopt,
                                 Clock::now() + kPendingTtl};
    } else {
        Pending& p = pit->second;
        if (adopt) {
            p = Pending{payload.amountA, payload.amountB, true, Clock::now() + kPendingTtl};
        } else if (!p.adopt) {
            if (payload.amountA < p.a) p.a = payload.amountA;
            if (payload.amountB < p.b) p.b = payload.amountB;
            p.deadline = Clock::now() + kPendingTtl;
        }
    }
}

void NotifyWireDestroy(const std::wstring& key) {
    if (key.empty()) return;
    if (g_index.erase(key) > 0) {
        UE_LOGI("trash_pile: wire destroy for key='%ls' -- unwatched (no re-broadcast)", key.c_str());
    }
    g_pending.erase(key);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    RebuildIndex();
    size_t sent = 0;
    for (auto& [key, e] : g_index) {
        if (!R::IsLiveByIndex(e.actor, e.idx)) continue;
        int32_t a = 0, b = 0;
        if (!UP::ReadTrashPileAmounts(e.actor, a, b)) continue;
        SendState(s, key, a, b, /*adopt=*/true, peerSlot);
        ++sent;
    }
    // Depleted-this-session piles: replay the keyed destroy so a joiner whose
    // transferred save predates the depletion drops its copy. (Pre-session
    // depletions are already absent from the host save; mid-session ones may
    // race the joiner's load window -- the claim sweep is the backstop, this
    // is the precise signal.)
    size_t replayed = 0;
    for (const auto& key : g_depletedKeys) {
        coop::net::PropDestroyPayload dp{};
        dp.key.len = 0;
        for (size_t i = 0; i < key.size() && i < 31; ++i)
            dp.key.data[dp.key.len++] = static_cast<char>(key[i]);
        dp.elementId = 0;
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::PropDestroy, &dp, sizeof(dp));
        ++replayed;
    }
    UE_LOGI("trash_pile: connect-snapshot -> slot %d (%zu pile(s), %zu depleted-key replay(s))",
            peerSlot, sent, replayed);
}

void Tick(bool inTransition) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (!g_installed) { Install(s); if (!g_installed) return; }
    const auto now = Clock::now();
    if (now >= g_nextRebuild) {
        g_nextRebuild = now + kRebuildEvery;
        RebuildIndex();
        // Deferred-apply retry on the same throttle.
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            auto idx = g_index.find(it->first);
            if (idx != g_index.end() && R::IsLiveByIndex(idx->second.actor, idx->second.idx)) {
                ApplyToLive(idx->second, it->first, it->second.a, it->second.b,
                            it->second.adopt, 0);
                it = g_pending.erase(it);
            } else if (now >= it->second.deadline) {
                it = g_pending.erase(it);  // event-cluster pile with a peer-local key: expire silently
            } else {
                ++it;
            }
        }
    }
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPollEvery;

    void* local = coop::players::Registry::Get().Local();
    ue_wrap::FVector cam{};
    if (local) cam = ue_wrap::engine::GetActorLocation(local);

    for (auto it = g_index.begin(); it != g_index.end();) {
        Entry& e = it->second;
        if (!R::IsLiveByIndex(e.actor, e.idx)) {
            // DEATH-WATCH: depleted (BP-internal K2_DestroyActor, invisible to the
            // observer + the poll) vs streamed out -- discriminate by proximity.
            const float dx = e.x - cam.X, dy = e.y - cam.Y, dz = e.z - cam.Z;
            const bool nearCam = local &&
                (dx * dx + dy * dy + dz * dz) <= (kDeathNearCm * kDeathNearCm);
            if (nearCam && !inTransition) {
                coop::net::PropDestroyPayload dp{};
                dp.key.len = 0;
                for (size_t i = 0; i < it->first.size() && i < 31; ++i)
                    dp.key.data[dp.key.len++] = static_cast<char>(it->first[i]);
                dp.elementId = 0;
                s->SendPropDestroy(dp);
                g_depletedKeys.insert(it->first);
                UE_LOGI("trash_pile: pile key='%ls' depleted near camera -- broadcast keyed destroy",
                        it->first.c_str());
            }
            g_pending.erase(it->first);
            it = g_index.erase(it);
            continue;
        }
        int32_t a = 0, b = 0;
        if (UP::ReadTrashPileAmounts(e.actor, a, b)) {
            if (e.lastA < 0) {
                e.lastA = a; e.lastB = b;  // first sighting: prime silently
            } else if (a < e.lastA || b < e.lastB) {
                // Local collect (any of the three writers). Broadcast absolute values.
                e.lastA = a; e.lastB = b;
                SendState(s, it->first, a, b, /*adopt=*/false, /*toSlot=*/-1);
                UE_LOGI("trash_pile: local collect key='%ls' -> A=%d B=%d (broadcast)",
                        it->first.c_str(), a, b);
            } else if (a > e.lastA || b > e.lastB) {
                e.lastA = a; e.lastB = b;  // BeginPlay re-roll / save reload: re-prime, never propagate
            }
            // NO per-poll GetActorLocation here (audit CRIT-1): it is a ProcessEvent
            // dispatch -- ~400 piles x 20 Hz would add ~8000 PE/s through our own
            // detour. Piles are save-placed and never move; the position captured at
            // RebuildIndex time (2 s throttle) is the death-watch's proximity input.
        }
        ++it;
    }
}

void OnDisconnect() {
    g_index.clear();
    g_pending.clear();
    g_depletedKeys.clear();
    g_installed = false;
    g_nextPoll = {};
    g_nextRebuild = {};
    UE_LOGI("trash_pile: OnDisconnect cleared index + baselines + depleted keys");
}

}  // namespace coop::trash_pile_sync
