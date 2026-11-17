// coop/dev/droneunhook_probe.cpp -- [dev] the host unhooks the drone's sack (VOTVCOOP_DRONEUNHOOK_PROBE=1 on both
// peers), for GitHub issue "The client's drone keeps showing its sack after the host unhooks it".
//
// THE HOST, 24 s after a client is connected: its drone (drone_C) given a sack as a delivery has it (hasSack set, the
// sack mesh shown); at 34 s the drone's own dropSack, the unhook. EVERY PEER, every 2 s to 50 s: the drone's hasSack
// and whether its sack mesh is visible, and the loose sacks in the world (`[DRONEUNHOOK]`). VOTVCOOP_DRONEUNHOOK_REHOOK=host
// or client: at 44 s that peer puts the sack back, its copy of the loose sack set into the drone's sack box (`sackPhys`),
// whose overlap runs the drone's own putSackOn; the log runs to 70 s. VOTVCOOP_DRONEUNHOOK_REHOOK=hold: the host first
// brings its drone down 4 m before its player (20 s; with =client too), and at 40 s the client takes the dropped sack into its hand through
// its own `Hold Object` and steps until the held sack is in the drone's sack box, as a player carrying it there would.
// VOTVCOOP_DRONEUNHOOK_BY=client: the client takes the sack off at 34 s through its drone's own menu (actionOptionIndex 7)
// instead of the host. VOTVCOOP_DRONEUNHOOK_REHOOK=floor: the client grabs the loose sack with its use key at 40 s (the
// director's aimed grab), lets it fall to the floor at 46 s, grabs it again at 52 s and carries it into the sack box.

#include "coop/dev/droneunhook_probe.h"

#include "coop/dev/director/aimed_grab.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/devices/drone.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/engine_component.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <cstdint>
#include <string>

namespace coop::dev::droneunhook_probe {
namespace {

namespace R = ue_wrap::reflection;

uint64_t Now() { return ::GetTickCount64(); }

bool On() {
    static const bool v = coop::config::ReadEnv("VOTVCOOP_DRONEUNHOOK_PROBE") == "1";
    return v;
}

void* Live(const wchar_t* cls, int* count = nullptr) {
    void* first = nullptr;
    int n = 0;
    for (void* o : R::FindObjectsByClass(cls))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) {
            if (!first) first = o;
            ++n;
        }
    if (count) *count = n;
    return first;
}

int Bool(void* o, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!o || !R::FindBoolProperty(R::ClassOf(o), name, off, mask)) return -1;
    return (static_cast<const uint8_t*>(o)[off] & mask) ? 1 : 0;
}

int SackVisible(void* drone) {
    const int32_t off = drone ? R::FindPropertyOffset(R::ClassOf(drone), L"sack") : -1;
    void* sack = off >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(drone) + off) : nullptr;
    void* sc = R::FindClass(L"SceneComponent");
    void* fn = sc ? R::FindFunction(sc, L"IsVisible") : nullptr;
    if (!sack || !fn) return -1;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(sack, f) ? (f.Get<bool>(L"ReturnValue") ? 1 : 0) : -1;
}

// Who puts the sack back: 'h', 'c', 'o' (the client carries it there), 'f' (the client carries it, drops it on the
// floor, takes it again and carries it there) or none.
char Rehook() {
    static const std::string v = coop::config::ReadEnv("VOTVCOOP_DRONEUNHOOK_REHOOK");
    return v == "host" ? 'h' : v == "client" ? 'c' : v == "hold" ? 'o' : v == "floor" ? 'f' : 0;
}

// Who unhooks: the host's drone's dropSack (the default), or the client through its drone's action menu.
bool ClientUnhooks() {
    static const bool v = coop::config::ReadEnv("VOTVCOOP_DRONEUNHOOK_BY") == "client";
    return v;
}

bool HoldSack(void* me, void* sack) {
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(me), L"Hold Object"));
    return f.valid() && f.Set<bool>(L"useHold", true) && f.Set<void*>(L"manual", sack) && ue_wrap::Call(me, f);
}

void* SackBox(void* drone) {
    const int32_t off = drone ? R::FindPropertyOffset(R::ClassOf(drone), L"sackPhys") : -1;
    return off >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(drone) + off) : nullptr;
}

