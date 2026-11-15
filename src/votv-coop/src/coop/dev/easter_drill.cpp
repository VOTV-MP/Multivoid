// coop/dev/easter_drill.cpp -- see coop/dev/easter_drill.h.

#include "coop/dev/easter_drill.h"

#include "coop/net/net_clock.h"
#include "coop/world/easter_hunt.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::easter_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

constexpr const wchar_t* kBunny = L"ominousBunny_C";
constexpr const wchar_t* kAngry = L"superHolyShitAngryBunny_C";
constexpr const wchar_t* kEgg = L"prop_easteregg_bunnyOwn_C";
constexpr const wchar_t* kEars = L"prop_equipment_bunnyears_C";
constexpr const wchar_t* kNote = L"prop_notebook_paper_u_C";
constexpr const wchar_t* kTalk = L"tutorialHint_C";
constexpr const wchar_t* kZone = L"easterZone_C";

uint64_t g_lastSayHost = 0, g_lastSayClient = 0, g_clientSeenMs = 0, g_clientBunnyMs = 0;
bool g_animCalled = false, g_bunnyEnded = false, g_cleaned = false;
bool g_clientFin = false, g_clientEgg = false, g_clientRepeat = false;
size_t g_notesBefore = 0;

struct Collect {
    std::vector<void*> found;
};

// This world's live actors of the class, through the object index (never a walk of every object).
std::vector<void*> Live(const wchar_t* className) {
    std::vector<void*> out;
    void* cls = ue_wrap::object_index::ClassByName(className);
    void* world = ue_wrap::world_identity::CurrentWorld();
    if (!cls || !world) return out;
    Collect c;
    ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        if (obj) static_cast<Collect*>(ctx)->found.push_back(obj);
    }, &c);
    for (void* obj : c.found)
        if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__") && ue_wrap::world_identity::WorldOf(obj) == world)
            out.push_back(obj);
    return out;
}

bool Flag(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    return obj && R::FindBoolProperty(R::ClassOf(obj), name, off, mask) && off >= 0 &&
           (*(static_cast<const uint8_t*>(obj) + off) & mask) != 0;
}

bool Call0(void* obj, const wchar_t* fn) {
    void* f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    return f && R::CallFunction(obj, f, nullptr);
}

void Say(const char* who, uint64_t sinceMs) {
    const std::vector<void*> bunnies = Live(kBunny);
    void* b = bunnies.empty() ? nullptr : bunnies.front();
    UE_LOGI("[EASTER-DRILL] [%s] t=%us: %zu bunny (eggs out %d, all in %d), %zu angry, %zu egg(s), %zu ears, %zu note(s), %zu talk object(s)",
            who, static_cast<unsigned>(sinceMs / 1000), bunnies.size(), Flag(b, L"spawnedEggs") ? 1 : 0, Flag(b, L"collectedEggs") ? 1 : 0,
            Live(kAngry).size(), Live(kEgg).size(), Live(kEars).size(), Live(kNote).size(), Live(kTalk).size());
}

bool SayDue(uint64_t sinceMs, uint64_t& last) {
    const uint64_t bucket = sinceMs / 5000 + 1;
    if (bucket <= last) return false;
    last = bucket;
    return true;
}

}  // namespace

bool IsDrill(const std::string& name) { return name == "easterHunt"; }

void ClientStep() {
    const uint64_t now = coop::net::NowMs();
    if (!g_clientSeenMs) {
        g_clientSeenMs = now;
        coop::easter_hunt::SetDrillCallsCount(true);
    }
    if (SayDue(now - g_clientSeenMs, g_lastSayClient)) Say("C", now - g_clientSeenMs);
    static uint64_t s_lastLook = 0;   // one look a second
    if (now - s_lastLook < 1000) return;
    s_lastLook = now;
    const std::vector<void*> bunnies = Live(kBunny);
    if (bunnies.empty()) return;
    void* bunny = bunnies.front();
    if (!g_clientBunnyMs) g_clientBunnyMs = now;
    const uint64_t held = now - g_clientBunnyMs;
    if (!g_clientFin && held >= 6000) {
        g_clientFin = true;
        const bool ran = Call0(bunny, L"fin");
        UE_LOGI("[EASTER-DRILL] [C] fin called on this peer's bunny copy (returned %d): it holds %zu egg(s) right after", ran ? 1 : 0,
                Live(kEgg).size());
    }
    if (!g_clientEgg && held >= 16000) {
        g_clientEgg = true;
        const std::vector<void*> eggs = Live(kEgg);
        void* fn = R::FindDispatchFunctionCached(R::ClassOf(bunny), L"playerUsedOn");
        ue_wrap::ParamFrame f(fn);
        const bool ran = !eggs.empty() && fn && f.valid() && f.Set<void*>(L"HoldObject", eggs.front()) && ue_wrap::Call(bunny, f);
        UE_LOGI("[EASTER-DRILL] [C] an egg copy used on the bunny copy (returned %d): %zu egg(s) before, %zu right after", ran ? 1 : 0,
                eggs.size(), Live(kEgg).size());
    }
    if (!g_clientRepeat && held >= 24000) {
        g_clientRepeat = true;
        const size_t ears = Live(kEars).size();
        const bool ran = Call0(bunny, L"CustomEvent");
        UE_LOGI("[EASTER-DRILL] [C] CustomEvent called on the bunny copy (returned %d): %zu ears before, %zu right after", ran ? 1 : 0, ears,
                Live(kEars).size());
    }
}

constexpr uint64_t kEndBunnyMs = 100000, kCleanMs = 108000;   // the list watches 115 s

void HostStep(uint64_t since) {
    if (SayDue(since, g_lastSayHost)) Say("H", since);
    if (!g_animCalled && since >= 8000) {
        g_animCalled = true;
        coop::easter_hunt::SetDrillCallsCount(true);   // the zone's own tick makes this call in the game
        g_notesBefore = Live(kNote).size();
        const std::vector<void*> zones = Live(kZone);
        const bool ran = !zones.empty() && Call0(zones.front(), L"playAnimation");
        UE_LOGI("[EASTER-DRILL] [H] the level holds %zu zone(s); playAnimation called: %d", zones.size(), ran ? 1 : 0);
    }
    if (!g_bunnyEnded && since >= kEndBunnyMs) {
        g_bunnyEnded = true;
        const std::vector<void*> bunnies = Live(kBunny);
        for (void* b : bunnies) E::DestroyActor(b);
        UE_LOGI("[EASTER-DRILL] [H] %zu bunny destroyed before its hunt ended: %zu angry right after", bunnies.size(), Live(kAngry).size());
    }
    if (!g_cleaned && since >= kCleanMs) {
        g_cleaned = true;
        size_t n = 0;
        for (const wchar_t* cls : {kAngry, kEgg, kEars})
            for (void* a : Live(cls)) { E::DestroyActor(a); ++n; }
        UE_LOGI("[EASTER-DRILL] [H] %zu actor(s) the hunt left destroyed (angry bunny, eggs, ears); notes: %zu now, %zu before", n,
                Live(kNote).size(), g_notesBefore);
    }
}

void OnDisconnect() {
    g_lastSayHost = g_lastSayClient = g_clientSeenMs = g_clientBunnyMs = 0;
    g_animCalled = g_bunnyEnded = g_cleaned = g_clientFin = g_clientEgg = g_clientRepeat = false;
    coop::easter_hunt::SetDrillCallsCount(false);
}

}  // namespace coop::dev::easter_drill
