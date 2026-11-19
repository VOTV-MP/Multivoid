// coop/dev/kwheel_probe.cpp -- [dev] a following Kerfus fast and through obstacles (VOTVCOOP_KWHEEL_PROBE=1 on every
// peer; VOTVCOOP_KWHEEL_DIR, a folder all peers share, for the client's turn). Ghezra/Multivoid issue 20.
// The Kerfus (p_kerfus_C) rolls on a wheel that is its own actor (prop_kerfurWheel_C, the `wheel` child actor), held to
// the body's `wheelAttach` by `physicsConstraint`; its tick torques the wheel at `wheelSpeed`. Times run from the
// host's start, once two clients are in. THE HOST: at 0 s finds a Kerfus, or spawns one by the ATV, at 6 s turns
// it on (it follows the host); A 10-40 s, the host steps 4 m every 5 s; B 40-75 s, wheelSpeed x3 (VOTVCOOP_KWHEEL_FAST),
// the host steps 6 m back every 4 s; C 75 s, a metal table and four boxes on the line ahead, the host goes 10 m past
// them, and back at 105 s; D 130 s, client slot 1 turns it off and on (it follows that client) and steps 5 m every 4 s
// through the obstacles and back; DONE at 230 s. EVERY PEER, 5 times a second: the body's place, the wheel's, the
// wheel's gap from its mount and each one's physics; once a second each obstacle by element id. Tick counts are one
// clock across the rig's peers, so the lines compare across logs by `ms`.

#include "coop/dev/kwheel_probe.h"

#include "coop/config/config.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/actors/kerfus.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_physics.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace coop::dev::kwheel_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace K  = ue_wrap::kerfus;
namespace OI = ue_wrap::object_index;
using ue_wrap::FVector;

bool On() {
    static const bool on = coop::config::ReadEnv("VOTVCOOP_KWHEEL_PROBE") == "1";
    return on;
}
std::string Dir() { return coop::config::ReadEnv("VOTVCOOP_KWHEEL_DIR"); }
float Fast() {
    const std::string v = coop::config::ReadEnv("VOTVCOOP_KWHEEL_FAST");
    return v.empty() ? 3.f : static_cast<float>(std::atof(v.c_str()));
}
uint64_t Now() { return ::GetTickCount64(); }
void* Me() { return coop::players::Registry::Get().Local(); }
unsigned Eid(void* a) { return a ? static_cast<unsigned>(coop::element::Registry::Get().EidForActor(a)) : 0u; }

template <class T>
T Field(void* obj, const wchar_t* name, T dflt) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? *reinterpret_cast<const T*>(static_cast<const char*>(obj) + off) : dflt;
}
template <class T>
bool SetField(void* obj, const wchar_t* name, T v) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    *reinterpret_cast<T*>(static_cast<char*>(obj) + off) = v;
    return true;
}

float Dist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The local player to `at` through its own teleportWObackrooms.
void PlaceMe(const FVector& at) {
    void* const me = Me();
    void* const f = me ? R::FindFunction(R::ClassOf(me), L"teleportWObackrooms") : nullptr;
    ue_wrap::ParamFrame pf(f);
    ue_wrap::FTransform t{};
    t.TX = at.X; t.TY = at.Y; t.TZ = at.Z;
    if (f && pf.valid() && pf.Set<ue_wrap::FTransform>(L"newTransform", t)) ue_wrap::Call(me, pf);
}

void* ClassOr(const wchar_t* name, const wchar_t* path) {
    void* c = OI::ClassByName(name);
    return c ? c : ue_wrap::asset_load::LoadObjectByPath(path);
}

void Write(const char* name) {
    if (!Dir().empty()) std::ofstream(Dir() + "\\" + name, std::ios::binary | std::ios::trunc) << "x";
}
bool Seen(const char* name) { return !Dir().empty() && std::ifstream(Dir() + "\\" + name).good(); }

