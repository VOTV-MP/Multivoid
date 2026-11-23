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
#include "coop/world/ambient_births.h"
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
#include <vector>

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
constexpr int kCap = 1024;              // pillars a client matches a cue's place against
constexpr uint64_t kPassMs = 500;
constexpr int kSendsPerPass = 48;
constexpr uint64_t kPendingMs = 30000;  // CLIENT: a rise whose mirror has not come yet waits this long
constexpr float kRiseStep = 0.02f;
constexpr float kDigReachCm = 2000.f;   // the sender's body to the stone
constexpr float kStoneNearCm = 300.f;   // the asked place to the host's stone
constexpr float kMatchCm = 100.f;       // a cue's place to a client's mirror

std::atomic<N::Session*> g_session{nullptr};
bool g_watched = false;
uint64_t g_nextPass = 0;
std::unordered_map<void*, float> g_sentRise;   // HOST: every pillar born (the birth hub), and the `a` last sent
std::vector<void*> g_join[N::kMaxPeers];       // HOST: a joiner's pillars, sent in batches
void* g_pillars[kCap];
ue_wrap::FVector g_at{};                       // CLIENT: the place 131 named
bool g_haveAt = false;
struct Pending { ue_wrap::FVector at; float a; uint64_t until; };
std::vector<Pending> g_pending;                // CLIENT: rises whose mirror has not come yet
std::vector<coop::ambient_births::Event> g_births;
int g_ended = 0;
int32_t g_riseOff = -1;                        // `a`'s offset, resolved once per class object
void* g_riseCls = nullptr;

N::Session* Session() { return g_session.load(std::memory_order_acquire); }
bool IsClient() {
    N::Session* const s = Session();
    return s && s->role() == N::Role::Client;
}

