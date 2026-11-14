// coop/dev/cover_probe.cpp -- [dev] the covers on the rig (VOTVCOOP_COVER_PROBE=1).
//
// CLIENT, after it is connected: at 12 s it stands 120 cm out from the desk's top left plate; at 16 s it ends a timed
// unbolt of that plate through the desk's own unscrewPanel (lookingAtPanel 0, as the look writes it); at 21 s it
// presses E on it through actionOptionIndex action 4; at 28 s the plate prop in its hand touches its place through the
// desk's own ReceiveActorBeginOverlap; at 33 s it bolts the plate again. At 38 s it stands 150 cm from the first
// transformer station, and at 42 s and 50 s presses E on its door (lookAtDoor written, as the look writes it). HOST:
// at 56 s it unbolts the bottom right plate through unscrewPanel, and at 58 s opens the station's door through
// openDoor. BOTH, every second from 12 s to 65 s: the desk's plates, its take-off and set-back flags, the plate props in
// the world and the station's door (`[COVER]`).

#include "coop/dev/cover_probe.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/devices/generator.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace coop::dev::cover_probe {
namespace {

namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace OI  = ue_wrap::object_index;
namespace GEN = ue_wrap::generator;

uint64_t Now() { return ::GetTickCount64(); }

bool On() {
    static const bool on = coop::config::ReadEnv("VOTVCOOP_COVER_PROBE") == "1";
    return on;
}

void* First(const wchar_t* cls) {
    void* const c = OI::ClassByName(cls);
    void* found = nullptr;
    if (c)
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            void*& f = *static_cast<void**>(ctx);
            if (!f && obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) f = obj;
        }, &found);
    return found;
}

int Count(const wchar_t* cls) {
    void* const c = OI::ClassByName(cls);
    int n = 0;
    if (c)
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) ++*static_cast<int*>(ctx);
        }, &n);
    return n;
}

int Bool(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return -1;
    return (static_cast<const uint8_t*>(obj)[off] & mask) ? 1 : 0;
}

void SetBool(void* obj, const wchar_t* name, bool v) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return;
    uint8_t& b = static_cast<uint8_t*>(obj)[off];
    b = v ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
}

void SetInt(void* obj, const wchar_t* name, int32_t v) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off >= 0) *reinterpret_cast<int32_t*>(static_cast<char*>(obj) + off) = v;
}

const ue_wrap::field_io::TArrayView* Arr(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? reinterpret_cast<const ue_wrap::field_io::TArrayView*>(static_cast<const char*>(obj) + off) : nullptr;
}

void* Plate(void* desk, int i) {
    const auto* a = Arr(desk, L"panels");
    return a && a->data && i < a->num ? reinterpret_cast<void* const*>(a->data)[i] : nullptr;
}

void* Station0() {
    std::vector<void*> gens;
    GEN::ReadGenerators(gens);
    return gens.empty() ? nullptr : gens[0];
}

bool CallPlain(void* obj, const wchar_t* fn) {
    void* const f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    return f && R::CallFunction(obj, f, nullptr);
}

bool PressE(void* obj) {
    void* const me = coop::players::Registry::Get().Local();
    ue_wrap::ParamFrame f(obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), L"actionOptionIndex") : nullptr);
    return me && f.valid() && f.Set<void*>(L"player", me) && f.Set<uint8_t>(L"action", 4) && ue_wrap::Call(obj, f);
}

void* HeldPlate() {
    void* const me = coop::players::Registry::Get().Local();
    for (const wchar_t* field : {L"grabbing_actor", L"holding_actor", L"grabbedActor"}) {
        const int32_t off = me ? R::FindPropertyOffset(R::ClassOf(me), field) : -1;
        void* const h = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const char*>(me) + off) : nullptr;
        if (h && R::IsLive(h) && R::ClassNameOf(h) == L"prop_computerBackPanel_tl_C") return h;
    }
    return First(L"prop_computerBackPanel_tl_C");
}

void Report() {
    void* const desk = First(L"analogDScreenTest_C");
    const auto* on = Arr(desk, L"panelsState");
    const auto* loose = Arr(desk, L"panelsUnscrew");
    int bitsOn = 0, bitsLoose = 0;
    for (int i = 0; on && loose && on->data && loose->data && i < 4 && i < on->num && i < loose->num; ++i) {
        if (on->data[i]) bitsOn |= 1 << i;
        if (loose->data[i]) bitsLoose |= 1 << i;
    }
    void* const gen = Station0();
    UE_LOGI("[COVER] desk on=0x%x unbolted=0x%x takeoff=%d process=%d plates_tl=%d plates_br=%d | station0 opened=%d moving=%d",
            bitsOn, bitsLoose, Bool(desk, L"panelTakeoff"), Bool(desk, L"panelsProcess"),
            Count(L"prop_computerBackPanel_tl_C"), Count(L"prop_computerBackPanel_br_C"), Bool(gen, L"opened"),
            Bool(gen, L"moving"));
}

uint64_t g_startAt = 0, g_reportAt = 0;
int g_step = 0;

// A spot by `at` the client stays standing on: eight directions at `out` cm tried in turn, each kept when, 1.5 s on, the
// player still stands within 250 cm of it. Ticked until it settles.
struct Stand {
    ue_wrap::FVector at{};
    float out = 0.f;
    int tried = -1;
    uint64_t sinceMs = 0;
    bool settled = true;
};
Stand g_stand;

