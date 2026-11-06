// harness/autotest/autotest_join_drop.cpp -- TEST-ONLY: a pinecone spawner's drop inside a join's sweep window.
//
// The client, once the sweep's claim tracking opens, spawns two sticks beside its player: one with the spawner's 600 s
// lifespan, as a real drop has, and one without, as a save divergence would be. After the sweep the drop must stand and
// the other must be gone, which also shows the sweep ran rather than stopping at its valve. Client only; gated by
// VOTVCOOP_RUN_JOIN_DROP.

#include "harness/autotest.h"

#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_echo_suppress.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>

namespace harness::autotest {
namespace {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace JS = coop::join_membership_sweep;

constexpr float kDropLifeS = 600.f;   // pineconeSpawner's setlifespan

int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Check(bool ok, const char* what) {
    UE_LOGI("join-drop: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

struct Stick {
    void* actor = nullptr;
    int32_t idx = -1;
};

// A stick 3 m up beside this player, given `life` seconds when above zero. The spawner calls the engine's spawn
// directly, past the ambient mirror's observer, so the drill's spawn hides from that observer too.
Stick SpawnStick(float side, float life) {
    Stick s;
    void* pawn = coop::players::Registry::Get().Local();
    void* cls = R::FindClass(L"prop_stick_C");
    ue_wrap::FVector me{};
    if (!pawn || !cls || !E::TryGetActorLocation(pawn, me)) return s;
    {
        coop::prop_echo_suppress::ScopedMirrorSpawn unseen;
        s.actor = E::SpawnActor(cls, {me.X + 200.f, me.Y + side, me.Z + 300.f});
    }
    if (!s.actor) return s;
    s.idx = R::InternalIndexOf(s.actor);
    if (life > 0.f) E::SetActorLifeSpan(s.actor, life);
    float left = 0.f;
    E::TryGetActorLifeSpan(s.actor, left);
    UE_LOGI("join-drop: stick %p key='%ls' life %.0f s", s.actor, ue_wrap::prop::GetInteractableKeyString(s.actor).c_str(),
            left);
    return s;
}

bool Live(const Stick& s) { return s.actor && R::IsLiveByIndex(s.actor, s.idx); }

}  // namespace

DWORD WINAPI JoinDropThread(LPVOID /*arg*/) {
    bool open = false;
    for (int i = 0; i < 3000 && !open; ++i) {   // 5 min: the save download and load come first
        OnGameThread([&] { open = JS::IsClaimTrackingActive(); });
        if (!open) ::Sleep(100);
    }
    Check(open, "the sweep's claim tracking opened");
    if (!open) return 0;

    Stick drop, other;
    OnGameThread([&] {
        drop = SpawnStick(-150.f, kDropLifeS);
        other = SpawnStick(150.f, 0.f);
    });
    Check(drop.actor && other.actor, "both sticks stand inside the window");
    if (!drop.actor || !other.actor) return 0;

    bool settled = false;
    for (int i = 0; i < 1200 && !settled; ++i) {
        OnGameThread([&] { settled = JS::HasLoadTailQuiesced(); });
        if (!settled) ::Sleep(100);
    }
    Check(settled, "the sweep fired");
    ::Sleep(2000);   // the destroys land within the sweep's drain; a frame or two for liveness
    bool dropLive = false, otherLive = true;
    OnGameThread([&] { dropLive = Live(drop); otherLive = Live(other); });
    Check(dropLive, "the drop with a lifespan was spared");
    Check(!otherLive, "the stick without one was swept, so the sweep ran past its valve");
    UE_LOGI("join-drop: DONE fails=%d", g_fails);
    return 0;
}

}  // namespace harness::autotest