float Dist2(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

int32_t RiseOffset(void* pillar) {
    void* const cls = R::ClassOf(pillar);
    if (cls != g_riseCls) {
        g_riseCls = cls;
        g_riseOff = R::FindPropertyOffset(cls, L"a");
    }
    return g_riseOff;
}

bool ReadRise(void* pillar, float& out) {
    const int32_t off = RiseOffset(pillar);
    if (off < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(pillar) + off, sizeof(out));
    return std::isfinite(out);
}

void ApplyRise(void* pillar, float a) {
    const int32_t off = RiseOffset(pillar);
    if (off < 0) return;
    std::memcpy(static_cast<uint8_t*>(pillar) + off, &a, sizeof(a));
    ue_wrap::ParamFrame pf(R::FindDispatchFunction(R::ClassOf(pillar), L"upd", nullptr));
    if (pf.valid()) ue_wrap::Call(pillar, pf);
}

// To every client (`slot` -1) or to one.
void Send(N::Session& s, uint32_t cue, float x, float y, float z, int slot = -1) {
    N::EventCuePayload p{};
    p.cueId = cue;
    p.x = x;
    p.y = y;
    p.z = z;
    if (slot < 0) s.SendReliable(N::ReliableKind::EventCue, &p, sizeof(p));
    else s.SendReliableToSlot(slot, N::ReliableKind::EventCue, &p, sizeof(p));
}

bool SendPillar(N::Session& s, void* pillar, float a, int slot) {
    ue_wrap::FVector at{};
    if (!E::TryGetActorLocation(pillar, at)) return false;
    Send(s, kCuePillarAt, at.X, at.Y, at.Z, slot);
    Send(s, kCuePillarRise, a, 0.f, 0.f, slot);
    return true;
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

// HOST: the pillars the birth hub announces (the save's at its seed, every later one at its birth) are enrolled once.
void HostTakeBirths() {
    g_births.clear();
    coop::ambient_births::Drain(coop::ambient_births::Bit(coop::ambient_births::Class::Pillar), g_births);
    int enrolled = 0;
    for (const auto& e : g_births) {
        if (e.kind != coop::ambient_births::Kind::Birth || !R::IsLiveByIndex(e.obj, e.index)) continue;
        if (WA::HostEnrollExSpawn(e.obj) != 0) ++enrolled;
        g_sentRise.emplace(e.obj, std::nanf(""));   // not sent yet (a new pillar's `a` starts below 0)
    }
    if (enrolled) UE_LOGI("hexpillar: %d pillar(s) born and enrolled as the host's (%zu standing)", enrolled, g_sentRise.size());
}

// HOST: a pillar still rising is sent as its `a` moves; one at full height is done.
void HostPass(N::Session& s) {
    int sends = 0;
    for (auto it = g_sentRise.begin(); it != g_sentRise.end();) {
        void* const p = it->first;
        if (!R::IsLive(p)) {
            it = g_sentRise.erase(it);
            continue;
        }
        float a = 0.f;
        const bool done = it->second >= 1.f;
        if (!done && sends < kSendsPerPass && ReadRise(p, a) &&
            (std::isnan(it->second) || std::fabs(a - it->second) >= kRiseStep || (a >= 1.f && it->second < 1.f)) &&
            SendPillar(s, p, a, -1)) {
            it->second = a;
            ++sends;
        }
        ++it;
    }
    // A joiner's pillars, in batches after the rises.
    for (int slot = 1; slot < N::kMaxPeers && sends < kSendsPerPass; ++slot) {
        auto& q = g_join[slot];
        while (!q.empty() && sends < kSendsPerPass) {
            void* const p = q.back();
            q.pop_back();
            float a = 0.f;
            if (R::IsLive(p) && ReadRise(p, a) && SendPillar(s, p, a, slot)) ++sends;
        }
    }
}

// CLIENT: its own pillars (a save's, or one born before a refusal) end at their birth; the host's mirrors stand.
void ClientTakeBirths() {
    if (WA::IsMaterializingMirror()) return;
    g_births.clear();
    coop::ambient_births::Drain(coop::ambient_births::Bit(coop::ambient_births::Class::Pillar), g_births);
    for (const auto& e : g_births) {
        if (e.kind != coop::ambient_births::Kind::Birth || !R::IsLiveByIndex(e.obj, e.index) ||
            WA::IsMirroredActor(e.obj))
            continue;
        if (E::DestroyActor(e.obj) && (++g_ended == 1 || (g_ended & (g_ended - 1)) == 0))
            UE_LOGI("hexpillar: this client's own pillar ended at its birth -- the host's cross (%d so far)", g_ended);
    }
}

// CLIENT: the mirror nearest the place, within a metre.
void* MirrorAt(const ue_wrap::FVector& where) {
    const int n = ue_wrap::world_instances::Find(kPillarClass, g_pillars, kCap);
    void* best = nullptr;
    float bestD = kMatchCm * kMatchCm;
    for (int k = 0; k < n; ++k) {
        void* const q = g_pillars[k];
        ue_wrap::FVector at{};
        if (!q || !R::IsLive(q) || !WA::IsMirroredActor(q) || !E::TryGetActorLocation(q, at)) continue;
        const float d = Dist2(at, where);
        if (d < bestD) {
            bestD = d;
            best = q;
        }
    }
    return best;
}

// CLIENT: a rise that came before its mirror (a joiner's, ahead of the world-actor snapshot) is applied once it stands.
void ClientPending(uint64_t now) {
    size_t keep = 0;
    for (size_t i = 0; i < g_pending.size(); ++i) {
        const Pending pd = g_pending[i];
        if (now > pd.until) continue;
        if (void* const m = MirrorAt(pd.at)) {
            ApplyRise(m, pd.a);
            UE_LOGI("hexpillar: a rise the host sent before its mirror (%.2f) is drawn now", pd.a);
        } else {
            g_pending[keep++] = pd;
        }
    }
    g_pending.resize(keep);
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* const s = Session();
    if (!s || !s->running()) return;
    Watch();
    const bool host = s->role() == N::Role::Host;
    if (host) HostTakeBirths();
    else ClientTakeBirths();
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextPass || !s->connected()) return;
    g_nextPass = now + kPassMs;
    if (host) HostPass(*s);
    else if (!g_pending.empty()) ClientPending(now);
}

void SendJoin(int slot) {
    if (slot < 1 || slot >= N::kMaxPeers || !Session() || Session()->role() != N::Role::Host) return;
    auto& q = g_join[slot];
    q.clear();
    for (const auto& kv : g_sentRise)
        if (R::IsLive(kv.first)) q.push_back(kv.first);
    if (!q.empty()) UE_LOGI("hexpillar: slot %d's world is ready -- its %zu pillar(s)' rise queued", slot, q.size());
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
    const float a = std::fmin(std::fmax(p.x, -1.f), 1.f);
    static int s_drawn = 0;
    if (void* const m = MirrorAt(g_at)) {
        ApplyRise(m, a);
        if (++s_drawn == 1 || (s_drawn & (s_drawn - 1)) == 0)
            UE_LOGI("hexpillar: the host's rise (%.2f) drawn on its mirror (%d so far)", a, s_drawn);
    } else {
        g_pending.push_back(Pending{g_at, a, ::GetTickCount64() + kPendingMs});
    }
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
    g_nextPass = 0;
    g_sentRise.clear();
    for (auto& q : g_join) q.clear();
    g_pending.clear();
    g_haveAt = false;
    g_ended = 0;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::hexpillar_sync
