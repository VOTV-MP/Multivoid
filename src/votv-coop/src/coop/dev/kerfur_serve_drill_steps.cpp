// coop/dev/kerfur_serve_drill_steps.cpp -- see coop/dev/kerfur_serve_drill_steps.h.

#include "coop/dev/kerfur_serve_drill_steps.h"

#include "coop/dev/director/director.h"
#include "coop/dev/kerfur_serve_drill_internal.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/shutdown.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <memory>

namespace coop::dev::kerfur_serve_drill::steps {
namespace {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

std::atomic<coop::net::Session*> g_session{nullptr};

}  // namespace

void SetSession(coop::net::Session* s) { g_session.store(s, std::memory_order_release); }

bool Alive() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->connected() && !coop::shutdown::IsShuttingDown();
}

bool WaitGame(const std::function<bool()>& pred, uint64_t boundMs) {
    const uint64_t t0 = ::GetTickCount64();
    for (;;) {
        bool ok = false;
        bool alive = true;
        if (GT::RunAndWait([&](std::atomic<int>& done) {
                alive = Alive();
                ok = alive && pred();
                done.store(1);
            }) != 1)
            return false;
        if (ok) return true;
        if (!alive || ::GetTickCount64() - t0 >= boundMs) return false;
        ::Sleep(100);
    }
}

bool Run(const std::function<bool()>& fn) {
    bool ok = false;
    if (GT::RunAndWait([&](std::atomic<int>& done) { ok = fn(); done.store(1); }) != 1) return false;
    return ok;
}

void Invalid(const char* why, const char* tag) {
    Run([&] {
        if (Alive()) detail::ClientInvalid(why, tag);
        return true;
    });
}

bool AimedAt(void* mirror) {
    void* me = coop::players::Registry::Get().Local();
    return me && E::ReadMainPlayerHitActor(me) == mirror && E::ReadMainPlayerLookAtActor(me) == mirror &&
           E::MainPlayerHasSelectedAction(me);
}

bool AimFan(void* mirror, int32_t mirrorIdx) {
    static constexpr float kYaw[] = {0.f, -5.f, 5.f, -10.f, 10.f, -16.f, 16.f};
    static constexpr float kPitch[] = {0.f, 8.f, -8.f, 16.f, -16.f};
    for (float dp : kPitch) {
        for (float dy : kYaw) {
            if (!Run([=] {
                    void* p = coop::players::Registry::Get().Local();
                    void* c = p ? E::GetController(p) : nullptr;
                    ue_wrap::FVector at{};
                    if (!c || !R::IsLiveByIndex(mirror, mirrorIdx) || !E::TryGetActorLocation(mirror, at)) return false;
                    ue_wrap::FRotator r = coop::director::LookAt(E::GetCameraLocation(), at);
                    r.Pitch += dp;
                    r.Yaw += dy;
                    E::SetControlRotation(c, r);
                    return true;
                }))
                return false;
            if (WaitGame([=] { return AimedAt(mirror); }, 300)) return true;
            if (!Alive()) return false;
        }
    }
    return false;
}

const char* WalkUpTo(void* mirror, float reachCm, int deadlineS) {
    std::shared_ptr<coop::director::BackgroundWalk> walk;
    Run([&] {
        ue_wrap::FVector me{}, at{};
        void* p = coop::players::Registry::Get().Local();
        if (!p || !E::TryGetActorLocation(p, me) || !E::TryGetActorLocation(mirror, at)) return false;
        const float dx = me.X - at.X, dy = me.Y - at.Y;
        if (std::sqrt(dx * dx + dy * dy) > reachCm) walk = coop::director::StartBackgroundWalk(at, reachCm, deadlineS);
        return true;
    });
    if (walk && !WaitGame([&] { return walk->state.load() != 0; }, (deadlineS + 5) * 1000ull))
        return "the walk up to the Omega did not end";
    if (walk && walk->state.load() != 1) return "the walk up to the Omega did not arrive";
    return nullptr;
}

}  // namespace coop::dev::kerfur_serve_drill::steps
