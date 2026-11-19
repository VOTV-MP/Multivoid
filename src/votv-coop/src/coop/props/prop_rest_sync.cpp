// coop/props/prop_rest_sync.cpp -- a client's resting copy of a keyed prop put where the host's rests. See the header.

#include "coop/props/prop_drive_rules.h"
#include "coop/props/prop_rest_sync.h"

#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_stick_sync.h"
#include "coop/props/prop_wire_parity.h"
#include "coop/props/remote_prop.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_physics.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::prop_rest_sync {
namespace {

namespace N = coop::net;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
using ue_wrap::FRotator;
using ue_wrap::FVector;
using Clock = std::chrono::steady_clock;

constexpr float kOffCm = 2.f;          // a copy further than this from the host's rest is put there
constexpr float kOffDeg = 3.f;         // or turned further than this
constexpr float kRestCmS = 1.f;        // at rest, as prop_drive_host reads it
constexpr auto kRestedFor = std::chrono::milliseconds(1000);   // a copy unmoved this long is asked for
constexpr auto kRestPassAfter = std::chrono::seconds(30);         // a joiner's world settles first
constexpr int kMaxCorrections = 3;     // per prop per minute, then left alone
constexpr auto kCorrectionWindow = std::chrono::seconds(60);
constexpr size_t kMaxPending = 128;    // a client's props waiting to rest
constexpr auto kAskEvery = std::chrono::milliseconds(200);

N::Session* g_session = nullptr;

float Dist2(const FVector& a, const FVector& b) {
    const float x = a.X - b.X, y = a.Y - b.Y, z = a.Z - b.Z;
    return x * x + y * y + z * z;
}

bool AtRest(void* actor) {
    const ue_wrap::prop::VelocityState v = ue_wrap::prop::GetPhysicsVelocity(actor, /*linearOnly=*/true);
    if (!v.ok) return true;
    const FVector& l = v.linearCmS;
    return l.X * l.X + l.Y * l.Y + l.Z * l.Z < kRestCmS * kRestCmS;
}

// The live actor of host element `eid`, validated by its index; null when this peer binds none.
void* ActorOf(uint32_t eid) {
    coop::element::Element* el = coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(eid));
    void* actor = el ? el->GetActor() : nullptr;
    if (!actor || !R::IsLiveByIndex(actor, el->GetInternalIdx())) return nullptr;
    return actor;
}

int16_t Cdeg(float deg) { return static_cast<int16_t>(std::lround(ue_wrap::NormalizeAxis(deg) * 100.f)); }
float Deg(int16_t c) { return static_cast<float>(c) / 100.f; }

// ---------------------------------------------------------------- HOST

struct Outbox {
    std::deque<uint32_t> asked;      // answers first
    std::deque<uint32_t> pass;       // then the rest pass, paced
    std::unordered_set<uint32_t> queued;
    Clock::time_point passAt{};     // armed: the rest pass fills `pass` at this instant
    bool passArmed = false;
    double budget = 0.0;             // rest pass rows this slot may send now
    N::PropRestPosePayload batch{};
    Clock::time_point batchSince{};
};
Outbox g_out[N::kMaxPeers];
Clock::time_point g_lastHostTick{};
uint64_t g_rowsSent = 0, g_passesSent = 0;

void Flush(int slot) {
    Outbox& o = g_out[slot];
    if (!o.batch.count || !g_session) return;
    g_session->SendReliableToSlot(slot, N::ReliableKind::PropRestPose, &o.batch, sizeof(o.batch));
    g_rowsSent += o.batch.count;
    o.batch = N::PropRestPosePayload{};
}

// One row of the host's rest pose of `eid`, when it is at rest, in no hand and under no row; false otherwise (a stream
// carries it, or it is gone).
bool RowOf(uint32_t eid, N::PropRestRow& row) {
    void* actor = ActorOf(eid);
    if (!actor || ue_wrap::prop::IsChipPile(actor)) return false;
    if (coop::prop_drive_host::InAHand(actor) || coop::prop_drive_host::IsDriven(actor)) return false;
    FVector loc{};
    FRotator rot{};
    if (!E::TryGetActorLocation(actor, loc) || !E::TryGetActorRotation(actor, rot) || !AtRest(actor)) return false;
    row.eid = eid;
    row.loc[0] = loc.X; row.loc[1] = loc.Y; row.loc[2] = loc.Z;
    row.rotCdeg[0] = Cdeg(rot.Pitch); row.rotCdeg[1] = Cdeg(rot.Yaw); row.rotCdeg[2] = Cdeg(rot.Roll);
    row.physFlags = coop::prop_wire_parity::PhysFlagsOf(actor);
    row.driveGen = coop::prop_drive_host::ContextOf(actor, eid).gen;
    return true;
}

