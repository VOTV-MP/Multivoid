// coop/dev/foam_probe.cpp -- [dev] a fire extinguisher's foam on the rig (VOTVCOOP_FOAM_PROBE=1).
//
// CLIENT, 15 s after it is connected: takes an extinguisher into its hand through the drill hand (the hold slot and the
// game's updateHold); at 20 s it presses the hand's LMB through the extinguisher's own playerHandUse_LMB, and at 26 s
// releases it through playerHandRelease_LMB, as the player's LMB does. HOST, at 30 s: spawns a prop_fireExt_C 2 m before
// its player; at 35 s runs its `shoot`, as an impact does. BOTH, every second from 16 s to 50 s: this peer's own hand
// extinguisher (`[FOAM] own`), every other player's hand mirror that is one (`[FOAM] mirror`), and every world
// extinguisher (`[FOAM] world`): firing or active, the charge, and whether its spray and thrust effects are active.

#include "coop/dev/foam_probe.h"

#include "coop/config/config.h"
#include "coop/creatures/npc_sync.h"
#include "coop/dev/drill_hand.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cstdint>
#include <vector>

namespace coop::dev::foam_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;

constexpr const wchar_t* kClass = L"prop_fireExt_C";

uint64_t Now() { return ::GetTickCount64(); }

bool On() {
    static const bool on = coop::config::ReadEnv("VOTVCOOP_FOAM_PROBE") == "1";
    return on;
}

bool IsExt(void* obj) {
    void* const base = OI::ClassByName(kClass);
    return obj && base && R::IsLive(obj) && R::IsDescendantOfAny(R::ClassOf(obj), &base, 1);
}

int Bool(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return -1;
    return (static_cast<const uint8_t*>(obj)[off] & mask) ? 1 : 0;
}

float Life(void* obj) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), L"life");
    return off >= 0 ? *reinterpret_cast<const float*>(static_cast<const char*>(obj) + off) : -1.f;
}

int Active(void* obj, const wchar_t* comp) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), comp);
    void* const c = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const char*>(obj) + off) : nullptr;
    if (!c) return -1;
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(c), L"IsActive"));
    return f.valid() && ue_wrap::Call(c, f) ? (f.Get<bool>(L"ReturnValue") ? 1 : 0) : -1;
}

void* Held() {
    void* const me = coop::players::Registry::Get().Local();
    const int32_t off = me ? R::FindPropertyOffset(R::ClassOf(me), L"holding_actor") : -1;
    void* const h = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const char*>(me) + off) : nullptr;
    return IsExt(h) ? h : nullptr;
}

bool CallLmb(void* ext, const wchar_t* verb) {
    void* const me = coop::players::Registry::Get().Local();
    ue_wrap::ParamFrame f(ext ? R::FindDispatchFunctionCached(R::ClassOf(ext), verb) : nullptr);
    return me && f.valid() && f.Set<void*>(L"player", me) && ue_wrap::Call(ext, f);
}

void* Spawn() {
    void* const me = coop::players::Registry::Get().Local();
    void* const cls = R::FindClass(kClass);
    coop::npc_sync::DevSpawnRefs refs;
    ue_wrap::FVector at{};
    if (!me || !cls || !coop::npc_sync::GetDevSpawnRefs(refs) || !E::TryGetActorLocation(me, at)) return nullptr;
    const ue_wrap::FVector fwd = E::GetActorForwardVector(me);
    ue_wrap::FTransform xf{};
    xf.TX = at.X + fwd.X * 200.f;
    xf.TY = at.Y + fwd.Y * 200.f;
    xf.TZ = at.Z;
    ue_wrap::ParamFrame begin(refs.beginDeferredFn);
    if (!begin.valid()) return nullptr;
    begin.Set<void*>(L"WorldContextObject", me);
    begin.Set<void*>(L"ActorClass", cls);
    begin.SetRaw(L"SpawnTransform", &xf, sizeof(xf));
    if (!ue_wrap::Call(refs.gsCdo, begin)) return nullptr;
    void* const ext = begin.Get<void*>(L"ReturnValue");
    if (!ext) return nullptr;
    ue_wrap::ParamFrame finish(refs.finishSpawnFn);
    if (!finish.valid()) return nullptr;
    finish.Set<void*>(L"Actor", ext);
    finish.SetRaw(L"SpawnTransform", &xf, sizeof(xf));
    return ue_wrap::Call(refs.gsCdo, finish) ? ext : nullptr;
}

