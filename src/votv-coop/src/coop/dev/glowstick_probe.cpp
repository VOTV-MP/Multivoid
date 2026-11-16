// coop/dev/glowstick_probe.cpp -- [dev] a client sticks a glowstick to the floor (VOTVCOOP_GLOWSTICK_PROBE=1 on both
// peers).
//
// A held glowstick (prop_glowstick_C) ticks a short ray from its tip; when the ray strikes, the player drops it and its
// stickTo hangs it from a hook at the struck point by a physics constraint, still simulating. THE HOST, 24 s after a
// client is connected: a glowstick at the client's feet. THE CLIENT: at 28 s steps back 140 uu from the nearest glowstick and grabs it as the
// use key does (the director's aimed grab), then holds it upside down, its tip stepped down
// to the floor until its own ray strikes it, then runs what its tick runs then (the player's
// dropGrabObject, its stickTo); at 40 s grabs
// it again (its grab frees it), and at 45 s lets go. EVERY PEER, every second to 60 s: the glowstick's
// key, place, stick and whether it simulates (`[GLOWSTICK]`).

#include "coop/dev/glowstick_probe.h"

#include "coop/config/config.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::glowstick_probe {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace D = coop::director;
using ue_wrap::FVector;

uint64_t Now() { return ::GetTickCount64(); }

bool On() {
    static const bool v = coop::config::ReadEnv("VOTVCOOP_GLOWSTICK_PROBE") == "1";
    return v;
}

int Bool(void* o, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!o || !R::FindBoolProperty(R::ClassOf(o), name, off, mask)) return -1;
    return (static_cast<const uint8_t*>(o)[off] & mask) ? 1 : 0;
}