void* g_k = nullptr;
FVector g_origin{};          // the Kerfus's place at its turn-on: the obstacles' line runs +Y from it
uint64_t g_start = 0, g_logAt = 0, g_obsAt = 0;
int g_step = 0, g_cstep = 0;
bool g_spawned = false;
uint64_t g_settleAt = 0, g_phase0 = 0;
float g_baseSpeed = -1.f;
FVector g_lastBody{};
uint64_t g_lastMs = 0;
bool g_host = false;
FVector g_dir{};             // the body's heading over the last log step, horizontal unit
float g_speed = 0.f;
int g_drops = 0;
uint64_t g_dropAt = 0;

// The nearest player's body to `at`, the host's own and every puppet's (cm).
float NearestPlayer(const FVector& at) {
    float best = 1.0e9f;
    FVector p{};
    if (E::TryGetActorLocation(Me(), p)) best = Dist(p, at);
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
        void* const a = rp && rp->valid() ? rp->actor() : nullptr;
        if (a && E::TryGetActorLocation(a, p) && Dist(p, at) < best) best = Dist(p, at);
    }
    return best;
}

// HOST: while the Kerfus drives fast (B, and D following the client), a box 2.5 m ahead of it on its heading every
// 3 s, so it hits one its path did not plan around; 12 at most. F: the same while it is over 13 m from every player.
void DropAhead(uint64_t T) {
    const bool isFar = T >= 232000 && T < 290000;
    const bool window = (T >= 45000 && T < 75000) || (T >= 140000 && T < 225000) || isFar;
    if (!window || g_drops >= (isFar ? 20 : 12) || Now() < g_dropAt || !g_k || g_speed < 150.f) return;
    void* const box = ClassOr(L"prop_box_C", L"/Game/objects/prop_box.prop_box_C");
    FVector k{};
    if (!box || !E::TryGetActorLocation(g_k, k)) return;
    const float nearCm = NearestPlayer(k);
    if (isFar && nearCm < 1500.f) return;
    const FVector at{k.X + g_dir.X * 250.f, k.Y + g_dir.Y * 250.f, k.Z + 10.f};
    void* const b = E::SpawnActor(box, at);
    g_dropAt = Now() + 3000;
    ++g_drops;
    UE_LOGI("[KWHEEL] host: DROP %d a box %p ahead of the Kerfus at %.0f cm/s, at (%.0f,%.0f,%.0f), nearest player %.0f cm",
            g_drops, b, g_speed, at.X, at.Y, at.Z, nearCm);
}

void LogKerfus() {
    if (!g_k || !R::IsLive(g_k)) g_k = K::FindLive();
    void* const k = g_k;
    if (!k) return;
    FVector body{}, wheel{}, mount{};
    E::TryGetActorLocation(k, body);
    void* const bodyMesh = Field<void*>(k, L"StaticMesh", nullptr);
    void* const wheelObj = Field<void*>(k, L"wheelObject", nullptr);
    void* const wheelComp = Field<void*>(k, L"wheelComponent", nullptr);
    void* const attach = Field<void*>(k, L"wheelAttach", nullptr);
    const bool hasWheel = wheelComp && R::IsLive(wheelComp) && E::TryGetComponentLocation(wheelComp, wheel);
    const bool hasMount = attach && E::TryGetComponentLocation(attach, mount);
    bool on = false;
    K::ReadActive(k, on);
    const uint64_t ms = Now();
    float speed = 0.f;
    if (g_lastMs && ms > g_lastMs) {
        speed = Dist(body, g_lastBody) * 1000.f / static_cast<float>(ms - g_lastMs);
        g_dir = speed > 1.f ? FVector{(body.X - g_lastBody.X) / (speed * (ms - g_lastMs) / 1000.f),
                                      (body.Y - g_lastBody.Y) / (speed * (ms - g_lastMs) / 1000.f), 0.f}
                            : FVector{};
        g_speed = speed;
    }
    g_lastBody = body;
    g_lastMs = ms;
    UE_LOGI("[KWHEEL] ms=%llu eid=%u on=%d spd=%.0f ws=%.1f body=(%.0f,%.0f,%.0f) bsim=%d wheel=(%.0f,%.0f,%.0f) "
            "weid=%u wsim=%d gap=%.1f np=%.0f",
            static_cast<unsigned long long>(ms), Eid(k), on ? 1 : 0, speed, Field<float>(k, L"wheelSpeed", -1.f),
            body.X, body.Y, body.Z, bodyMesh && E::IsComponentSimulatingPhysics(bodyMesh) ? 1 : 0, wheel.X, wheel.Y,
            wheel.Z, Eid(wheelObj), hasWheel && E::IsComponentSimulatingPhysics(wheelComp) ? 1 : 0,
            hasWheel && hasMount ? Dist(wheel, mount) : -1.f, g_host ? NearestPlayer(body) : -1.f);
}

