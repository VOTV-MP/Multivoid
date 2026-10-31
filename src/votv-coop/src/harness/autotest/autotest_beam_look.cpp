// harness/autotest/autotest_beam_look.cpp -- TEST-ONLY: how a remote player's flashlight looks on the host, and
// which of the two bodies' components cast the shadows its cone draws.
//
// Each peer first logs the shadow flags of every primitive and light component of its own player and of the
// other peer's puppet: the same class, so any difference is the puppet's own setup. Then the CLIENT switches its
// flashlight on, aims its view down onto the ground a few metres ahead and captures its own window; the HOST turns
// its own view onto the spot that beam lands on and captures its window every half second, while the client
// moves (run VOTVCOOP_RUN_MOVE_OSC beside it on the client). The captures land in coop-screenshots/ beside each
// game's executable. Gated by VOTVCOOP_RUN_BEAM_LOOK.

#include "harness/autotest.h"
#include "harness/screenshot.h"

#include "coop/dev/director/director.h"  // LookAt
#include "coop/player/item_activate.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <string>

namespace harness::autotest {
namespace {

namespace GT = ue_wrap::game_thread;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;

constexpr float kAimPitchDeg = -25.f;   // the beam onto the ground a few metres ahead
constexpr float kSpotCm      = 450.f;   // where along the aim the host looks
constexpr int   kShots       = 12;

template <class Fn>
void OnGameThread(Fn&& fn) {
    GT::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

// One component's flag by its reflected name: 1, 0, or -1 when the class has no such flag.
int Flag(void* obj, const wchar_t* name) {
    int32_t byte = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(obj), name, byte, mask) || byte < 0) return -1;
    return (static_cast<const uint8_t*>(obj)[byte] & mask) ? 1 : 0;
}

void DumpShadowFlags(const char* who, void* actor) {
    if (!actor) { UE_LOGW("beam_look: %s -- no actor", who); return; }
    for (const R::ObjectRef& c : R::ChildObjectsOf(actor)) {
        const int castShadow = Flag(c.object, L"CastShadow");     // a primitive
        const int castShadows = Flag(c.object, L"CastShadows");   // a light
        if (castShadow < 0 && castShadows < 0) continue;
        UE_LOGI("beam_look: %s %ls (%ls) visible=%d hiddenInGame=%d CastShadow=%d bCastDynamicShadow=%d "
                "bCastHiddenShadow=%d bOwnerNoSee=%d bOnlyOwnerSee=%d bSelfShadowOnly=%d | light CastShadows=%d "
                "CastDynamicShadows=%d", who, c.name.c_str(), c.className.c_str(), Flag(c.object, L"bVisible"),
                Flag(c.object, L"bHiddenInGame"), castShadow, Flag(c.object, L"bCastDynamicShadow"),
                Flag(c.object, L"bCastHiddenShadow"), Flag(c.object, L"bOwnerNoSee"), Flag(c.object, L"bOnlyOwnerSee"),
                Flag(c.object, L"bSelfShadowOnly"), castShadows, Flag(c.object, L"CastDynamicShadows"));
    }
}

void* PuppetActor(uint8_t slot) {
    coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(slot);
    return (p && p->valid()) ? p->GetActor() : nullptr;
}

void RunSourceLook() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(8000);
    void* local = nullptr;
    OnGameThread([&] {
        local = coop::players::Registry::Get().Local();
        DumpShadowFlags("client local", local);
        DumpShadowFlags("client's puppet of the host", PuppetActor(0));
    });
    if (!local) { UE_LOGW("beam_look: SOURCE no local player -- abort"); return; }
    const bool on = coop::item_activate::DebugForceToggle(local);
    // Hold the aim down onto the ground for the whole look, every tick, so a move oscillator's steps keep it.
    UE_LOGI("beam_look: SOURCE flashlight %s, aiming %.0f deg down", on ? "on" : "OFF", kAimPitchDeg);
    float yaw = 0.f;
    OnGameThread([&] { yaw = E::GetControlRotation(E::GetController(local)).Yaw; });
    const ULONGLONG end = ::GetTickCount64() + 30000;
    bool shot = false;
    while (::GetTickCount64() < end) {
        OnGameThread([&] { E::SetControlRotation(E::GetController(local), ue_wrap::FRotator{kAimPitchDeg, yaw, 0.f}); });
        if (!shot && ::GetTickCount64() + 25000 < end) { shot = harness::screenshot::Capture(L"beamlook-client"); }
        ::Sleep(16);
    }
    UE_LOGI("beam_look: SOURCE DONE (own view captured=%d)", shot ? 1 : 0);
}

void RunObserverLook() {
    void* puppet = nullptr;
    for (int i = 0; i < 480 && !puppet; ++i) {
        OnGameThread([&] { puppet = PuppetActor(1); });
        if (!puppet) ::Sleep(500);
    }
    if (!puppet) { UE_LOGW("beam_look: OBSERVER no puppet for slot 1 -- abort"); return; }
    ::Sleep(12000);   // the client's join settles and its flashlight comes on
    OnGameThread([&] {
        DumpShadowFlags("host local", coop::players::Registry::Get().Local());
        DumpShadowFlags("host's puppet of the client", PuppetActor(1));
    });
    int shots = 0;
    for (int i = 0; i < kShots; ++i) {
        // Each shot re-aims at the spot the client's beam points at now: the client may be moving.
        OnGameThread([&] {
            coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(1);
            void* actor = (p && p->valid()) ? p->GetActor() : nullptr;
            void* local = coop::players::Registry::Get().Local();
            if (!actor || !local) return;
            void* light = E::GetMainPlayerLightR(actor);
            ue_wrap::FVector from{};
            if (!light || !E::TryGetComponentLocation(light, from)) return;
            const ue_wrap::FRotator aim = E::GetComponentWorldRotation(light);
            const float cp = std::cos(aim.Pitch * 0.0174533f), sp = std::sin(aim.Pitch * 0.0174533f);
            const ue_wrap::FVector spot{from.X + std::cos(aim.Yaw * 0.0174533f) * cp * kSpotCm,
                                        from.Y + std::sin(aim.Yaw * 0.0174533f) * cp * kSpotCm, from.Z + sp * kSpotCm};
            E::SetControlRotation(E::GetController(local), coop::director::LookAt(E::GetCameraLocation(), spot));
        });
        ::Sleep(500);
        if (harness::screenshot::Capture((L"beamlook-host-" + std::to_wstring(i)).c_str())) ++shots;
    }
    UE_LOGI("beam_look: OBSERVER DONE (%d capture(s))", shots);
}

}  // namespace

DWORD WINAPI BeamLookThread(LPVOID /*arg*/) {
    if (IsClientRole()) RunSourceLook();
    else                RunObserverLook();
    return 0;
}

}  // namespace harness::autotest
