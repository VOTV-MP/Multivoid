// coop/dev/miner_probe.cpp -- [dev] the GPU mining rig on the rig (VOTVCOOP_MINER_PROBE=1), for VOTV-MP/Multivoid issue 43.
//
// HOST, 25 s after a client is connected: spawns a prop_miner_C 2 m before its player; 10 s later it builds it up through
// the rig's own `upgrade`, as a held item used on it does: two slots (scrap_metal), two GPUs, one cooler. CLIENT, 50 s
// after it is connected: claims its copy's points through actionOptionIndex action 4, as the use key does; 10 s later it
// uses scrap metal on it through the rig's own playerUsedOn, its player's holding_name set to scrap_metal for the call,
// as a use with the item in hand reaches the body. BOTH, every 2 s: every rig's key, upgrades, points and power (`[MINER] rig`),
// so the two peers' lines can be compared.

#include "coop/dev/miner_probe.h"

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
#include <string>
#include <vector>

namespace coop::dev::miner_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;

uint64_t Now() { return ::GetTickCount64(); }

bool On() {
    static const bool on = coop::config::ReadEnv("VOTVCOOP_MINER_PROBE") == "1";
    return on;
}

void Rigs(std::vector<void*>& out) {
    void* c = OI::ClassByName(L"prop_miner_C");
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

int BoolField(void* obj, const wchar_t* name) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), name);
    return off >= 0 ? (*(static_cast<const uint8_t*>(obj) + off) ? 1 : 0) : -1;
}

std::wstring KeyOf(void* obj) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), L"Key");
    if (off < 0) return L"?";
    return R::ToString(*reinterpret_cast<const R::FName*>(static_cast<const char*>(obj) + off));
}

void* Nearest() {
    void* const me = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    if (!me || !E::TryGetActorLocation(me, at)) return nullptr;
    std::vector<void*> rigs;
    Rigs(rigs);
    void* best = nullptr;
    float bestD = 1e30f;
    for (void* r : rigs) {
        ue_wrap::FVector p{};
        if (!E::TryGetActorLocation(r, p)) continue;
        const float d = (p.X - at.X) * (p.X - at.X) + (p.Y - at.Y) * (p.Y - at.Y) + (p.Z - at.Z) * (p.Z - at.Z);
        if (d < bestD) { bestD = d; best = r; }
    }
    return best;
}

bool Upgrade(void* rig, const wchar_t* item) {
    ue_wrap::ParamFrame f(R::FindFunction(R::ClassOf(rig), L"upgrade"));
    if (!f.valid()) return false;
    f.Set<R::FName>(L"upgrade", ue_wrap::fname_utils::StringToFName(item));
    return ue_wrap::Call(rig, f) && f.Get<bool>(L"success");
}

void* Spawn() {
    void* const me = coop::players::Registry::Get().Local();
    void* const cls = R::FindClass(L"prop_miner_C");
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
    void* const rig = begin.Get<void*>(L"ReturnValue");
    if (!rig) return nullptr;
    ue_wrap::ParamFrame finish(refs.finishSpawnFn);
    if (!finish.valid()) return nullptr;
    finish.Set<void*>(L"Actor", rig);
    finish.SetRaw(L"SpawnTransform", &xf, sizeof(xf));
    return ue_wrap::Call(refs.gsCdo, finish) ? rig : nullptr;
}

bool UseScrap(void* rig) {
    void* const me = coop::players::Registry::Get().Local();
    const int32_t off = me ? R::FindPropertyOffset(R::ClassOf(me), L"holding_name") : -1;
    ue_wrap::ParamFrame f(R::FindFunction(R::ClassOf(rig), L"playerUsedOn"));
    if (off < 0 || !f.valid()) return false;
    R::FName& held = *reinterpret_cast<R::FName*>(static_cast<char*>(me) + off);
    const R::FName was = held;
    held = ue_wrap::fname_utils::StringToFName(L"scrap_metal");
    f.Set<void*>(L"player", me);
    const bool ran = ue_wrap::Call(rig, f);
    held = was;
    return ran;
}

bool Claim(void* rig) {
    void* const me = coop::players::Registry::Get().Local();
    ue_wrap::ParamFrame f(R::FindFunction(R::ClassOf(rig), L"actionOptionIndex"));
    if (!me || !f.valid()) return false;
    f.Set<void*>(L"player", me);
    f.Set<uint8_t>(L"action", 4);
    return ue_wrap::Call(rig, f);
}

void Report() {
    std::vector<void*> rigs;
    Rigs(rigs);
    UE_LOGI("[MINER] census rigs=%zu", rigs.size());
    for (void* r : rigs) {
        ue_wrap::FVector p{};
        E::TryGetActorLocation(r, p);
        UE_LOGI("[MINER] rig key=%ls gpu=%d slots=%d fans=%d points=%d powered=%d at=(%.0f,%.0f,%.0f)", KeyOf(r).c_str(),
                IntField(r, L"upg_gpu"), IntField(r, L"upg_slots"), IntField(r, L"upg_fans"),
                IntField(r, L"accumulatedPoints"), BoolField(r, L"powered"), p.X, p.Y, p.Z);
    }
}

uint64_t g_startAt = 0, g_reportAt = 0;
bool g_done = false, g_clientAdd = false, g_hostBuilt = false;

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[MINER] probe armed role=%s", host ? "host" : "client");
    }
    const uint64_t t = Now() - g_startAt;
    if (!g_done && t >= (host ? 25000u : 50000u)) {
        g_done = true;
        if (host) {
            void* rig = Spawn();
            UE_LOGI("[MINER] host: spawned a rig key=%ls", rig ? KeyOf(rig).c_str() : L"-");
        } else {
            void* rig = Nearest();
            UE_LOGI("[MINER] client: claimed key=%ls (ran=%d)", rig ? KeyOf(rig).c_str() : L"-", rig && Claim(rig) ? 1 : 0);
        }
    }
    if (host && !g_hostBuilt && t >= 35000u) {
        g_hostBuilt = true;
        void* rig = Nearest();
        const bool ok = rig && Upgrade(rig, L"scrap_metal") && Upgrade(rig, L"scrap_metal") &&
                        Upgrade(rig, L"miner_gpu") && Upgrade(rig, L"miner_gpu") && Upgrade(rig, L"miner_fan");
        UE_LOGI("[MINER] host: built the rig up key=%ls (ok=%d)", rig ? KeyOf(rig).c_str() : L"-", ok ? 1 : 0);
    }
    if (!host && !g_clientAdd && t >= 60000u) {
        g_clientAdd = true;
        void* rig = Nearest();
        UE_LOGI("[MINER] client: added a slot key=%ls (ran=%d)", rig ? KeyOf(rig).c_str() : L"-",
                rig && UseScrap(rig) ? 1 : 0);
    }
    if (t < 20000 || Now() < g_reportAt) return;
    g_reportAt = Now() + 2000;
    Report();
}

}  // namespace coop::dev::miner_probe