uint64_t g_startAt = 0, g_nextLog = 0;
int g_step = 0;
bool g_rehooked = false, g_droneMoved = false, g_held = false;
int g_floorStep = 0;   // the floor arm: 1 grabbing, 2 held, 3 let go, 4 grabbing again, 5 carrying, 9 failed
std::unique_ptr<coop::director::AimedGrab> g_grab;

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    return std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z));
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[DRONEUNHOOK] probe armed role=%s", host ? "host" : "client");
    }
    const uint64_t t = Now() - g_startAt;
    void* const drone = Live(L"drone_C");
    if (host && drone && g_step == 0 && t >= 24000u) {
        g_step = 1;
        int32_t off = -1;
        uint8_t mask = 0;
        if (R::FindBoolProperty(R::ClassOf(drone), L"hasSack", off, mask)) static_cast<uint8_t*>(drone)[off] |= mask;
        // Parked as an arrival leaves it, so a player's menu on any copy takes the sack off.
        if (R::FindBoolProperty(R::ClassOf(drone), L"canTakeOff", off, mask)) static_cast<uint8_t*>(drone)[off] |= mask;
        UE_LOGI("[DRONEUNHOOK] host: the drone given a sack (hasSack %d, mesh shown %d)", Bool(drone, L"hasSack"),
                ue_wrap::drone::SetSackVisible(drone, true) ? 1 : 0);
    }
    if (!host && ClientUnhooks() && drone && g_step == 0 && t >= 34000u) {
        g_step = 2;
        // The menu's choice: the drone's actionOptionIndex with action 7, its "take the sack off".
        ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(drone), L"actionOptionIndex"));
        const bool ran = f.valid() && f.Set<uint8_t>(L"action", 7) && ue_wrap::Call(drone, f);
        UE_LOGI("[DRONEUNHOOK] client: the drone's menu, take the sack off (ran=%d, canTakeOff %d, hasSack %d)",
                ran ? 1 : 0, Bool(drone, L"canTakeOff"), Bool(drone, L"hasSack"));
    }
    if (host && !ClientUnhooks() && drone && g_step == 1 && t >= 34000u) {
        g_step = 2;
        ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(drone), L"dropSack"));
        UE_LOGI("[DRONEUNHOOK] host: the drone's dropSack (ran=%d)", f.valid() && ue_wrap::Call(drone, f) ? 1 : 0);
    }
    void* const me = coop::players::Registry::Get().Local();
    if ((Rehook() == 'o' || Rehook() == 'c' || Rehook() == 'f' || ClientUnhooks()) && host && drone && me && !g_droneMoved && t >= 20000u) {
        g_droneMoved = true;
        ue_wrap::FVector at{};
        const ue_wrap::FVector fwd = ue_wrap::engine::GetActorForwardVector(me);
        const bool ok = ue_wrap::engine::TryGetActorLocation(me, at) &&
                        ue_wrap::engine::SetActorLocation(drone, {at.X + fwd.X * 400.f, at.Y + fwd.Y * 400.f, at.Z + 150.f});
        UE_LOGI("[DRONEUNHOOK] host: the drone brought down before the player (ok=%d)", ok ? 1 : 0);
    }
    // The floor arm, as a player does it with its use key (the director's aimed grab): the sack grabbed at 40 s,
    // let go at 46 s so it falls to the floor, grabbed again at 52 s and carried into the drone's sack box.
    if (Rehook() == 'f' && !host && drone && me && t >= 40000u && t < 70000u) {
        void* const sack = Live(L"prop_dronesack_C");
        void* const box = SackBox(drone);
        auto startGrab = [&](const char* what) {
            ue_wrap::FVector s{}, p{};
            if (!sack || !ue_wrap::engine::TryGetActorLocation(sack, s) || !ue_wrap::engine::TryGetActorLocation(me, p))
                return;
            const float dx = p.X - s.X, dy = p.Y - s.Y, d = std::sqrt(dx * dx + dy * dy);
            if (d > 150.f)  // within the use key's reach first, 120 cm off the sack on the side the player stands
                ue_wrap::engine::SetActorLocation(me, {s.X + dx / d * 120.f, s.Y + dy / d * 120.f, p.Z});
            g_grab = std::make_unique<coop::director::AimedGrab>(me, sack);
            UE_LOGI("[DRONEUNHOOK] client: grabs the sack (%s)", what);
        };
        if (g_floorStep == 0 && sack) { g_floorStep = 1; startGrab("first"); }
        if ((g_floorStep == 1 || g_floorStep == 4) && g_grab) {
            const auto st = g_grab->Tick();
            if (st != coop::director::GrabState::Working) {
                UE_LOGI("[DRONEUNHOOK] client: the grab %s%s%s", st == coop::director::GrabState::Grabbed ? "held it" : "failed: ",
                        st == coop::director::GrabState::Grabbed ? "" : g_grab->Why(), "");
                g_floorStep = st == coop::director::GrabState::Grabbed ? g_floorStep + 1 : 9;
                g_grab.reset();
            }
        }
        if (g_floorStep == 2 && t >= 46000u) {
            g_floorStep = 3;
            UE_LOGI("[DRONEUNHOOK] client: let the sack go onto the floor (released=%d)",
                    sack && ue_wrap::engine::ReleaseMainPlayerGrabIfHolding(me, sack) ? 1 : 0);
        }
        if (g_floorStep == 3 && t >= 52000u && sack) { g_floorStep = 4; startGrab("again, off the floor"); }
        if (g_floorStep == 5 && sack && box) {
            // Carried as a player brings it: the player stands 150 cm off the box on the floor, a step at a time, and
            // looks at the box, so the grab holds the sack up into it.
            ue_wrap::FVector b{}, p{};
            if (ue_wrap::engine::TryGetComponentLocation(box, b) && ue_wrap::engine::TryGetActorLocation(me, p)) {
                const float dx = p.X - b.X, dy = p.Y - b.Y, d = (std::max)(1.f, std::sqrt(dx * dx + dy * dy));
                const float step = (std::min)(5.f, (std::max)(-5.f, d - 150.f));
                ue_wrap::engine::SetActorLocation(me, {p.X - dx / d * step, p.Y - dy / d * step, p.Z});
                const ue_wrap::FVector cam = ue_wrap::engine::GetCameraLocation();
                const float vx = b.X - cam.X, vy = b.Y - cam.Y, vz = b.Z - cam.Z;
                const float yaw = std::atan2(vy, vx) * 57.29578f;
                const float pitch = std::atan2(vz, std::sqrt(vx * vx + vy * vy)) * 57.29578f;
                if (void* pc = ue_wrap::engine::GetController(me))
                    ue_wrap::engine::SetControlRotation(pc, ue_wrap::FRotator{pitch, yaw, 0.f});
            }
        }
    }
    if (Rehook() == 'o' && !host && drone && me && t >= 40000u && t < 66000u) {
        void* const sack = Live(L"prop_dronesack_C");
        void* const box = SackBox(drone);
        if (!g_held && sack) {
            g_held = true;
            UE_LOGI("[DRONEUNHOOK] client: took the loose sack into its hand (ran=%d)", HoldSack(me, sack) ? 1 : 0);
        } else if (g_held && sack && box) {
            // Step the player by the held sack's miss, a little each frame, so the hand carries it into the box.
            ue_wrap::FVector s{}, b{}, p{};
            if (ue_wrap::engine::TryGetActorLocation(sack, s) && ue_wrap::engine::TryGetComponentLocation(box, b) &&
                ue_wrap::engine::TryGetActorLocation(me, p) && Dist(s, b) > 10.f)
                ue_wrap::engine::SetActorLocation(me, {p.X + (b.X - s.X) * 0.2f, p.Y + (b.Y - s.Y) * 0.2f,
                                                       p.Z + (b.Z - s.Z) * 0.2f});
        }
    }
    if (drone && !g_rehooked && t >= 44000u && Rehook() == (host ? 'h' : 'c')) {
        g_rehooked = true;
        void* const sack = Live(L"prop_dronesack_C");
        void* const box = SackBox(drone);
        ue_wrap::FVector at{};
        const bool moved = sack && box && ue_wrap::engine::TryGetComponentLocation(box, at) &&
                           ue_wrap::engine::SetActorLocation(sack, at);
        UE_LOGI("[DRONEUNHOOK] %s: the loose sack set into the drone's sack box (moved=%d, sackInteract %d)",
                host ? "host" : "client", moved ? 1 : 0, Bool(drone, L"sackInteract"));
    }
    if (t >= 20000u && t < (Rehook() ? 76000u : 50000u) && Now() >= g_nextLog) {
        g_nextLog = Now() + 2000;
        int sacks = 0;
        Live(L"prop_dronesack_C", &sacks);
        void* const sack = Live(L"prop_dronesack_C");
        ue_wrap::FVector s{}, b{};
        const float miss = sack && SackBox(drone) && ue_wrap::engine::TryGetActorLocation(sack, s) &&
                                   ue_wrap::engine::TryGetComponentLocation(SackBox(drone), b)
                               ? Dist(s, b)
                               : -1.f;
        UE_LOGI("[DRONEUNHOOK] %s t=%llu: drone %d, hasSack %d, sack mesh visible %d, loose sacks %d, sack to box %.0f",
                host ? "host" : "client", t / 1000ull, drone ? 1 : 0, Bool(drone, L"hasSack"), SackVisible(drone),
                sacks, miss);
        for (void* o : R::FindObjectsByClass(L"prop_dronesack_C")) {
            ue_wrap::FVector at{};
            if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__") &&
                ue_wrap::engine::TryGetActorLocation(o, at))
                UE_LOGI("[DRONEUNHOOK] %s t=%llu:   a loose sack at (%.0f,%.0f,%.0f)", host ? "host" : "client",
                        t / 1000ull, at.X, at.Y, at.Z);
        }
    }
}

}  // namespace coop::dev::droneunhook_probe
