// coop/creatures/scout_target.cpp -- see coop/creatures/scout_target.h.
//
// tentacleBall_C (0.9.0n), its tick while `isHostile`: not `chasing`, it traces from its light to each body within
// 150 m and, at a mainPlayer_C, sets `chasing`, calms its buddies and moves to GetPlayerPawn(0); chasing, it takes the
// first mainPlayer_C within 40 m, tests player 0's camera (GetPlayerCameraManager(0)) and then GetPlayerPawn(0) for sky
// above, and pins that player: `isAnim`, the player's firstPersonBodyAnimation("pindown"), its model turned to the floor,
// `chasing` and `isHostile` off, and its own place lerped half way to GetPlayerPawn(0). Its move callbacks move to
// GetPlayerPawn(0) again while it chases.

#include "coop/creatures/scout_target.h"

#include "coop/creatures/served_player.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_instances.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace coop::scout_target {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace SP = coop::served_player;
namespace UH = ue_wrap::ufunction_hook;

constexpr const wchar_t* kBall = L"tentacleBall_C";
UE_FACT_FN(kRowBodyAnim, "scout_target", L"mainPlayer_C", L"firstPersonBodyAnimation", Declared);
UE_FACT_FN(kRowCameraManager, "scout_target", L"GameplayStatics", L"GetPlayerCameraManager", Declared);
constexpr int kTag = 0x53434F55;   // 'SCOU'
constexpr uint64_t kPeriodMs = 200;

std::atomic<N::Session*> g_session{nullptr};
bool g_watched = false;
bool g_hooked = false;
bool g_armed = false;
uint64_t g_nextAt = 0;
unsigned g_pinsSent = 0;

int ReadBool(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask) || off < 0) return -1;
    return (static_cast<const uint8_t*>(obj)[off] & mask) != 0 ? 1 : 0;
}

bool IsBall(void* obj) { return obj && R::ClassNameOf(obj) == kBall; }

// The nearest player body to `at`: 0 the host's own, else a client slot whose puppet has a body and a pose.
uint8_t NearestPlayer(const ue_wrap::FVector& at) {
    auto& reg = coop::players::Registry::Get();
    uint8_t best = 0;
    float bestD = 3.4e38f;
    ue_wrap::FVector p{};
    if (void* me = reg.Local(); me && ue_wrap::engine::TryGetActorLocation(me, p)) {
        const float dx = p.X - at.X, dy = p.Y - at.Y, dz = p.Z - at.Z;
        bestD = dx * dx + dy * dy + dz * dz;
    }
    for (uint8_t slot = 1; slot < N::kMaxPeers; ++slot) {
        coop::RemotePlayer* const rp = reg.Puppet(slot);
        if (!rp || !rp->valid() || !rp->HasPose() || !rp->GetActor() || !rp->TryGetLocation(p)) continue;
        const float dx = p.X - at.X, dy = p.Y - at.Y, dz = p.Z - at.Z;
        const float d = dx * dx + dy * dy + dz * dz;
        if (d < bestD) {
            bestD = d;
            best = slot;
        }
    }
    return best;
}

// A served ball's GetPlayerCameraManager(0), its first sky check, answers its target's body: the Blueprint reads only
// that actor's place from it.
void OnCameraManagerPost(void* /*context*/, void* source, void* /*result*/) {
    if (!IsBall(source) || !ue_wrap::game_thread::IsGameThread()) return;
    const uint8_t slot = SP::ServedSlot(source);
    coop::RemotePlayer* const rp = slot ? coop::players::Registry::Get().Puppet(slot) : nullptr;
    void* const body = rp && rp->valid() && rp->HasPose() ? rp->GetActor() : nullptr;
    if (!body) return;
    if (void* r = UH::CurrentResult()) *static_cast<void**>(r) = body;
}

