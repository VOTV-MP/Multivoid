// coop/turbine_sync.cpp -- see coop/turbine_sync.h.

#include "coop/interactables/turbine_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"

#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/settled_object_scan.h"  // stream-settle scan (L5 + the 18:41 world-reload cure)
#include "ue_wrap/core/walk_timer.h"           // L5: [WALK-TIME] profiling
#include "ue_wrap/devices/windturbine.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::turbine_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace WT = ue_wrap::windturbine;
namespace E  = ue_wrap::engine;

using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;

constexpr auto kRebuildThrottle = std::chrono::seconds(2);
constexpr auto kPollInterval    = std::chrono::milliseconds(1000);  // ~1 Hz host poll
constexpr auto kPendingTTL      = std::chrono::seconds(25);
// Re-send gate: any driver float moved by more than this since the last send.
// alpha_blades accumulates whenever the wind blows, so a spinning world resends
// each poll (by design, ~13 x 56 B/s); a CALM world goes silent.
constexpr float kSendEpsilon = 0.01f;
// Quantization grid (cm) for the position identity. Turbines are static
// (SetMobility(Static), loadTransform=false) and hundreds of meters apart;
// the same save yields bit-identical transforms on both peers.
constexpr double kPosGrid = 10.0;

std::atomic<coop::net::Session*> g_session{nullptr};

struct Ref { void* actor; int32_t idx; };
std::mutex g_mutex;  // guards the maps (game-thread-serial; defensive)
std::unordered_map<std::wstring, Ref>        g_index;      // posKey -> live turbine
std::unordered_map<std::wstring, WT::State>  g_lastKnown;  // posKey -> last sent/applied
struct Pending { WT::State want; std::chrono::steady_clock::time_point deadline; };
std::unordered_map<std::wstring, Pending>    g_pending;    // posKey -> deferred apply

std::chrono::steady_clock::time_point g_lastRebuild{};
std::chrono::steady_clock::time_point g_lastPoll{};
size_t g_lastLogCount = SIZE_MAX;
std::vector<std::pair<std::wstring, Ref>> g_scratch;  // GT-only reused snapshot

// The cross-peer identity of a STATIC turbine: its quantized world position
// (the grime PosKey shape). Fits the 31-char WireKey for any map coordinate.
std::wstring PosKey(void* turbine) {
    const ue_wrap::FVector loc = E::GetActorLocation(turbine);
    auto q = [](float v) -> long { return std::lround(static_cast<double>(v) / kPosGrid); };
    std::wstring k = L"t_";
    k += std::to_wstring(q(loc.X)); k += L'_';
    k += std::to_wstring(q(loc.Y)); k += L'_';
    k += std::to_wstring(q(loc.Z));
    return k;
}

bool StateDiffers(const WT::State& a, const WT::State& b) {
    return std::fabs(a.headRotation   - b.headRotation)   > kSendEpsilon ||
           std::fabs(a.targetRot      - b.targetRot)      > kSendEpsilon ||
           std::fabs(a.rot            - b.rot)            > kSendEpsilon ||
           std::fabs(a.alphaBlades    - b.alphaBlades)    > kSendEpsilon ||
           std::fabs(a.bladesMomentum - b.bladesMomentum) > kSendEpsilon ||
           std::fabs(a.mult           - b.mult)           > kSendEpsilon;
}

void StateToPayload(const std::wstring& key, const WT::State& st,
                    coop::net::TurbineStatePayload& p) {
    std::memset(&p, 0, sizeof(p));
    WireKeyFromString(key, p.key);
    p.headRotation   = st.headRotation;
    p.targetRot      = st.targetRot;
    p.rot            = st.rot;
    p.alphaBlades    = st.alphaBlades;
    p.bladesMomentum = st.bladesMomentum;
    p.mult           = st.mult;
}

WT::State PayloadToState(const coop::net::TurbineStatePayload& p) {
    WT::State st;
    st.headRotation   = p.headRotation;
    st.targetRot      = p.targetRot;
    st.rot            = p.rot;
    st.alphaBlades    = p.alphaBlades;
    st.bladesMomentum = p.bladesMomentum;
    st.mult           = p.mult;
    return st;
}

bool PayloadFinite(const coop::net::TurbineStatePayload& p) {
    const float vals[6] = { p.headRotation, p.targetRot, p.rot,
                            p.alphaBlades, p.bladesMomentum, p.mult };
    for (float v : vals)
        if (!std::isfinite(v)) return false;
    return true;
}