void Push(int slot, uint32_t eid) {
    Outbox& o = g_out[slot];
    N::PropRestRow row{};
    if (!RowOf(eid, row)) return;
    if (!o.batch.count) o.batchSince = Clock::now();
    o.batch.rows[o.batch.count++] = row;
    if (o.batch.count == N::kPropRestPoseMax) Flush(slot);
}

void ArmRestPass(int slot) {
    Outbox& o = g_out[slot];
    std::unordered_map<coop::element::ElementId, FVector> props;
    coop::prop_element_tracker::CollectTrackedKeyedPropTransforms(props);
    for (const auto& [eid, pos] : props) {
        (void)pos;
        const uint32_t e = static_cast<uint32_t>(eid);
        if (o.queued.insert(e).second) o.pass.push_back(e);
    }
    ++g_passesSent;
    UE_LOGI("prop_rest_sync: slot %d rest pass -- %zu keyed props' rest poses to send at %d a second", slot, o.pass.size(),
            kRestPassRowsPerSec);
}

void TickHost(Clock::time_point now) {
    const double dt = g_lastHostTick == Clock::time_point{}
                          ? 0.0
                          : std::chrono::duration<double>(now - g_lastHostTick).count();
    g_lastHostTick = now;
    for (int slot = 1; slot < N::kMaxPeers; ++slot) {
        Outbox& o = g_out[slot];
        if (o.passArmed && now >= o.passAt) {
            o.passArmed = false;
            ArmRestPass(slot);
        }
        // Answers are few (a client's own moves) and go at once.
        while (!o.asked.empty()) {
            const uint32_t eid = o.asked.front();
            o.asked.pop_front();
            o.queued.erase(eid);
            Push(slot, eid);
        }
        if (!o.pass.empty()) {
            o.budget = std::fmin(o.budget + dt * kRestPassRowsPerSec, static_cast<double>(N::kPropRestPoseMax) * 2.0);
            // A row the prop cannot make (moving, in a hand, gone) spends no budget, but the scan is bounded per tick.
            int scanned = 0;
            while (!o.pass.empty() && o.budget >= 1.0 && scanned < 64) {
                const uint32_t eid = o.pass.front();
                o.pass.pop_front();
                o.queued.erase(eid);
                ++scanned;
                const uint8_t before = o.batch.count;
                const uint64_t sentBefore = g_rowsSent;
                Push(slot, eid);
                if (o.batch.count != before || g_rowsSent != sentBefore) o.budget -= 1.0;
            }
            if (o.pass.empty()) UE_LOGI("prop_rest_sync: slot %d rest pass sent (%llu rows so far this session)", slot,
                                         static_cast<unsigned long long>(g_rowsSent + o.batch.count));
        }
        if (o.batch.count && (o.pass.empty() || now - o.batchSince > std::chrono::milliseconds(100))) Flush(slot);
    }
}

// ---------------------------------------------------------------- CLIENT

struct Pending {
    ue_wrap::CachedObjRef ref;
    FVector loc{};
    FRotator rot{};
    Clock::time_point stillSince{};
};
std::unordered_map<void*, Pending> g_pending;
struct Watch {
    ue_wrap::CachedObjRef ref;
    FVector loc{};
    FRotator rot{};
    Clock::time_point until{};
};
std::vector<Watch> g_watch;
constexpr size_t kMaxWatch = 256;
uint64_t g_endMoves = 0;   // copies that moved by themselves after an End
std::vector<uint32_t> g_ask;
Clock::time_point g_nextAsk{};
struct Corrected {
    int n = 0;
    Clock::time_point since{};
    bool said = false;
};
std::unordered_map<uint32_t, Corrected> g_corrected;
uint64_t g_applied = 0, g_agreed = 0, g_skipped = 0;
float g_maxCm = 0.f;

// The copy is the stream's or the hand's now: whatever it carries is the truth, not a rest row.
bool Streamed(void* actor) {
    void* me = coop::players::Registry::Get().Local();
    return (me && E::IsMainPlayerGrabbing(me, actor)) || coop::remote_prop::IsActorUnderAnyDrive(actor) ||
           coop::prop_drive_stream::IsParked(actor);
}