void LogObstacles() {
    if (!g_k) return;
    FVector kat{};
    E::TryGetActorLocation(g_k, kat);
    for (const wchar_t* cls : {L"prop_metalTable_C", L"prop_box_C"}) {
        void* const c = OI::ClassByName(cls);
        if (!c) continue;
        struct Ctx { FVector at; const wchar_t* cls; } ctx{kat, cls};
        OI::ForEachInstance(c, [](void* p, void* obj, int32_t) {
            auto* x = static_cast<Ctx*>(p);
            if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            FVector at{};
            if (!E::TryGetActorLocation(obj, at) || Dist(at, x->at) > 2500.f) return;
            UE_LOGI("[KWHEEL-OBS] ms=%llu %ls eid=%u at=(%.1f,%.1f,%.1f) sim=%d", static_cast<unsigned long long>(Now()),
                    x->cls, Eid(obj), at.X, at.Y, at.Z, E::IsActorRootSimulatingPhysics(obj) ? 1 : 0);
        }, &ctx);
    }
}

void HostSteps(uint64_t t) {
    const uint64_t T = g_phase0 ? t - g_phase0 + 6000 : 0;   // the phases' clock: the turn-on at 6 s
    if (g_phase0) DropAhead(T);
    if (g_step == 0) {
        g_step = 1;
        g_k = K::FindLive();
        void* atv = nullptr;
        if (ue_wrap::atv::EnsureResolved())
            ue_wrap::atv::ForEachAtv([](void* ctx, void* a, int32_t) {
                if (!*static_cast<void**>(ctx)) *static_cast<void**>(ctx) = a;
            }, &atv);
        FVector yard{};
        if (g_k) E::TryGetActorLocation(g_k, yard);   // the save's own, where it stands: its wheel is its own actor
        else if (atv && E::TryGetActorLocation(atv, yard)) yard = {yard.X + 600.f, yard.Y + 300.f, yard.Z + 100.f};
        else E::TryGetActorLocation(Me(), yard);
        PlaceMe({yard.X + 250.f, yard.Y, yard.Z + 60.f});
        UE_LOGI("[KWHEEL] host: to the yard (%.0f,%.0f,%.0f), a Kerfus of the save's: %d", yard.X, yard.Y, yard.Z,
                g_k ? 1 : 0);
    } else if (g_step == 1 && t >= 3000 && (!g_k || g_spawned)) {
        // A Kerfus 2 m beside the host at its feet, upright; again, up to three times, while it lands on its back.
        static int tries = 0;
        FVector body{}, wheel{};
        void* const wc = g_k ? Field<void*>(g_k, L"wheelComponent", nullptr) : nullptr;
        const bool upright = g_k && E::TryGetActorLocation(g_k, body) && wc && E::TryGetComponentLocation(wc, wheel) &&
                             wheel.Z < body.Z - 5.f;
        if (g_k && (upright || tries >= 3)) {
            g_step = 2;
            UE_LOGI("[KWHEEL] host: the Kerfus eid %u stands (upright=%d, wheel %.0f cm under the body)", Eid(g_k),
                    upright ? 1 : 0, body.Z - wheel.Z);
            g_settleAt = t + 3000;
            return;
        }
        if (t < g_settleAt) return;
        if (g_k) E::DestroyActor(g_k);
        ++tries;
        FVector me{};
        E::TryGetActorLocation(Me(), me);
        void* const cls = ClassOr(L"p_kerfus_C", L"/Game/objects/p_kerfus.p_kerfus_C");
        g_k = cls ? E::SpawnActor(cls, {me.X - 200.f, me.Y, me.Z - 60.f}) : nullptr;
        g_spawned = true;
        g_settleAt = t + 4000;
        UE_LOGI("[KWHEEL] host: spawned a Kerfus (try %d) at (%.0f,%.0f,%.0f)", tries, me.X - 200.f, me.Y, me.Z - 60.f);
    } else if (g_step == 1 && t >= 3000 && g_k && !g_spawned) {
        g_step = 2;
        g_settleAt = t;
    } else if (g_step == 2 && t >= g_settleAt && g_k) {
        g_step = 3;
        g_phase0 = t;
        K::WriteEnergy(g_k, 100.f);
        bool on = false;
        K::ReadActive(g_k, on);
        if (!on) K::RunActionOptionIndex(g_k, Me(), 8);
        K::ReadActive(g_k, on);
        E::TryGetActorLocation(g_k, g_origin);
        g_baseSpeed = Field<float>(g_k, L"wheelSpeed", -1.f);
        UE_LOGI("[KWHEEL] host: turned it on (on=%d) wheelSpeed=%.2f; PHASE A normal speed T0 ms=%llu", on ? 1 : 0,
                g_baseSpeed, static_cast<unsigned long long>(Now() - 6000));
    } else if (g_step >= 3 && g_step < 9 && T >= 10000 + (g_step - 3) * 5000ull) {   // A: 6 steps of 4 m
        ++g_step;
        FVector me{};
        E::TryGetActorLocation(Me(), me);
        PlaceMe({me.X + 400.f, me.Y, me.Z + 30.f});
    } else if (g_step == 9 && T >= 40000) {
        g_step = 10;
        SetField<float>(g_k, L"wheelSpeed", g_baseSpeed * Fast());
        UE_LOGI("[KWHEEL] host: PHASE B wheelSpeed %.2f", Field<float>(g_k, L"wheelSpeed", -1.f));
    } else if (g_step >= 10 && g_step < 18 && T >= 41000 + (g_step - 10) * 4000ull) {   // B: 8 steps, 36 m back
        ++g_step;
        FVector me{};
        E::TryGetActorLocation(Me(), me);
        PlaceMe({me.X - 300.f * (g_step % 2 ? 1.f : 2.f), me.Y, me.Z + 30.f});
    } else if (g_step == 18 && T >= 75000) {
        g_step = 19;
        FVector k{};
        E::TryGetActorLocation(g_k, k);
        g_origin = k;
        void* const table = ClassOr(L"prop_metalTable_C", L"/Game/objects/prop_metalTable.prop_metalTable_C");
        void* const box = ClassOr(L"prop_box_C", L"/Game/objects/prop_box.prop_box_C");
        int made = 0;
        if (table && E::SpawnActor(table, {k.X, k.Y + 350.f, k.Z + 10.f})) ++made;
        const float bx[4] = {-40.f, 40.f, -20.f, 30.f}, by[4] = {500.f, 520.f, 650.f, 700.f};
        for (int i = 0; i < 4; ++i)
            if (box && E::SpawnActor(box, {k.X + bx[i], k.Y + by[i], k.Z + 15.f})) ++made;
        PlaceMe({k.X, k.Y + 1000.f, k.Z + 60.f});
        UE_LOGI("[KWHEEL] host: PHASE C %d obstacle(s) on the line from (%.0f,%.0f,%.0f); the host 10 m past them", made,
                k.X, k.Y, k.Z);
    } else if (g_step == 19 && T >= 105000) {
        g_step = 20;
        PlaceMe({g_origin.X + 60.f, g_origin.Y - 300.f, g_origin.Z + 60.f});
        UE_LOGI("[KWHEEL] host: PHASE C back through them");
    } else if (g_step == 20 && T >= 130000) {
        g_step = 21;
        PlaceMe({g_origin.X + 600.f, g_origin.Y - 300.f, g_origin.Z + 60.f});
        Write("kw_client");
        UE_LOGI("[KWHEEL] host: PHASE D client slot 1's turn");
    } else if (g_step == 21 && T >= 230000) {
        g_step = 22;
        Write("kw_far");
        K::RunActionOptionIndex(g_k, Me(), 8);   // off and on again by the host's press: it follows the host
        K::RunActionOptionIndex(g_k, Me(), 8);
        bool on = false;
        K::ReadActive(g_k, on);
        UE_LOGI("[KWHEEL] host: PHASE F the host's again (on=%d); client slot 1 goes 30 m off, the host runs off in 25 m "
                "jumps; boxes ahead while the Kerfus is far from everyone", on ? 1 : 0);
    } else if (g_step >= 22 && g_step < 30 && T >= 234000 + (g_step - 22) * 8000ull) {   // 25 m jumps
        FVector at{};
        if (g_step == 22) E::TryGetActorLocation(g_k, at), at.X += 2500.f;
        else E::TryGetActorLocation(Me(), at), at.X += 2500.f;
        PlaceMe({at.X, at.Y, at.Z + 60.f});
        ++g_step;
    } else if (g_step == 30 && T >= 300000) {
        g_step = 31;
        UE_LOGI("[KWHEEL] DONE");
    }
}