void BeginStand(const ue_wrap::FVector& at, float out) {
    g_stand = Stand{at, out, -1, 0, false};
}

void TickStand() {
    if (g_stand.settled) return;
    void* const me = coop::players::Registry::Get().Local();
    if (!me) return;
    if (g_stand.tried >= 0) {
        if (Now() - g_stand.sinceMs < 1500) return;
        ue_wrap::FVector p{};
        E::TryGetActorLocation(me, p);
        const float dx = p.X - g_stand.at.X, dy = p.Y - g_stand.at.Y, dz = p.Z - g_stand.at.Z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) < g_stand.out + 250.f) {
            g_stand.settled = true;
            UE_LOGI("[COVER] client: stands at (%.0f,%.0f,%.0f), try %d", p.X, p.Y, p.Z, g_stand.tried);
            return;
        }
    }
    if (++g_stand.tried >= 8) {
        g_stand.settled = true;
        UE_LOGW("[COVER] client: found no spot to stand by (%.0f,%.0f,%.0f)", g_stand.at.X, g_stand.at.Y, g_stand.at.Z);
        return;
    }
    const float a = 0.7853982f * static_cast<float>(g_stand.tried);
    E::SetActorLocation(me, {g_stand.at.X + std::cos(a) * g_stand.out, g_stand.at.Y + std::sin(a) * g_stand.out,
                             g_stand.at.Z + 60.f});
    g_stand.sinceMs = Now();
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[COVER] probe armed role=%s", host ? "host" : "client");
    }
    const uint64_t t = Now() - g_startAt;
    if (!host) TickStand();
    void* const desk = First(L"analogDScreenTest_C");
    struct Step { uint64_t at; bool host; int id; };
    static const Step kSteps[] = {{12000, false, 1}, {16000, false, 2}, {21000, false, 3}, {28000, false, 4},
                                  {33000, false, 5}, {38000, false, 6}, {42000, false, 7}, {50000, false, 8},
                                  {56000, true, 9},  {58000, true, 10}};
    if (g_step < static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]))) {
        const Step& s = kSteps[g_step];
        if (s.host != host) {
            ++g_step;
        } else if (t >= s.at) {
            ++g_step;
            switch (s.id) {
            case 1: {
                void* const p = Plate(desk, 0);
                ue_wrap::FVector at{};
                const bool ok = p && E::TryGetComponentLocation(p, at);
                if (ok) BeginStand(at, 110.f);
                UE_LOGI("[COVER] client: looks for a spot by the top left plate at (%.0f,%.0f,%.0f) (ok=%d)", at.X, at.Y, at.Z,
                        ok ? 1 : 0);
                break;
            }
            case 2:
            case 5:
                SetInt(desk, L"lookingAtPanel", 0);
                UE_LOGI("[COVER] client: ended a timed %hs of the top left plate (ran=%d)", s.id == 2 ? "unbolt" : "bolt",
                        CallPlain(desk, L"unscrewPanel") ? 1 : 0);
                break;
            case 3:
                SetInt(desk, L"lookingAtPanel", 0);
                UE_LOGI("[COVER] client: pressed E on the top left plate (ran=%d)", PressE(desk) ? 1 : 0);
                break;
            case 4:
                UE_LOGI("[COVER] client: carries the plate prop onto its lid for 2 s (plate=%d)", HeldPlate() ? 1 : 0);
                break;
            case 6: {
                void* const gen = Station0();
                ue_wrap::FVector at{};
                const bool ok = gen && E::TryGetActorLocation(gen, at);
                if (ok) BeginStand(at, 170.f);
                UE_LOGI("[COVER] client: looks for a spot by station 0 at (%.0f,%.0f,%.0f) (ok=%d)", at.X, at.Y, at.Z, ok ? 1 : 0);
                break;
            }
            case 7:
            case 8: {
                void* const gen = Station0();
                SetBool(gen, L"lookAtDoor", true);
                UE_LOGI("[COVER] client: pressed E on station 0's door (ran=%d)", PressE(gen) ? 1 : 0);
                SetBool(gen, L"lookAtDoor", false);
                break;
            }
            case 9:
                SetInt(desk, L"lookingAtPanel", 3);
                UE_LOGI("[COVER] host: ended a timed unbolt of the bottom right plate (ran=%d)",
                        CallPlain(desk, L"unscrewPanel") ? 1 : 0);
                break;
            case 10: {
                void* const gen = Station0();
                ue_wrap::ParamFrame f(gen ? R::FindDispatchFunctionCached(R::ClassOf(gen), L"openDoor") : nullptr);
                const bool ok = f.valid() && f.Set<bool>(L"open", Bool(gen, L"opened") != 1) && ue_wrap::Call(gen, f);
                UE_LOGI("[COVER] host: swung station 0's door (ran=%d)", ok ? 1 : 0);
                break;
            }
            default:
                break;
            }
        }
    }
    if (!host && t >= 28000u && t < 30000u) {  // the plate held on its lid, as a player carries it there
        void* const plate = HeldPlate();
        const int32_t off = desk ? R::FindPropertyOffset(R::ClassOf(desk), L"lid1") : -1;
        void* const lid = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const char*>(desk) + off) : nullptr;
        ue_wrap::FVector at{};
        if (plate && lid && E::TryGetComponentLocation(lid, at)) E::SetActorLocation(plate, at);
    }
    if (t < 12000u || t > 65000u || Now() < g_reportAt) return;
    g_reportAt = Now() + 1000;
    Report();
}

}  // namespace coop::dev::cover_probe
