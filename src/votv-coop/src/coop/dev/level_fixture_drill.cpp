// coop/dev/level_fixture_drill.cpp -- see coop/dev/level_fixture_drill.h.

#include "coop/dev/level_fixture_drill.h"

#include "coop/net/net_clock.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::level_fixture_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

constexpr const wchar_t* kFixtures[] = {L"church_C", L"teleBooth_C", L"sirenPost_C", L"hallwayDoor_C"};
constexpr const wchar_t* kBuriedPath = L"/Game/objects/prop_hexpillar.prop_hexpillar_C";

uint64_t g_clientSeenMs = 0, g_lastSayHost = 0, g_lastSayClient = 0;
bool g_moved = false, g_movedAgain = false, g_triggered = false, g_buriedA = false, g_dugA = false, g_raised = false,
     g_buriedB = false, g_clientDug = false;

// This world's live instances of a class, found as level_fixture_sync's LiveOf finds them: the object index's walk of the class's
// own instances first, the liveness and world tests after it. Never R::FindObjectsByClass, a walk of every object that touched a
// package still loading (a client's run absorbed 16 faults that way); and never every frame.
std::vector<void*> Live(const wchar_t* cls) {
    std::vector<void*> out;
    void* c = ue_wrap::object_index::ClassByName(cls);
    void* world = ue_wrap::world_identity::CurrentWorld();
    if (!c || !world) return out;
    std::vector<void*> found;
    ue_wrap::object_index::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
        if (obj) static_cast<std::vector<void*>*>(ctx)->push_back(obj);
    }, &found);
    for (void* obj : found)
        if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__") && ue_wrap::world_identity::WorldOf(obj) == world)
            out.push_back(obj);
    return out;
}

bool Hidden(void* actor) {
    int32_t off = -1;
    uint8_t mask = 0;
    return R::FindBoolProperty(R::ClassOf(actor), L"bHidden", off, mask) && off >= 0 &&
           (*(static_cast<const uint8_t*>(actor) + off) & mask) != 0;
}

void Say(const char* who, uint64_t sinceMs) {
    for (const wchar_t* cls : kFixtures) {
        const std::vector<void*> live = Live(cls);
        if (live.empty()) {
            UE_LOGI("[GAP-DRILL] [%s] t=%us: no %ls", who, static_cast<unsigned>(sinceMs / 1000), cls);
            continue;
        }
        for (void* a : live) {
            ue_wrap::FVector at{};
            E::TryGetActorLocation(a, at);
            UE_LOGI("[GAP-DRILL] [%s] t=%us: %ls '%ls' at (%.0f, %.0f, %.0f) %s", who, static_cast<unsigned>(sinceMs / 1000), cls,
                    R::ToString(R::NameOf(a)).c_str(), at.X, at.Y, at.Z, Hidden(a) ? "hidden" : "shown");
        }
    }
    float tallest = -1.f;
    const std::vector<void*> pillars = Live(L"hexpillar_C");
    for (void* p : pillars) {
        const int32_t off = R::FindPropertyOffset(R::ClassOf(p), L"a");
        if (off >= 0) {
            const float a = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(p) + off);
            if (a > tallest) tallest = a;
        }
    }
    UE_LOGI("[GAP-DRILL] [%s] t=%us: %zu blood stain(s), %zu buried basalt(s), %zu pillar(s) (tallest a=%.3f)", who,
            static_cast<unsigned>(sinceMs / 1000), Live(L"decal_skeletonBlood_C").size(), Live(L"prop_hexpillar_C").size(),
            pillars.size(), tallest);
}

bool SayDue(uint64_t sinceMs, uint64_t& last) {
    const uint64_t bucket = sinceMs / 5000 + 1;
    if (bucket <= last) return false;
    last = bucket;
    return true;
}

void MoveFixtures(float byCm) {
    for (const wchar_t* cls : kFixtures) {
        if (std::wstring(cls) == L"hallwayDoor_C") continue;
        for (void* a : Live(cls)) {
            ue_wrap::FVector at{};
            if (!E::TryGetActorLocation(a, at)) continue;
            at.X += byCm;
            E::SetActorLocation(a, at);
            E::SetActorHiddenInGame(a, false);
            UE_LOGI("[GAP-DRILL] [H] %ls '%ls' moved %.0f cm to (%.0f, %.0f, %.0f) and shown", cls, R::ToString(R::NameOf(a)).c_str(),
                    byCm, at.X, at.Y, at.Z);
        }
    }
}