// Rebuild the posKey->actor index (~13 static actors; the set virtually never changes).
// Stream-settle scan (ue_wrap/settled_object_scan.h) -- the raw tail-scan died at the 18:41
// host world reload (prune-to-0, recycled slots below the cursor). WT::IsTurbine is the same
// cheap class-descendant filter the old FindObjectsByClass applied, per-object on the range.
void RebuildIndex() {
    static ue_wrap::scan::SettledObjectScan sScan;
    const auto r = sScan.Begin();
    std::vector<std::pair<std::wstring, Ref>> found;
    for (int32_t i = r.begin; i < r.end; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || !WT::IsTurbine(obj) || !R::IsLive(obj)) continue;
        found.emplace_back(PosKey(obj), Ref{ obj, R::InternalIndexOf(obj) });
    }
    size_t total;
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
        total = g_index.size();
    }
    sScan.End(total);  // feed the settle gate (any count change re-arms full walks)
    if (total != g_lastLogCount) {
        g_lastLogCount = total;
        UE_LOGI("turbine: index rebuilt -- %zu turbine(s)", total);
    }
}

void* ResolveFast(const std::wstring& key) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_index.find(key);
    if (it != g_index.end() && R::IsLiveByIndex(it->second.actor, it->second.idx))
        return it->second.actor;
    return nullptr;
}

void ApplyState(void* turbine, const std::wstring& key, const WT::State& want) {
    if (!WT::WriteState(turbine, want)) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    g_lastKnown[key] = want;  // echo-irrelevant (clients never send) but keeps the map warm
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!WT::EnsureResolved()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastRebuild >= kRebuildThrottle) {
        g_lastRebuild = now;
        ue_wrap::ScopedWalkTimer _wt("turbine:RebuildIndex");  // logs only the rare ~5min full safety
        RebuildIndex();   // L5: INCREMENTAL -- tail-scan + a rare full safety (no 237k walk)
        // Retry deferred applies for turbines that streamed in.
        std::vector<std::pair<std::wstring, WT::State>> ready;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            for (auto it = g_pending.begin(); it != g_pending.end();) {
                auto idxIt = g_index.find(it->first);
                if (idxIt != g_index.end() &&
                    R::IsLiveByIndex(idxIt->second.actor, idxIt->second.idx)) {
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
            if (void* t = ResolveFast(rdy.first)) ApplyState(t, rdy.first, rdy.second);
    }

    // HOST: the ~1 Hz authoritative poll/broadcast.
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (now - g_lastPoll < kPollInterval) return;
    g_lastPoll = now;

    auto& refs = g_scratch;
    refs.clear();
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_index.empty()) return;
        refs.reserve(g_index.size());
        for (auto& kv : g_index) refs.emplace_back(kv.first, kv.second);
    }
    for (auto& r : refs) {
        if (!R::IsLiveByIndex(r.second.actor, r.second.idx)) continue;
        WT::State cur;
        if (!WT::ReadState(r.second.actor, cur)) continue;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_lastKnown.find(r.first);
            if (it != g_lastKnown.end() && !StateDiffers(it->second, cur)) continue;
        }
        coop::net::TurbineStatePayload p{};
        StateToPayload(r.first, cur, p);
        if (s->SendReliable(coop::net::ReliableKind::TurbineState, &p, sizeof(p))) {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_lastKnown[r.first] = cur;
        }
    }
}

void OnReliable(const coop::net::TurbineStatePayload& payload) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (s && s->role() == coop::net::Role::Host) return;  // host-auth: never applied on the host
    if (!PayloadFinite(payload)) {
        UE_LOGW("turbine: non-finite payload -- dropping");
        return;
    }
    std::wstring key = StringFromWireKey(payload.key);
    if (key.empty()) return;
    if (!WT::EnsureResolved()) return;  // class not loaded -- the snapshot will re-send within 1 s anyway
    WT::State want = PayloadToState(payload);
    if (void* t = ResolveFast(key)) { ApplyState(t, key, want); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    g_pending[key] = Pending{ want, std::chrono::steady_clock::now() + kPendingTTL };
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (!WT::EnsureResolved()) return;
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
        WT::State cur;
        if (!WT::ReadState(d.second.actor, cur)) continue;
        coop::net::TurbineStatePayload p{};
        StateToPayload(d.first, cur, p);
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::TurbineState, &p, sizeof(p));
        ++sent;
    }
    UE_LOGI("turbine: connect-snapshot -- sent %d state(s) to slot %d", sent, peerSlot);
}

void OnDisconnect() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_lastKnown.clear();
    g_pending.clear();
}

}  // namespace coop::turbine_sync