void SendAsk() {
    if (g_ask.empty() || !g_session) return;
    N::PropRestQueryPayload p{};
    size_t i = 0;
    while (i < g_ask.size()) {
        p = N::PropRestQueryPayload{};
        for (; i < g_ask.size() && p.count < N::kPropRestQueryMax; ++i) p.eids[p.count++] = g_ask[i];
        g_session->SendReliable(N::ReliableKind::PropRestQuery, &p, sizeof(p));
    }
    g_ask.clear();
}

void TickClient(Clock::time_point now) {
    if (now < g_nextAsk) return;
    g_nextAsk = now + kAskEvery;
    for (size_t i = 0; i < g_watch.size();) {
        Watch& w = g_watch[i];
        void* actor = w.ref.Get();
        FVector loc{};
        FRotator rot{};
        bool drop = !actor || now >= w.until || Streamed(actor) || !E::TryGetActorLocation(actor, loc) ||
                    !E::TryGetActorRotation(actor, rot);
        if (!drop && (Dist2(loc, w.loc) > kOffCm * kOffCm || coop::prop_drive_rules::TurnDeg(rot, w.rot) > kOffDeg)) {
            ++g_endMoves;
            if (g_endMoves <= 3 || (g_endMoves & (g_endMoves - 1)) == 0)
                UE_LOGI("prop_rest_sync: '%ls' moved %.1f cm by itself after its End -- the host's rest is asked for (#%llu)",
                        R::ClassNameOf(actor).c_str(), std::sqrt(Dist2(loc, w.loc)), static_cast<unsigned long long>(g_endMoves));
            NoteLocalMove(actor);
            drop = true;
        }
        if (drop) {
            g_watch[i] = g_watch.back();
            g_watch.pop_back();
        } else {
            ++i;
        }
    }
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        Pending& pd = it->second;
        void* actor = pd.ref.Get();
        FVector loc{};
        FRotator rot{};
        if (!actor || Streamed(actor) || !E::TryGetActorLocation(actor, loc) || !E::TryGetActorRotation(actor, rot)) {
            it = g_pending.erase(it);
            continue;
        }
        if (Dist2(loc, pd.loc) > 0.25f || coop::prop_drive_rules::TurnDeg(rot, pd.rot) > 0.5f) {
            pd.loc = loc;
            pd.rot = rot;
            pd.stillSince = now;
            ++it;
            continue;
        }
        if (now - pd.stillSince < kRestedFor) {
            ++it;
            continue;
        }
        const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(actor);
        if (eid != coop::element::kInvalidId && eid != 0) g_ask.push_back(static_cast<uint32_t>(eid));
        it = g_pending.erase(it);
    }
    SendAsk();
}

void Apply(const N::PropRestRow& r) {
    void* actor = ActorOf(r.eid);
    if (!actor || Streamed(actor) || ue_wrap::prop::IsChipPile(actor)) { ++g_skipped; return; }
    // A row read under a drive the client has since seen end is older than that End's pose.
    if (coop::prop_drive_stream::IsCorrectionStale(r.eid, r.driveGen, false)) { ++g_skipped; return; }
    FVector cur{};
    FRotator curRot{};
    if (!E::TryGetActorLocation(actor, cur) || !E::TryGetActorRotation(actor, curRot)) { ++g_skipped; return; }
    const FVector to{r.loc[0], r.loc[1], r.loc[2]};
    const FRotator toRot{Deg(r.rotCdeg[0]), Deg(r.rotCdeg[1]), Deg(r.rotCdeg[2])};
    const float cm2 = Dist2(cur, to);
    const float turn = coop::prop_drive_rules::TurnDeg(curRot, toRot);
    if (cm2 <= kOffCm * kOffCm && turn <= kOffDeg) { ++g_agreed; return; }
    // A copy still moving here comes to rest first: its own move asks again when it does.
    if (!AtRest(actor)) { NoteLocalMove(actor); ++g_skipped; return; }
    const auto now = Clock::now();
    Corrected& c = g_corrected[r.eid];
    if (now - c.since > kCorrectionWindow) c = Corrected{0, now, false};
    if (c.n >= kMaxCorrections) {
        if (!c.said) {
            c.said = true;
            UE_LOGI("prop_rest_sync: '%ls' eid=%u corrected %d times in a minute and still off by %.1f cm -- left alone",
                    R::ClassNameOf(actor).c_str(), r.eid, c.n, std::sqrt(cm2));
        }
        ++g_skipped;
        return;
    }
    ++c.n;
    coop::prop_wire_parity::NoteCorrection(actor, to, "rest-sync");
    E::SetActorLocation(actor, to);
    E::SetActorRotation(actor, toRot);
    coop::prop_stick_sync::ConvergeStuck(actor, r.physFlags);
    coop::prop_wire_parity::ConvergeStatic(actor, r.physFlags);
    coop::prop_wire_parity::ConvergeFrozenSleep(actor, r.physFlags);
    // The host's copy rests here: this one rests here too, asleep, as a drive's End leaves it, so no second simulation
    // settles it elsewhere (a thin copy pushed out through the top it lies on). Touched, it wakes as the host's would.
    if (void* mesh = ue_wrap::prop::GetStaticMesh(actor); mesh && E::IsComponentSimulatingPhysics(mesh))
        E::PutComponentToSleep(mesh);
    WatchAfterEnd(actor);   // and should it move by itself all the same, it is asked for again
    ++g_applied;
    const float cm = std::sqrt(cm2);
    if (cm > g_maxCm) g_maxCm = cm;
    if (g_applied <= 5 || (g_applied & (g_applied - 1)) == 0)
        UE_LOGI("prop_rest_sync: '%ls' eid=%u put at the host's rest, %.1f cm and %.1f deg off (#%llu)",
                R::ClassNameOf(actor).c_str(), r.eid, cm, turn, static_cast<unsigned long long>(g_applied));
}

}  // namespace

