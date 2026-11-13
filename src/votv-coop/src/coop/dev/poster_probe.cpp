// coop/dev/poster_probe.cpp -- [dev] the posters on the rig (VOTVCOOP_POSTER_PROBE=1), for VOTV-MP/Multivoid issue 56.
//
// HOST, 25 s after a client is connected: puts a poster up 2 m before its player, as a poster item's put spawns one
// (poster_c, `type` 3). CLIENT, 45 s after it is connected: takes the nearest poster whose `type` is not the default down
// through its own actionOptionIndex, action 7, as the use key does; 15 s later it puts one up the same way the host did
// (`type` 2). BOTH, every 2 s: every placed poster (`[POSTER] wall`: key, type, place) and every poster item
// (`[POSTER] item`: class, type, key, place), so the two peers' lines can be compared.

#include "coop/dev/poster_probe.h"

#include "coop/config/config.h"
#include "coop/creatures/npc_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::poster_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;

uint64_t Now() { return ::GetTickCount64(); }

bool On() {
    static const bool on = coop::config::ReadEnv("VOTVCOOP_POSTER_PROBE") == "1";
    return on;
}

void Collect(const wchar_t* cls, std::vector<void*>& out) {
    void* c = OI::ClassByName(cls);
    if (!c) return;
    OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
            static_cast<std::vector<void*>*>(ctx)->push_back(obj);
    }, &out);
}

int32_t IntField(void* obj, const wchar_t* name) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), name);
    return off >= 0 ? *reinterpret_cast<const int32_t*>(static_cast<const char*>(obj) + off) : -999;
}

std::wstring KeyOf(void* obj) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), L"Key");
    if (off < 0) return L"?";
    return R::ToString(*reinterpret_cast<const R::FName*>(static_cast<const char*>(obj) + off));
}

bool PutUp(int32_t type) {
    void* const me = coop::players::Registry::Get().Local();
    void* const cls = R::FindClass(L"poster_c");
    coop::npc_sync::DevSpawnRefs refs;
    ue_wrap::FVector at{};
    if (!me || !cls || !coop::npc_sync::GetDevSpawnRefs(refs) || !E::TryGetActorLocation(me, at)) return false;
    const ue_wrap::FVector fwd = E::GetActorForwardVector(me);
    ue_wrap::FTransform xf{};
    xf.TX = at.X + fwd.X * 200.f;
    xf.TY = at.Y + fwd.Y * 200.f;
    xf.TZ = at.Z + 60.f;
    ue_wrap::ParamFrame begin(refs.beginDeferredFn);
    if (!begin.valid()) return false;
    begin.Set<void*>(L"WorldContextObject", me);
    begin.Set<void*>(L"ActorClass", cls);
    begin.SetRaw(L"SpawnTransform", &xf, sizeof(xf));
    if (!ue_wrap::Call(refs.gsCdo, begin)) return false;
    void* const poster = begin.Get<void*>(L"ReturnValue");
    if (!poster) return false;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(poster), L"type");
    if (off >= 0) *reinterpret_cast<int32_t*>(static_cast<char*>(poster) + off) = type;
    ue_wrap::ParamFrame finish(refs.finishSpawnFn);
    if (!finish.valid()) return false;
    finish.Set<void*>(L"Actor", poster);
    finish.SetRaw(L"SpawnTransform", &xf, sizeof(xf));
    return ue_wrap::Call(refs.gsCdo, finish);
}

bool TakeDownNearest() {
    void* const me = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    if (!me || !E::TryGetActorLocation(me, at)) return false;
    std::vector<void*> walls;
    Collect(L"poster_c", walls);
    void* best = nullptr;
    float bestD = 1e30f;
    for (void* w : walls) {
        ue_wrap::FVector p{};
        if (!E::TryGetActorLocation(w, p) || IntField(w, L"type") <= 0) continue;  // a picture other than the default
        const float d = (p.X - at.X) * (p.X - at.X) + (p.Y - at.Y) * (p.Y - at.Y) + (p.Z - at.Z) * (p.Z - at.Z);
        if (d < bestD) { bestD = d; best = w; }
    }
    if (!best) return false;
    UE_LOGI("[POSTER] client: taking down key=%ls type=%d %.0f cm away", KeyOf(best).c_str(), IntField(best, L"type"),
            std::sqrt(bestD));
    ue_wrap::ParamFrame f(R::FindFunction(R::ClassOf(best), L"actionOptionIndex"));
    if (!f.valid()) return false;
    f.Set<void*>(L"player", me);
    f.Set<uint8_t>(L"action", 7);
    return ue_wrap::Call(best, f);
}

void Report() {
    std::vector<void*> walls, items;
    Collect(L"poster_c", walls);
    for (const wchar_t* c : {L"prop_poster_C", L"prop_poster_0_C", L"prop_poster_1_C", L"prop_poster_2_C", L"prop_poster_3_C",
                             L"prop_poster_4_C", L"prop_poster_5_C", L"prop_poster_6_C", L"prop_poster_7_C", L"prop_poster_8_C",
                             L"prop_poster_c_C"})
        Collect(c, items);
    UE_LOGI("[POSTER] census walls=%zu items=%zu", walls.size(), items.size());
    for (void* w : walls) {
        ue_wrap::FVector p{};
        E::TryGetActorLocation(w, p);
        UE_LOGI("[POSTER] wall key=%ls cls=%ls type=%d at=(%.0f,%.0f,%.0f)", KeyOf(w).c_str(), R::ClassNameOf(w).c_str(),
                IntField(w, L"type"), p.X, p.Y, p.Z);
    }
    for (void* it : items) {
        ue_wrap::FVector p{};
        E::TryGetActorLocation(it, p);
        UE_LOGI("[POSTER] item key=%ls cls=%ls type=%d at=(%.0f,%.0f,%.0f)", KeyOf(it).c_str(), R::ClassNameOf(it).c_str(),
                IntField(it, L"type"), p.X, p.Y, p.Z);
    }
}

uint64_t g_startAt = 0, g_reportAt = 0;
bool g_done = false, g_clientPut = false;

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[POSTER] probe armed role=%s", host ? "host" : "client");
    }
    const uint64_t t = Now() - g_startAt;
    if (!g_done && t >= (host ? 25000u : 45000u)) {
        g_done = true;
        if (host) UE_LOGI("[POSTER] host: put a poster up (ok=%d)", PutUp(3) ? 1 : 0);
        else UE_LOGI("[POSTER] client: took the nearest poster down (ran=%d)", TakeDownNearest() ? 1 : 0);
    }
    if (!host && !g_clientPut && t >= 60000u) {
        g_clientPut = true;
        UE_LOGI("[POSTER] client: put a poster up (ok=%d)", PutUp(2) ? 1 : 0);
    }
    if (t < 20000 || Now() < g_reportAt) return;
    g_reportAt = Now() + 2000;
    Report();
}

}  // namespace coop::dev::poster_probe
