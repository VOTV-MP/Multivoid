// coop/dev/tower_poof_probe.cpp -- [dev] the stolen radio tower on the rig (VOTVCOOP_TOWER_POOF_PROBE=1), for
// VOTV-MP/Multivoid issue 63.
//
// The gamemode's ticker_radiotowerPoof (every hour, chance 0.005) spawns a radiotowerPoof_C, which waits `d` seconds,
// then each second hides the gamemode's radio tower once that peer's view has not rendered it lately, and `d2` seconds
// later shows it again the same way. HOST, 25 s after a client is connected: spawns the event's actor as the ticker
// does, with `d` 2 s and `d2` 40 s. BOTH, every 2 s: whether the tower is hidden and its collision on
// (`[POOF] tower`, hidden read off the tower's `segm_r` part), and how many of the event's actors stand, so the two peers' lines can be compared.

#include "coop/dev/tower_poof_probe.h"

#include "coop/config/config.h"
#include "coop/creatures/npc_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <cstdint>

namespace coop::dev::tower_poof_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;

uint64_t Now() { return ::GetTickCount64(); }

bool On() {
    static const bool on = coop::config::ReadEnv("VOTVCOOP_TOWER_POOF_PROBE") == "1";
    return on;
}

void* Tower() {
    void* gm = ue_wrap::world_singleton::Gamemode();
    const int32_t off = gm ? R::FindPropertyOffset(R::ClassOf(gm), L"radiotower") : -1;
    void* t = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const char*>(gm) + off) : nullptr;
    return t && R::IsLive(t) ? t : nullptr;
}

int Flag(void* actor, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!actor || !R::FindBoolProperty(R::ClassOf(actor), name, off, mask)) return -1;
    return (static_cast<const uint8_t*>(actor)[off] & mask) ? 1 : 0;
}

// The tower's own setHidden hides each of its parts in game: one of them, read as the engine keeps it.
int PartHidden(void* tower, const wchar_t* part) {
    const int32_t off = tower ? R::FindPropertyOffset(R::ClassOf(tower), part) : -1;
    void* c = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const char*>(tower) + off) : nullptr;
    if (!c || !R::IsLive(c)) return -1;
    return (static_cast<const uint8_t*>(c)[ue_wrap::profile::off::USceneComponent_HiddenFlagsByte] & 0x04) ? 1 : 0;
}

size_t Poofs() {
    size_t n = 0;
    void* c = OI::ClassByName(L"radiotowerPoof_C");
    if (!c) return 0;
    OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) ++*static_cast<size_t*>(ctx);
    }, &n);
    return n;
}

bool WriteFloat(void* obj, const wchar_t* name, float v) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), name);
    if (off < 0) return false;
    *reinterpret_cast<float*>(static_cast<char*>(obj) + off) = v;
    return true;
}

bool Fire() {
    void* const me = coop::players::Registry::Get().Local();
    void* const cls = R::FindClass(L"radiotowerPoof_C");
    coop::npc_sync::DevSpawnRefs refs;
    if (!me || !cls || !coop::npc_sync::GetDevSpawnRefs(refs)) return false;
    ue_wrap::FTransform xf{};
    ue_wrap::ParamFrame begin(refs.beginDeferredFn);
    if (!begin.valid()) return false;
    begin.Set<void*>(L"WorldContextObject", me);
    begin.Set<void*>(L"ActorClass", cls);
    begin.SetRaw(L"SpawnTransform", &xf, sizeof(xf));
    if (!ue_wrap::Call(refs.gsCdo, begin)) return false;
    void* const poof = begin.Get<void*>(L"ReturnValue");
    if (!poof || !WriteFloat(poof, L"d", 2.f) || !WriteFloat(poof, L"d2", 40.f)) return false;
    ue_wrap::ParamFrame finish(refs.finishSpawnFn);
    if (!finish.valid()) return false;
    finish.Set<void*>(L"Actor", poof);
    finish.SetRaw(L"SpawnTransform", &xf, sizeof(xf));
    return ue_wrap::Call(refs.gsCdo, finish);
}

uint64_t g_startAt = 0, g_reportAt = 0;
bool g_done = false;

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[POOF] probe armed role=%s", host ? "host" : "client");
    }
    const uint64_t t = Now() - g_startAt;
    if (host && !g_done && t >= 25000u) {
        g_done = true;
        UE_LOGI("[POOF] host: fired the tower's event (ok=%d)", Fire() ? 1 : 0);
    }
    if (t < 20000 || Now() < g_reportAt) return;
    g_reportAt = Now() + 2000;
    void* tower = Tower();
    UE_LOGI("[POOF] tower found=%d hidden=%d collision=%d poofs=%zu", tower ? 1 : 0, PartHidden(tower, L"segm_r"),
            Flag(tower, L"bActorEnableCollision"), Poofs());
}

}  // namespace coop::dev::tower_poof_probe
