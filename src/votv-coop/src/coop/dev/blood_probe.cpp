// coop/dev/blood_probe.cpp -- [dev] a player's blood on the ground (VOTVCOOP_BLOOD_PROBE=client|host, the bleeder, on
// both peers), for Ghezra/Multivoid#38: a bleeding player's blood marks were seen by nobody else.
//
// BOTH PEERS, at 20 s and 40 s after a client is connected: the live grime_blood decals are counted. THE BLEEDER at 24 s
// drops five drops of blood from 60 cm over its player through the blood master's own addParticle, as a bleeding body
// does; they fall and leave their marks. The spill lane's lines (`spill:`) say what crossed.

#include "coop/dev/blood_probe.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::blood_probe {
namespace {

namespace R = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;

uint64_t Now() { return ::GetTickCount64(); }

const std::string& Who() {
    static const std::string v = coop::config::ReadEnv("VOTVCOOP_BLOOD_PROBE");
    return v;
}

uint64_t g_startAt = 0;
int g_count = 0;
bool g_bled = false;

int CountBlood() {
    std::vector<void*> classes;
    OI::ForEachClass([](void* ctx, void* cls, void*) { static_cast<std::vector<void*>*>(ctx)->push_back(cls); }, &classes);
    int n = 0;
    for (void* cls : classes) {
        if (!R::NameStartsWith(R::NameOf(cls), L"grime_blood")) continue;
        OI::ForEachInstance(cls, [](void* c, void* o, int32_t) {
            if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) ++*static_cast<int*>(c);
        }, &n);
    }
    return n;
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (Who().empty() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[BLOOD] probe armed role=%s bleeder=%s", host ? "host" : "client", Who().c_str());
    }
    const uint64_t t = Now() - g_startAt;
    if ((g_count == 0 && t >= 20000u) || (g_count == 1 && t >= 40000u)) {
        UE_LOGI("[BLOOD] %s: %d blood mark(s) here", g_count == 0 ? "before" : "after", CountBlood());
        ++g_count;
    }
    if (g_bled || t < 24000u || Who() != (host ? "host" : "client")) return;
    g_bled = true;
    void* const me = coop::players::Registry::Get().Local();
    void* master = nullptr;
    for (void* o : R::FindObjectsByClass(L"bloodDecalMaster_C"))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) master = o;
    ue_wrap::FVector at{};
    void* const fn = master ? R::FindDispatchFunctionCached(R::ClassOf(master), L"addParticle") : nullptr;
    int ran = 0;
    if (fn && me && ue_wrap::engine::TryGetActorLocation(me, at)) {
        for (int i = 0; i < 5; ++i) {
            ue_wrap::ParamFrame f(fn);
            const ue_wrap::FVector loc{at.X + 40.f * static_cast<float>(i - 2), at.Y + 30.f, at.Z + 60.f};
            const ue_wrap::FVector vel{0.f, 0.f, -200.f};
            if (f.valid() && f.Set<ue_wrap::FVector>(L"location", loc) && f.Set<ue_wrap::FVector>(L"velocity", vel) &&
                f.Set<float>(L"size", 1.f) && f.Set<int32_t>(L"type", 0) && ue_wrap::Call(master, f))
                ++ran;
        }
    }
    UE_LOGI("[BLOOD] bleeder: %d of 5 drops let fall (master %d)", ran, master ? 1 : 0);
}

}  // namespace coop::dev::blood_probe