void NoteLocalMove(void* actor) {
    if (!actor || g_pending.size() >= kMaxPending || g_pending.count(actor)) return;
    Pending pd;
    pd.ref.Set(actor);
    E::TryGetActorLocation(actor, pd.loc);
    E::TryGetActorRotation(actor, pd.rot);
    pd.stillSince = Clock::now();
    g_pending.emplace(actor, pd);
}

void WatchAfterEnd(void* actor) {
    if (!actor || g_watch.size() >= kMaxWatch) return;
    Watch w;
    w.ref.Set(actor);
    if (!E::TryGetActorLocation(actor, w.loc) || !E::TryGetActorRotation(actor, w.rot)) return;
    w.until = Clock::now() + std::chrono::milliseconds(kWatchAfterEndMs);
    for (Watch& old : g_watch)
        if (old.ref.Raw() == actor) {
            old = w;
            return;
        }
    g_watch.push_back(w);
}

void OnPeerWorldReady(int slot) {
    if (slot < 1 || slot >= N::kMaxPeers) return;
    Outbox& o = g_out[slot];
    o.passArmed = true;
    o.passAt = Clock::now() + kRestPassAfter;
}

void OnQuery(int slot, const N::PropRestQueryPayload& p) {
    UE_ASSERT_GAME_THREAD("prop_rest_sync::OnQuery");
    if (slot < 1 || slot >= N::kMaxPeers) return;
    Outbox& o = g_out[slot];
    for (int i = 0; i < p.count && i < N::kPropRestQueryMax; ++i)
        if (o.queued.insert(p.eids[i]).second) o.asked.push_back(p.eids[i]);
}

void OnPose(const N::PropRestPosePayload& p) {
    UE_ASSERT_GAME_THREAD("prop_rest_sync::OnPose");
    for (int i = 0; i < p.count && i < N::kPropRestPoseMax; ++i) Apply(p.rows[i]);
}

void Tick(N::Session& s, bool isHost) {
    UE_ASSERT_GAME_THREAD("prop_rest_sync::Tick");
    g_session = &s;
    if (!s.connected()) return;
    const auto now = Clock::now();
    if (isHost) TickHost(now);
    else TickClient(now);
}

void OnDisconnect() {
    if (g_rowsSent || g_applied || g_agreed)
        UE_LOGI("prop_rest_sync: session end -- host rows sent %llu (%llu rest passes); client copies put right %llu (max %.1f cm), "
                "agreed %llu, skipped %llu, moved by themselves after an End %llu",
                static_cast<unsigned long long>(g_rowsSent), static_cast<unsigned long long>(g_passesSent),
                static_cast<unsigned long long>(g_applied), g_maxCm, static_cast<unsigned long long>(g_agreed),
                static_cast<unsigned long long>(g_skipped), static_cast<unsigned long long>(g_endMoves));
    for (Outbox& o : g_out) o = Outbox{};
    g_pending.clear();
    g_watch.clear();
    g_endMoves = 0;
    g_ask.clear();
    g_corrected.clear();
    g_rowsSent = g_passesSent = g_applied = g_agreed = g_skipped = 0;
    g_maxCm = 0.f;
    g_lastHostTick = {};
    g_nextAsk = {};
}

}  // namespace coop::prop_rest_sync