// The live glowstick nearest `at`, or null.
void* Nearest(const FVector& at) {
    void* best = nullptr;
    float bestD = 1e18f;
    for (void* o : R::FindObjectsByClass(L"prop_glowstick_C")) {
        if (!o || !R::IsLive(o) || R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
        FVector p{};
        if (!E::TryGetActorLocation(o, p)) continue;
        const float d = (p.X - at.X) * (p.X - at.X) + (p.Y - at.Y) * (p.Y - at.Y) + (p.Z - at.Z) * (p.Z - at.Z);
        if (d < bestD) {
            bestD = d;
            best = o;
        }
    }
    return best;
}

uint64_t g_startAt = 0, g_nextLog = 0;
int g_step = 0;
void* g_stick = nullptr;
std::unique_ptr<D::AimedGrab> g_grab;
uint64_t g_grabStart = 0, g_turnAt = 0;
FVector g_spot{};
int g_turn = 0, g_saidTurn = -1;
bool g_saidStick = false, g_regrabDone = false;
std::unique_ptr<D::AimedGrab> g_regrab;

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[GLOWSTICK] probe armed role=%s", host ? "host" : "client");
    }
    const uint64_t t = Now() - g_startAt;
    void* const me = coop::players::Registry::Get().Local();
    if (!me) return;
    if (host && g_step == 0 && t >= 24000u) {
        coop::RemotePlayer& rp = coop::puppet_drive::Puppet(1);
        FVector at{};
        if (rp.valid() && E::TryGetActorLocation(rp.GetActor(), at)) {
            g_step = 1;
            void* cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/prop_glowstick.prop_glowstick_C");
            if (!cls) cls = R::FindClass(L"prop_glowstick_C");
            UE_LOGI("[GLOWSTICK] host: a glowstick at the client's feet (class %d, ok=%d)", cls ? 1 : 0,
                    cls && E::SpawnActor(cls, {at.X + 70.f, at.Y, at.Z - 40.f}) ? 1 : 0);
        }
    }
    if (!host) {
        FVector myAt{};
        E::TryGetActorLocation(me, myAt);
        if (g_step == 0 && t >= 28000u) {
            g_step = 1;
            g_stick = Nearest(myAt);
            UE_LOGI("[GLOWSTICK] client: the glowstick '%ls' to grab (found=%d)",
                    g_stick ? ue_wrap::prop::GetInteractableKeyString(g_stick).c_str() : L"", g_stick ? 1 : 0);
            if (g_stick) {
                // A step back from it, so it lies before the player rather than under the feet.
                FVector at{};
                E::TryGetActorLocation(g_stick, at);
                const float dx = myAt.X - at.X, dy = myAt.Y - at.Y;
                const float len = std::sqrt(dx * dx + dy * dy);
                if (len > 1.f)
                    E::SetActorLocation(me, {at.X + dx / len * 140.f, at.Y + dy / len * 140.f, myAt.Z});
                g_grab = std::make_unique<D::AimedGrab>(me, g_stick);
                g_grabStart = Now();
            } else {
                g_step = -1;
            }
        }
        if (g_step == 1 && g_grab) {
            const D::GrabState st = g_grab->Tick();
            if (st != D::GrabState::Working || Now() - g_grabStart > 15000u) {
                g_step = 2;
                UE_LOGI("[GLOWSTICK] client: the grab %hs%hs (aimed %d after %d poses, no aim %d, chain %d%d%d)",
                        st == D::GrabState::Grabbed ? "took it" : st == D::GrabState::Failed ? "failed: " : "timed out",
                        st == D::GrabState::Failed ? g_grab->Why() : "", g_grab->Aimed() ? 1 : 0, g_grab->AimPoses(),
                        g_grab->NoAim() ? 1 : 0, g_grab->ChainPre() ? 1 : 0, g_grab->ChainUse() ? 1 : 0,
                        g_grab->ChainPost() ? 1 : 0);
                if (st == D::GrabState::Grabbed) {
                    E::TryGetActorLocation(g_stick, g_spot);
                    g_turnAt = Now();
                }
            }
        }
        // Held: the glowstick a hand's breadth over the floor where it lay, upside down, its tip stepped down to the floor
        // a uu each 0.6 s, its own `ray` asked each tick; once the ray strikes, what its tick then does: the player's
        // dropGrabObject, then the glowstick's stickTo(the ray's hit, false).
        if (g_step == 2 && g_turn < 12 && D::Grabbing(me) == g_stick && Bool(g_stick, L"sticked") != 1) {
            static const ue_wrap::FRotator kTurns[6] = {{-90.f, 0.f, 0.f}, {90.f, 0.f, 0.f}, {0.f, 0.f, 90.f},
                                                         {0.f, 0.f, -90.f}, {0.f, 0.f, 0.f}, {0.f, 180.f, 0.f}};
            if (Now() - g_turnAt >= 600u) {
                g_turnAt = Now();
                ++g_turn;
            }
            if (g_turn < 12) {
                // Upside down, the tip (28 uu out along the glowstick's up axis) stepped down to the floor a turn at
                // a time: its ray reaches 5 uu.
                (void)kTurns;
                E::SetActorLocation(g_stick, {g_spot.X, g_spot.Y, g_spot.Z + 34.f - 1.f * static_cast<float>(g_turn)});
                E::SetActorRotation(g_stick, {0.f, 0.f, 180.f});
                if (g_turn != g_saidTurn) {
                    g_saidTurn = g_turn;
                    const int32_t so = R::FindPropertyOffset(R::ClassOf(g_stick), L"sticking");
                    const int32_t lo = R::FindPropertyOffset(R::ClassOf(g_stick), L"length");
                    void* comp = nullptr;
                    float len = -1.f;
                    if (so >= 0) std::memcpy(&comp, static_cast<const uint8_t*>(g_stick) + so, sizeof(comp));
                    if (lo >= 0) std::memcpy(&len, static_cast<const uint8_t*>(g_stick) + lo, sizeof(len));
                    FVector a{}, c{};
                    E::TryGetActorLocation(g_stick, a);
                    if (comp) E::TryGetComponentLocation(comp, c);
                    UE_LOGI("[GLOWSTICK] client: turn %d, the glowstick at (%.0f,%.0f,%.0f), its tip at (%.0f,%.0f,%.0f), "
                            "the ray %.1f uu", g_turn, a.X, a.Y, a.Z, c.X, c.Y, c.Z, len);
                }
                ue_wrap::ParamFrame ray(R::FindDispatchFunctionCached(R::ClassOf(g_stick), L"ray"));
                if (ray.valid() && ue_wrap::Call(g_stick, ray) && ray.Get<bool>(L"ReturnValue")) {
                    const int32_t size = R::FindParamSize(ray.function(), L"outHit");
                    std::vector<uint8_t> hit(static_cast<size_t>(size > 0 ? size : 0), 0u);
                    const bool got = size > 0 && ray.GetRaw(L"outHit", hit.data(), size);
                    const bool dropped = D::CallOnPlayer(me, L"dropGrabObject");
                    ue_wrap::ParamFrame to(R::FindDispatchFunctionCached(R::ClassOf(g_stick), L"stickTo"));
                    const bool ran = got && to.valid() && to.SetRaw(L"hit", hit.data(), size) &&
                                     to.Set(L"setFrame", false) && ue_wrap::Call(g_stick, to);
                    UE_LOGI("[GLOWSTICK] client: the ray struck at turn %d; dropped %d, stickTo ran %d", g_turn,
                            dropped ? 1 : 0, ran ? 1 : 0);
                    g_turn = 99;
                }
            }
        }
        if (g_step == 2 && !g_saidStick && Bool(g_stick, L"sticked") == 1) {
            g_saidStick = true;
            UE_LOGI("[GLOWSTICK] client: stuck at turn %d", g_turn);
        }
        // At 40 s, a stuck glowstick grabbed again: its grab prelude frees it.
        if (g_step == 2 && g_saidStick && t >= 40000u && !g_regrab) {
            g_regrab = std::make_unique<D::AimedGrab>(me, g_stick);
        }
        if (g_regrab && !g_regrabDone) {
            const D::GrabState st = g_regrab->Tick();
            if (st != D::GrabState::Working) {
                g_regrabDone = true;
                UE_LOGI("[GLOWSTICK] client: the stuck glowstick grabbed again (%hs)",
                        st == D::GrabState::Grabbed ? "took it" : g_regrab->Why());
            }
        }
        if (g_step == 2 && t >= 45000u) {
            g_step = 3;
            void* const held = D::Grabbing(me);
            UE_LOGI("[GLOWSTICK] client: at 45 s it holds %ls; let go (ran=%d)", held ? R::ClassNameOf(held).c_str() : L"none",
                    held ? (D::CallOnPlayer(me, L"dropGrabObject") ? 1 : 0) : 0);
        }
    }
    if (t >= 22000u && t < 60000u && Now() >= g_nextLog) {
        g_nextLog = Now() + 1000;
        FVector myAt{};
        E::TryGetActorLocation(me, myAt);
        void* const s = g_stick && R::IsLive(g_stick) ? g_stick : Nearest(myAt);
        FVector p{};
        if (s) E::TryGetActorLocation(s, p);
        UE_LOGI("[GLOWSTICK] %s t=%llu: '%ls' at (%.0f,%.0f,%.0f) sticked %d stick %d simulating %d held %d",
                host ? "host" : "client", t / 1000ull, s ? ue_wrap::prop::GetInteractableKeyString(s).c_str() : L"none",
                p.X, p.Y, p.Z, Bool(s, L"sticked"), Bool(s, L"stick"), s && E::IsActorRootSimulatingPhysics(s) ? 1 : 0,
                s && D::Grabbing(me) == s ? 1 : 0);
    }
}

}  // namespace coop::dev::glowstick_probe