// Client slot 1, from the host's D: beside the Kerfus, off and on (asked of the host), then 5 m steps through the
// obstacles' line and back.
void ClientSteps() {
    static uint64_t at = 0;
    static bool isFar = false;
    if (!isFar && g_cstep > 0 && Seen("kw_far") && g_k) {
        isFar = true;
        FVector k{};
        E::TryGetActorLocation(g_k, k);
        PlaceMe({k.X + 3000.f, k.Y, k.Z + 150.f});
        UE_LOGI("[KWHEEL] client: PHASE F 30 m off the Kerfus at (%.0f,%.0f,%.0f)", k.X, k.Y, k.Z);
        g_cstep = 100;
        return;
    }
    if (g_cstep == 0) {
        if (!Seen("kw_client") || !g_k) return;
        g_cstep = 1;
        FVector k{};
        E::TryGetActorLocation(g_k, k);
        g_origin = k;
        PlaceMe({k.X + 200.f, k.Y - 150.f, k.Z + 60.f});
        at = Now() + 3000;
        UE_LOGI("[KWHEEL] client: beside the Kerfus at (%.0f,%.0f,%.0f)", k.X, k.Y, k.Z);
    } else if ((g_cstep == 1 || g_cstep == 2) && Now() >= at) {
        K::RunActionOptionIndex(g_k, Me(), 8);
        UE_LOGI("[KWHEEL] client: pressed on/off (%d)", g_cstep);
        ++g_cstep;
        at = Now() + 4000;
    } else if (g_cstep >= 3 && g_cstep < 3 + 14 && Now() >= at) {
        FVector me{};
        E::TryGetActorLocation(Me(), me);
        const float dy = g_cstep < 3 + 7 ? 500.f : -500.f;
        PlaceMe({me.X, me.Y + dy, me.Z + 30.f});
        ++g_cstep;
        at = Now() + 4000;
    }
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    g_host = host;
    if (host && !g_start) {
        if (!session->IsSlotConnected(1) || !session->IsSlotConnected(2)) return;
        static uint64_t both = 0;
        if (!both) both = Now();
        if (Now() - both < 45000) return;   // the second client's world settles
        g_start = Now();
        UE_LOGI("[KWHEEL] host: START ms=%llu", static_cast<unsigned long long>(g_start));
    }
    if (host) HostSteps(Now() - g_start);
    else if (coop::players::Registry::Get().LocalPeerId() == 1) ClientSteps();
    if (Now() >= g_logAt) {
        g_logAt = Now() + 200;
        LogKerfus();
    }
    if (Now() >= g_obsAt) {
        g_obsAt = Now() + 1000;
        LogObstacles();
    }
}

}  // namespace coop::dev::kwheel_probe