void Report() {
    if (void* own = Held())
        UE_LOGI("[FOAM] own firing=%d life=%.3f spray=%d", Bool(own, L"firing"), Life(own), Active(own, L"eff_ext"));
    for (uint8_t s = 0; s < coop::net::kMaxPeers; ++s) {
        void* const m = coop::hand_item::MirrorActorForSlot(s);
        if (IsExt(m))
            UE_LOGI("[FOAM] mirror slot=%u firing=%d spray=%d sound=%d", s, Bool(m, L"firing"), Active(m, L"eff_ext"),
                    Active(m, L"fireExtFire"));
    }
    void* const cls = OI::ClassByName(kClass);
    std::vector<void*> all;
    if (cls)
        OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &all);
    for (void* e : all) {
        if (coop::hand_item::IsHandAxisActor(e)) continue;
        const auto eid = coop::element::Registry::Get().EidForActor(e);
        UE_LOGI("[FOAM] world eid=%u active=%d life=%.3f thrust=%d sound=%d", static_cast<unsigned>(eid),
                Bool(e, L"active"), Life(e), Active(e, L"eff_fireExtThr"), Active(e, L"fireExtThrust"));
    }
}

uint64_t g_startAt = 0, g_reportAt = 0;
bool g_took = false, g_pressed = false, g_released = false, g_spawned = false, g_shot = false;
bool g_hostTook = false, g_hostPressed = false, g_hostReleased = false;
void* g_world = nullptr;

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[FOAM] probe armed role=%s", host ? "host" : "client");
    }
    const uint64_t t = Now() - g_startAt;
    if (!host && !g_took && t >= 15000u) {
        g_took = true;
        const wchar_t* const names[] = {L"fireExt", L"fireext", L"fireExtinguisher", L"extinguisher", L"fire_extinguisher"};
        for (const wchar_t* n : names) {
            const bool ok = coop::dev::drill_hand::TakeIntoHandNow(n, kClass);
            UE_LOGI("[FOAM] client: took '%ls' into the hand (ok=%d, holds an extinguisher=%d)", n, ok ? 1 : 0,
                    Held() ? 1 : 0);
            if (Held()) break;
        }
    }
    if (!host && !g_pressed && t >= 20000u) {
        g_pressed = true;
        void* const own = Held();
        const int32_t off = own ? R::FindPropertyOffset(R::ClassOf(own), L"life") : -1;
        if (off >= 0) *reinterpret_cast<float*>(static_cast<char*>(own) + off) = 1.f;  // a full one, as one off a mount
        UE_LOGI("[FOAM] client: pressed the hand's LMB (ran=%d)", CallLmb(Held(), L"playerHandUse_LMB") ? 1 : 0);
    }
    if (!host && !g_released && t >= 26000u) {
        g_released = true;
        UE_LOGI("[FOAM] client: released the hand's LMB (ran=%d)", CallLmb(Held(), L"playerHandRelease_LMB") ? 1 : 0);
    }
    if (host && !g_spawned && t >= 30000u) {
        g_spawned = true;
        g_world = Spawn();
        UE_LOGI("[FOAM] host: spawned a world extinguisher (ok=%d)", g_world ? 1 : 0);
    }
    if (host && !g_shot && t >= 35000u) {
        g_shot = true;
        const bool ok = g_world && R::IsLive(g_world) &&
                        R::CallFunction(g_world, R::FindDispatchFunctionCached(R::ClassOf(g_world), L"shoot"), nullptr);
        UE_LOGI("[FOAM] host: set the world extinguisher thrusting through its shoot (ran=%d)", ok ? 1 : 0);
    }
    // The host's own spray, 40 s to 46 s: what its clients must see on their mirror of its hand.
    if (host && !g_hostTook && t >= 38000u) {
        g_hostTook = true;
        const bool ok = coop::dev::drill_hand::TakeIntoHandNow(L"fireExt", kClass);
        UE_LOGI("[FOAM] host: took an extinguisher into the hand (ok=%d, holds one=%d)", ok ? 1 : 0, Held() ? 1 : 0);
    }
    if (host && !g_hostPressed && t >= 40000u) {
        g_hostPressed = true;
        void* const own = Held();
        const int32_t off = own ? R::FindPropertyOffset(R::ClassOf(own), L"life") : -1;
        if (off >= 0) *reinterpret_cast<float*>(static_cast<char*>(own) + off) = 1.f;
        UE_LOGI("[FOAM] host: pressed the hand's LMB (ran=%d)", CallLmb(own, L"playerHandUse_LMB") ? 1 : 0);
    }
    if (host && !g_hostReleased && t >= 46000u) {
        g_hostReleased = true;
        UE_LOGI("[FOAM] host: released the hand's LMB (ran=%d)", CallLmb(Held(), L"playerHandRelease_LMB") ? 1 : 0);
    }
    if (t < 16000u || t > 50000u || Now() < g_reportAt) return;
    g_reportAt = Now() + 1000;
    Report();
}

}  // namespace coop::dev::foam_probe