// HOST: a ball's pin of a client's puppet plays on that client's own player, not on the host's copy of it.
sg::Verdict OnBodyAnimPre(const sg::Call& c) {
    if (!c.object || !IsBall(c.callerObject)) return sg::Verdict::Run;
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != N::Role::Host) return sg::Verdict::Run;
    const uint8_t slot = coop::players::Registry::Get().PeerIdOfActor(c.object);
    if (slot == 0 || slot >= N::kMaxPeers) return sg::Verdict::Run;
    ue_wrap::FVector at{};
    ue_wrap::engine::TryGetActorLocation(c.callerObject, at);
    N::EventCuePayload p{};
    p.cueId = kCueScoutPin;
    p.x = at.X;
    p.y = at.Y;
    p.z = at.Z;
    const bool sent = s->SendReliableToSlot(slot, N::ReliableKind::EventCue, &p, sizeof(p));
    ++g_pinsSent;
    UE_LOGI("scout: a ball pins slot %u's player -- EventCue %s (%u sent)", static_cast<unsigned>(slot),
            sent ? "sent" : "not sent", g_pinsSent);
    return sg::Verdict::Cancel;
}

void ServeBalls() {
    void* balls[32];
    const int32_t n = ue_wrap::world_instances::Find(kBall, balls, 32);
    bool any = false;
    for (int32_t i = 0; i < n; ++i) {
        void* const ball = balls[i];
        if (!ball || !R::IsLive(ball)) continue;
        if (ReadBool(ball, L"isHostile") != 1) {
            if (SP::ServedSlot(ball)) SP::Unserve(ball);
            continue;
        }
        const uint8_t had = SP::ServedSlot(ball);
        // A chase or a pin keeps its player; a ball still looking takes the nearest.
        const bool locked = ReadBool(ball, L"chasing") == 1 || ReadBool(ball, L"isAnim") == 1;
        ue_wrap::FVector at{};
        if (!locked && ue_wrap::engine::TryGetActorLocation(ball, at)) {
            const uint8_t want = NearestPlayer(at);
            if (want != had) {
                if (want == 0) SP::Unserve(ball);
                else if (!SP::Serve(ball, want, SP::kPlayerPawn)) continue;
                UE_LOGI("scout: ball %ls hunts %s%u", R::ToString(R::NameOf(ball)).c_str(), want ? "slot " : "the host ",
                        static_cast<unsigned>(want));
            }
        }
        any = any || SP::ServedSlot(ball) != 0;
    }
    if (g_hooked && any != g_armed) {
        g_armed = any;
        UH::SetArmed(ue_wrap::facts::Resolve(kRowCameraManager).Fn(), &OnCameraManagerPost, any);
    }
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != N::Role::Host) return;
    if (!g_watched && (g_watched = sg::WatchName(kRowBodyAnim, kTag, &OnBodyAnimPre, nullptr)))
        UE_LOGI("scout: a player's body animation is watched");
    if (!g_hooked && (g_hooked = UH::InstallPostHook(ue_wrap::facts::Resolve(kRowCameraManager), &OnCameraManagerPost,
                                                      false)))
        UE_LOGI("scout: a ball's camera read is hooked, disarmed");
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextAt || !s->connected() || !SP::SeamsReady(SP::kPlayerPawn)) return;
    g_nextAt = now + kPeriodMs;
    ServeBalls();
}

bool OnCue(const N::EventCuePayload& p) {
    if (p.cueId != kCueScoutPin) return false;
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != N::Role::Client) return true;
    void* const me = coop::players::Registry::Get().Local();
    void* const fn = me ? R::FindFunction(R::ClassOf(me), L"firstPersonBodyAnimation") : nullptr;
    ue_wrap::ParamFrame f(fn);
    bool ran = false;
    if (f.valid()) {
        f.Set<R::FName>(L"startingSection", ue_wrap::fname_utils::StringToFName(L"pindown"));
        ran = ue_wrap::Call(me, f);
    }
    UE_LOGI("scout: the host's ball at (%.0f,%.0f,%.0f) pins this player (ran=%d)", p.x, p.y, p.z, ran ? 1 : 0);
    return true;
}

void OnDisconnect() {
    if (g_hooked && g_armed) UH::SetArmed(ue_wrap::facts::Resolve(kRowCameraManager).Fn(), &OnCameraManagerPost, false);
    g_armed = false;
    g_nextAt = 0;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::scout_target
