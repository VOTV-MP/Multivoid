// coop/world/hexpillar_sync.cpp -- see coop/world/hexpillar_sync.h.
//
// 0.9.0n. prop_hexpillar_C::digUp spawns a hexpillar_C at the stone (a dirt hole's begin play calls it on what it
// overlaps). hexpillar_C::ReceiveTick, while `ticking`: `a` grows by a random ease and `upd` sets the pillar mesh's
// height and scale and the box's extent from it; at a random weight of a cubed it takes one of its `i`, traces beside
// itself toward player 0 and, on grass or gravel, mints a hexpillar_C there; at `i` 0 it stops ticking. Its addDamage,
// exploded and reachedByExplosion feed its physics-impact part, which ends it.

#include "coop/world/hexpillar_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/world/anomaly_ask.h"
#include "coop/world/world_actor_sync.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_instances.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace coop::hexpillar_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace E  = ue_wrap::engine;
namespace WA = coop::world_actor_sync;

UE_FACT_FN(kRowDigUp, "hexpillar_sync", L"prop_hexpillar_C", L"digUp", Declared);
UE_FACT_FN(kRowExploded, "hexpillar_sync", L"hexpillar_C", L"exploded", Declared);
UE_FACT_FN(kRowReachedByExplosion, "hexpillar_sync", L"hexpillar_C", L"reachedByExplosion", Declared);

constexpr const wchar_t* kStoneClass = L"prop_hexpillar_C";
constexpr int kTagDig = 0x48504447;     // 'HPDG'
constexpr int kTagBlast = 0x48504258;   // 'HPBX'
constexpr int kCap = 1024;              // pillars read a pass
constexpr uint64_t kPassMs = 500;
constexpr uint64_t kRefreshMs = 10000;
constexpr int kSendsPerPass = 48;
constexpr float kRiseStep = 0.02f;
constexpr float kDigReachCm = 2000.f;   // the sender's body to the stone
constexpr float kStoneNearCm = 300.f;   // the asked place to the host's stone
constexpr float kMatchCm = 100.f;       // a cue's place to a client's mirror

std::atomic<N::Session*> g_session{nullptr};
bool g_watched = false;
uint64_t g_nextPass = 0, g_nextRefresh = 0;
std::unordered_map<void*, float> g_sentRise;   // HOST: the `a` last sent per pillar
bool g_refreshing = false;
void* g_pillars[kCap];
ue_wrap::FVector g_at{};                       // CLIENT: the place 131 named
bool g_haveAt = false;
int g_ended = 0;

N::Session* Session() { return g_session.load(std::memory_order_acquire); }
bool IsClient() {
    N::Session* const s = Session();
    return s && s->role() == N::Role::Client;
}

float Dist2(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

bool ReadRise(void* pillar, float& out) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(pillar), L"a");
    if (off < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(pillar) + off, sizeof(out));
    return std::isfinite(out);
}

void ApplyRise(void* pillar, float a) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(pillar), L"a");
    if (off < 0) return;
    std::memcpy(static_cast<uint8_t*>(pillar) + off, &a, sizeof(a));
    ue_wrap::ParamFrame pf(R::FindDispatchFunction(R::ClassOf(pillar), L"upd", nullptr));
    if (pf.valid()) ue_wrap::Call(pillar, pf);
}

void Send(N::Session& s, uint32_t cue, float x, float y, float z) {
    N::EventCuePayload p{};
    p.cueId = cue;
    p.x = x;
    p.y = y;
    p.z = z;
    s.SendReliable(N::ReliableKind::EventCue, &p, sizeof(p));
}

// CLIENT: a dirt hole's dig of the stone is the host's to run.
sg::Verdict OnDigPre(const sg::Call& call) {
    if (call.tag != kTagDig || !IsClient() || !call.object) return sg::Verdict::Run;
    ue_wrap::FVector at{};
    E::TryGetActorLocation(call.object, at);
    const bool sent = coop::anomaly_ask::Send(N::kAnomalyAskDigPillar, at.X, at.Y, at.Z);
    UE_LOGI("hexpillar: this client dug the basalt stone at (%.0f,%.0f,%.0f) -- asked of the host (sent=%d)", at.X, at.Y,
            at.Z, sent ? 1 : 0);
    return sg::Verdict::Cancel;
}

// CLIENT: an explosion's reach of a mirror is the host's (its own explosion reaches the host's pillar).
sg::Verdict OnBlastPre(const sg::Call& call) {
    return call.tag == kTagBlast && IsClient() && WA::IsMirroredActor(call.object) ? sg::Verdict::Cancel
                                                                                   : sg::Verdict::Run;
}

void Watch() {
    if (g_watched) return;
    bool all = sg::WatchName(kRowDigUp, kTagDig, &OnDigPre, nullptr);
    all = sg::WatchName(kRowExploded, kTagBlast, &OnBlastPre, nullptr) && all;
    all = sg::WatchName(kRowReachedByExplosion, kTagBlast, &OnBlastPre, nullptr) && all;
    if (all) {
        g_watched = true;
        UE_LOGI("hexpillar: the stone's dig and a pillar's explosion reach are watched");
    }
}

