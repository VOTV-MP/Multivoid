// coop/dev/ufo_probe.cpp -- see coop/dev/ufo_probe.h.

#include "coop/dev/ufo_probe.h"

#include "coop/config/config.h"
#include "coop/creatures/npc_adoption.h"
#include "coop/creatures/npc_sync.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/world/keyed_objects.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <cstdint>

namespace coop::dev::ufo_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace EL = coop::element;

uint64_t Now() { return ::GetTickCount64(); }

bool On() {
    static const bool on = coop::config::ReadEnv("VOTVCOOP_UFO_PROBE") == "1";
    return on;
}

struct Ufo {
    const wchar_t* cls;
    const wchar_t* part;  // the component that moves
};
constexpr Ufo kUfos[] = {
    {L"ufo_ballfo_C", L"balls"},
    {L"ufo_boofo_spawn1_C", L"ufo"},
    {L"ufo_pillfo_C", L"center"},
    {L"ufo_joel_C", L"point_a1"},
};

void* FirstLive(const wchar_t* cls) {
    struct Ctx { void* found; } ctx{nullptr};
    if (void* c = OI::ClassByName(cls))
        OI::ForEachInstance(c, [](void* p, void* obj, int32_t) {
            auto& x = *static_cast<Ctx*>(p);
            if (!x.found && obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) x.found = obj;
        }, &ctx);
    return ctx.found;
}

void* Part(void* actor, const wchar_t* name) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), name);
    return off >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(actor) + off) : nullptr;
}

bool CallGm(const wchar_t* fn) {
    void* gm = ue_wrap::world_singleton::Gamemode();
    void* f = gm ? R::FindFunction(R::ClassOf(gm), fn) : nullptr;
    ue_wrap::ParamFrame frame(f);
    return f && frame.valid() && ue_wrap::Call(gm, frame);
}

// A UFO minted the way any host code would (GameplayStatics' deferred spawn), so the world-actor lane enrols it.
bool Mint(const wchar_t* cls, const ue_wrap::FVector& at) {
    coop::npc_sync::DevSpawnRefs refs;
    void* const c = R::FindClass(cls);
    void* const ctx = E::GetWorldContext();
    if (!c || !ctx || !coop::npc_sync::GetDevSpawnRefs(refs)) return false;
    ue_wrap::FTransform xf{};
    xf.TX = at.X;
    xf.TY = at.Y;
    xf.TZ = at.Z;
    ue_wrap::ParamFrame begin(refs.beginDeferredFn);
    if (!begin.valid()) return false;
    begin.Set<void*>(L"WorldContextObject", ctx);
    begin.Set<void*>(L"ActorClass", c);
    begin.SetRaw(L"SpawnTransform", &xf, sizeof(xf));
    begin.Set<uint8_t>(L"CollisionHandlingOverride", 1);
    if (!ue_wrap::Call(refs.gsCdo, begin)) return false;
    void* const actor = begin.Get<void*>(L"ReturnValue");
    if (!actor) return false;
    ue_wrap::ParamFrame finish(refs.finishSpawnFn);
    if (!finish.valid()) return false;
    finish.Set<void*>(L"Actor", actor);
    finish.SetRaw(L"SpawnTransform", &xf, sizeof(xf));
    return ue_wrap::Call(refs.gsCdo, finish);
}

uint64_t g_startAt = 0;
int      g_step = 0;
uint64_t g_stepAt = 0;
uint64_t g_reportAt = 0;