void* StandBuried(float sideCm) {
    void* cls = ue_wrap::asset_load::LoadObjectByPath(kBuriedPath);
    void* player = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    void* prop = cls && player && E::TryGetActorLocation(player, at) ? E::SpawnActor(cls, {at.X + 250.f, at.Y + sideCm, at.Z}) : nullptr;
    UE_LOGI("[GAP-DRILL] [H] a buried basalt stood %p (class %p), %.0f cm to the side", prop, cls, sideCm);
    return prop;
}

bool Dig(void* prop) {
    void* fn = prop ? R::FindDispatchFunctionCached(R::ClassOf(prop), L"digUp") : nullptr;
    return fn && R::CallFunction(prop, fn, nullptr);
}

}  // namespace

bool IsDrill(const std::string& name) { return name == "levelGaps"; }

void ClientStep() {
    const uint64_t now = coop::net::NowMs();
    if (!g_clientSeenMs) g_clientSeenMs = now;
    if (SayDue(now - g_clientSeenMs, g_lastSayClient)) Say("C", now - g_clientSeenMs);
    // The second buried basalt is the client's to dig: it is the one that stands while a pillar already does. Looked for once a
    // second, never per frame.
    static uint64_t s_lastDigLook = 0;
    const bool lookNow = now - s_lastDigLook >= 1000;
    if (lookNow) s_lastDigLook = now;
    if (!g_clientDug && lookNow && !Live(L"hexpillar_C").empty()) {
        const std::vector<void*> buried = Live(L"prop_hexpillar_C");
        if (!buried.empty()) {
            g_clientDug = true;
            const bool ran = Dig(buried.front());
            UE_LOGI("[GAP-DRILL] [C] this peer dug its copy of the buried basalt %p: the call returned %d; it holds %zu buried, %zu "
                    "pillar(s) right after", buried.front(), ran ? 1 : 0, Live(L"prop_hexpillar_C").size(), Live(L"hexpillar_C").size());
        }
    }
}

void HostStep(uint64_t since) {
    if (SayDue(since, g_lastSayHost)) Say("H", since);
    if (!g_moved && since >= 10000) {
        g_moved = true;
        MoveFixtures(1500.f);
    }
    if (!g_triggered && since >= 14000) {
        g_triggered = true;
        const std::vector<void*> trig = Live(L"trigger_bloodSkeleton_C");
        void* fn = trig.empty() ? nullptr : R::FindDispatchFunctionCached(R::ClassOf(trig.front()), L"runTrigger");
        ue_wrap::ParamFrame f(fn);
        const bool ran = fn && f.valid() && f.Set<void*>(L"Owner", nullptr) && f.Set<int32_t>(L"Index", 0) && ue_wrap::Call(trig.front(), f);
        UE_LOGI("[GAP-DRILL] [H] the level holds %zu blood-skeleton trigger(s); runTrigger called: %d", trig.size(), ran ? 1 : 0);
    }
    if (!g_buriedA && since >= 18000) {
        g_buriedA = true;
        StandBuried(0.f);
    }
    if (!g_dugA && since >= 22000) {
        g_dugA = true;
        const std::vector<void*> buried = Live(L"prop_hexpillar_C");
        const bool ran = !buried.empty() && Dig(buried.front());
        UE_LOGI("[GAP-DRILL] [H] the host dug its buried basalt: %d; %zu pillar(s) right after", ran ? 1 : 0, Live(L"hexpillar_C").size());
    }
    if (!g_raised && since >= 30000) {
        g_raised = true;
        int n = 0;
        for (void* p : Live(L"hexpillar_C")) {
            const int32_t off = R::FindPropertyOffset(R::ClassOf(p), L"a");
            if (off < 0) continue;
            *reinterpret_cast<float*>(static_cast<uint8_t*>(p) + off) = 0.5f;
            ++n;
        }
        UE_LOGI("[GAP-DRILL] [H] `a` written 0.5 on %d pillar(s)", n);
    }
    if (!g_buriedB && since >= 45000) {
        g_buriedB = true;
        StandBuried(400.f);
    }
    if (!g_movedAgain && since >= 60000) {
        g_movedAgain = true;
        MoveFixtures(1500.f);
    }
}

void OnDisconnect() {
    g_clientSeenMs = g_lastSayHost = g_lastSayClient = 0;
    g_moved = g_movedAgain = g_triggered = g_buriedA = g_dugA = g_raised = g_buriedB = g_clientDug = false;
}

}  // namespace coop::dev::level_fixture_drill