void HostPass(N::Session& s, uint64_t now) {
    const int n = ue_wrap::world_instances::Find(kPillarClass, g_pillars, kCap);
    if (now >= g_nextRefresh) {
        g_nextRefresh = now + kRefreshMs;
        g_refreshing = true;
    }
    int sends = 0, enrolled = 0;
    bool done = true;
    for (int k = 0; k < n; ++k) {
        void* const p = g_pillars[k];
        if (!p || !R::IsLive(p)) continue;
        if (WA::HostEnrollExSpawn(p) != 0 && !g_sentRise.count(p)) ++enrolled;
        float a = 0.f;
        if (!ReadRise(p, a)) continue;
        auto it = g_sentRise.find(p);
        const bool moved = it == g_sentRise.end() || std::fabs(a - it->second) >= kRiseStep ||
                           (a >= 1.f && it->second < 1.f);
        if (!moved && !g_refreshing) continue;
        if (sends >= kSendsPerPass) {
            done = false;
            break;
        }
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(p, at)) continue;
        Send(s, kCuePillarAt, at.X, at.Y, at.Z);
        Send(s, kCuePillarRise, a, 0.f, 0.f);
        g_sentRise[p] = a;
        ++sends;
    }
    if (done) g_refreshing = false;
    if (enrolled) UE_LOGI("hexpillar: %d pillar(s) enrolled as the host's (%d standing)", enrolled, n);
    // Forget the pillars that ended.
    for (auto it = g_sentRise.begin(); it != g_sentRise.end();)
        it = R::IsLive(it->first) ? std::next(it) : g_sentRise.erase(it);
}

// CLIENT: its own pillars (a save's, or one born before a refusal) are ended; the host's mirrors stand.
void ClientSweep() {
    if (WA::IsMaterializingMirror()) return;
    const int n = ue_wrap::world_instances::Find(kPillarClass, g_pillars, kCap);
    for (int k = 0; k < n; ++k) {
        void* const p = g_pillars[k];
        if (!p || !R::IsLive(p) || WA::IsMirroredActor(p)) continue;
        if (E::DestroyActor(p) && (++g_ended == 1 || (g_ended & (g_ended - 1)) == 0))
            UE_LOGI("hexpillar: this client's own pillar ended -- the host's cross (%d so far)", g_ended);
    }
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* const s = Session();
    if (!s || !s->running()) return;
    Watch();
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextPass) return;
    g_nextPass = now + kPassMs;
    if (s->role() == N::Role::Host) HostPass(*s, now);
    else if (s->connected()) ClientSweep();
}

bool OnCue(const N::EventCuePayload& p) {
    if (p.cueId != kCuePillarAt && p.cueId != kCuePillarRise) return false;
    if (!IsClient() || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return true;
    if (p.cueId == kCuePillarAt) {
        g_at = ue_wrap::FVector{p.x, p.y, p.z};
        g_haveAt = true;
        return true;
    }
    if (!g_haveAt) return true;
    g_haveAt = false;
    const int n = ue_wrap::world_instances::Find(kPillarClass, g_pillars, kCap);
    void* best = nullptr;
    float bestD = kMatchCm * kMatchCm;
    for (int k = 0; k < n; ++k) {
        void* const q = g_pillars[k];
        ue_wrap::FVector at{};
        if (!q || !R::IsLive(q) || !WA::IsMirroredActor(q) || !E::TryGetActorLocation(q, at)) continue;
        const float d = Dist2(at, g_at);
        if (d < bestD) {
            bestD = d;
            best = q;
        }
    }
    if (best) ApplyRise(best, std::fmin(std::fmax(p.x, 0.f), 1.f));
    return true;
}

void OnDigAsk(uint8_t slot, float x, float y, float z) {
    const ue_wrap::FVector at{x, y, z};
    coop::RemotePlayer* const rp = coop::players::Registry::Get().Puppet(slot);
    void* const body = rp ? rp->actor() : nullptr;
    ue_wrap::FVector bodyAt{};
    if (!body || !E::TryGetActorLocation(body, bodyAt) || Dist2(bodyAt, at) > kDigReachCm * kDigReachCm) {
        UE_LOGW("hexpillar: slot %u's dig at (%.0f,%.0f,%.0f) refused: its body is not within %.0f m", slot, x, y, z,
                kDigReachCm / 100.f);
        return;
    }
    void* stones[16];
    const int n = ue_wrap::world_instances::Find(kStoneClass, stones, 16);
    void* best = nullptr;
    float bestD = kStoneNearCm * kStoneNearCm;
    for (int k = 0; k < n; ++k) {
        ue_wrap::FVector sAt{};
        if (!stones[k] || !R::IsLive(stones[k]) || !E::TryGetActorLocation(stones[k], sAt)) continue;
        const float d = Dist2(sAt, at);
        if (d < bestD) {
            bestD = d;
            best = stones[k];
        }
    }
    if (!best) {
        UE_LOGW("hexpillar: slot %u's dig at (%.0f,%.0f,%.0f) refused: no stone here within %.0f m", slot, x, y, z,
                kStoneNearCm / 100.f);
        return;
    }
    ue_wrap::ParamFrame pf(R::FindDispatchFunction(R::ClassOf(best), L"digUp", nullptr));
    const bool ran = pf.valid() && ue_wrap::Call(best, pf);
    UE_LOGI("hexpillar: slot %u dug the basalt stone up -- the host's digUp ran=%d", slot, ran ? 1 : 0);
}

void OnDisconnect() {
    g_nextPass = g_nextRefresh = 0;
    g_sentRise.clear();
    g_refreshing = g_haveAt = false;
    g_ended = 0;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::hexpillar_sync