void StepHost() {
    if (Now() < g_stepAt) return;
    g_stepAt = Now() + 5000;
    const ue_wrap::FVector me = E::GetCameraLocation();
    switch (g_step) {
    case 0: {
        // One call a frame until a ball is listed: a fresh instance is not listed in the call that made it.
        static int calls = 0;
        if (!FirstLive(L"ufo_ballfo_C") && calls < 60) {
            CallGm(L"ufo_ball");
            ++calls;
            g_stepAt = Now() + 200;
            return;
        }
        UE_LOGI("[UFO] host: the gamemode's ufo_ball ran %d time(s) -- a ball %hs", calls,
                FirstLive(L"ufo_ballfo_C") ? "flies" : "did NOT come");
        break;
    }
    case 1:
        UE_LOGI("[UFO] host: boo minted=%d", Mint(L"ufo_boofo_spawn1_C", {me.X + 3000.f, me.Y, me.Z + 3000.f}) ? 1 : 0);
        break;
    case 2:
        UE_LOGI("[UFO] host: pill minted=%d", Mint(L"ufo_pillfo_C", {me.X, me.Y + 3000.f, me.Z + 2000.f}) ? 1 : 0);
        break;
    case 3:
        UE_LOGI("[UFO] host: joel minted=%d", Mint(L"ufo_joel_C", {me.X - 3000.f, me.Y, me.Z + 3000.f}) ? 1 : 0);
        break;
    case 4: {
        void* midas = ue_wrap::keyed_objects::Resolve(L"midasufo");
        ue_wrap::ParamFrame f(midas ? R::FindFunction(R::ClassOf(midas), L"runTrigger") : nullptr);
        bool ran = false;
        if (f.valid()) ran = ue_wrap::Call(midas, f);
        UE_LOGI("[UFO] host: the Midas UFO's runTrigger ran=%d", ran ? 1 : 0);
        break;
    }
    default:
        return;
    }
    ++g_step;
}

void StepClient() {
    if (g_step != 0) return;
    g_step = 1;
    for (const wchar_t* fn : {L"ufo_ball", L"ufo_boo", L"ufo_pill", L"ufo_joel", L"ufo_midas"})
        CallGm(fn);
    UE_LOGI("[UFO] client: called its own five UFO timers");
}

void Report() {
    if (Now() < g_reportAt) return;
    g_reportAt = Now() + 2000;
    for (const Ufo& u : kUfos) {
        void* c = OI::ClassByName(u.cls);
        if (!c) continue;
        OI::ForEachInstance(c, [](void* p, void* obj, int32_t) {
            const Ufo& uu = *static_cast<const Ufo*>(p);
            if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            void* part = Part(obj, uu.part);
            ue_wrap::FVector at{};
            if (!part || !ue_wrap::engine::TryGetComponentLocation(part, at)) return;
            UE_LOGI("[UFO] at %ls eid=%u part=(%.0f,%.0f,%.0f)", uu.cls,
                    static_cast<unsigned>(EL::Registry::Get().EidForActor(obj)), at.X, at.Y, at.Z);
            void* center = Part(obj, L"center");
            void* b = Part(obj, L"b");
            ue_wrap::FVector bw{};
            if (center && b && ue_wrap::engine::TryGetComponentLocation(b, bw)) {
                const char* cc = static_cast<const char*>(center);
                const char* bc = static_cast<const char*>(b);
                const auto* rr = reinterpret_cast<const ue_wrap::FRotator*>(cc + ue_wrap::profile::off::USceneComponent_RelativeRotation);
                const auto* bl = reinterpret_cast<const ue_wrap::FVector*>(bc + ue_wrap::profile::off::USceneComponent_RelativeLocation);
                UE_LOGI("[UFO] path eid=%u center=(%.1f,%.1f,%.1f) b.rel=(%.0f,%.0f,%.0f) b.world=(%.0f,%.0f,%.0f)", static_cast<unsigned>(EL::Registry::Get().EidForActor(obj)), rr->Pitch,
                        rr->Yaw, rr->Roll, bl->X, bl->Y, bl->Z, bw.X, bw.Y, bw.Z);
            }
        }, const_cast<Ufo*>(&u));
    }
    if (void* midas = ue_wrap::keyed_objects::Resolve(L"midasufo")) {
        ue_wrap::FVector at{};
        if (void* fwd = Part(midas, L"forward"); fwd && ue_wrap::engine::TryGetComponentLocation(fwd, at))
            UE_LOGI("[UFO] at midasufotest_C part=(%.0f,%.0f,%.0f)", at.X, at.Y, at.Z);
    }
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (host ? !session->connected() : !coop::npc_adoption::GhostSweepDone()) return;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[UFO] probe armed role=%s", host ? "host" : "client");
    }
    if (Now() - g_startAt < 20000) return;
    if (host) StepHost();
    else StepClient();
    Report();
}

}  // namespace coop::dev::ufo_probe
