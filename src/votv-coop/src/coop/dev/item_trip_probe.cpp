// coop/dev/item_trip_probe.cpp -- [dev] an item's state across a trip into a player's inventory and back.
//
// HOST, 20 s after a client is connected: spawns the item (a canned food, or a fire extinguisher) 1.2 m before its
// player. THE ACTOR (VOTVCOOP_ITEM_TRIP_PROBE, client or host), from 24 s: uses it -- a can's own Open, then its eat;
// a disc's uses spent to 20 on its copy -- then at 32 s takes it into its hand through the player's own `Hold Object`;
// an extinguisher then sprays from 34 s to 40 s through its own LMB; at 38 s (44 s for an extinguisher) it drops what its
// hand holds through simulateDrop. BOTH, every second from 20 s to 55 s: every item of the class, its key and state, and
// what this player's hand holds (`[TRIP]`).

#include "coop/dev/item_trip_probe.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::item_trip_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;

uint64_t Now() { return ::GetTickCount64(); }

const std::string& Actor() {
    static const std::string a = coop::config::ReadEnv("VOTVCOOP_ITEM_TRIP_PROBE");
    return a;
}

const std::string& Item() {
    static const std::string i = coop::config::ReadEnv("VOTVCOOP_ITEM_TRIP_ITEM");
    return i;
}
bool Can() { return Item() != "ext" && Item() != "disc"; }
bool Disc() { return Item() == "disc"; }

const wchar_t* ClassName() { return Can() ? L"prop_cannedFood_0_C" : Disc() ? L"prop_floppyDisc_C" : L"prop_fireExt_C"; }
const wchar_t* ClassPath() {
    return Can() ? L"/Game/objects/prop_cannedFood_0.prop_cannedFood_0_C"
                 : Disc() ? L"/Game/objects/prop_floppyDisc.prop_floppyDisc_C" : L"/Game/objects/prop_fireExt.prop_fireExt_C";
}

int Bool(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return -1;
    return (static_cast<const uint8_t*>(obj)[off] & mask) ? 1 : 0;
}

void* Obj(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const char*>(obj) + off) : nullptr;
}

int32_t* IntAt(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? reinterpret_cast<int32_t*>(static_cast<char*>(obj) + off) : nullptr;
}

float* FloatAt(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? reinterpret_cast<float*>(static_cast<char*>(obj) + off) : nullptr;
}

std::vector<void*> Items() {
    std::vector<void*> out;
    void* const cls = OI::ClassByName(ClassName());
    if (cls)
        OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    return out;
}

std::string State(void* item) {
    char buf[96];
    if (Can()) {
        const int32_t* foods = IntAt(item, L"foods");
        snprintf(buf, sizeof(buf), "opened=%d foods=%d", Bool(item, L"opened"), foods ? *foods : -1);
    } else if (Disc()) {
        const int32_t* rw = IntAt(item, L"readWrites");
        snprintf(buf, sizeof(buf), "readWrites=%d", rw ? *rw : -1);
    } else {
        const float* life = FloatAt(item, L"life");
        snprintf(buf, sizeof(buf), "life=%.3f active=%d", life ? *life : -1.f, Bool(item, L"active"));
    }
    return buf;
}

bool Call(void* obj, const wchar_t* fn, void* player) {
    void* const f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    ue_wrap::ParamFrame p(f);
    if (!p.valid()) return false;
    if (player && R::FindParamOffset(f, L"player") >= 0) p.Set<void*>(L"player", player);
    return ue_wrap::Call(obj, p);
}

void* Nearest() {
    void* const me = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    if (!me || !E::TryGetActorLocation(me, at)) return nullptr;
    void* best = nullptr;
    float bestD = 1e30f;
    for (void* it : Items()) {
        ue_wrap::FVector p{};
        if (!E::TryGetActorLocation(it, p)) continue;
        const float d = (p.X - at.X) * (p.X - at.X) + (p.Y - at.Y) * (p.Y - at.Y) + (p.Z - at.Z) * (p.Z - at.Z);
        if (d < bestD) { bestD = d; best = it; }
    }
    return best;
}

void Report() {
    void* const me = coop::players::Registry::Get().Local();
    void* const hand = Obj(me, L"holding_actor");
    for (void* it : Items()) {
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(it);
        UE_LOGI("[TRIP] item key=%ls %hs%hs", key.c_str(), State(it).c_str(), it == hand ? " (in this hand)" : "");
    }
}

uint64_t g_startAt = 0, g_reportAt = 0;
int g_step = 0;

}  // namespace

void Tick(coop::net::Session* session) {
    if (Actor().empty() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    const bool acting = (Actor() == "host") == host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[TRIP] probe armed role=%s acting=%d item=%ls", host ? "host" : "client", acting ? 1 : 0, ClassName());
    }
    const uint64_t t = Now() - g_startAt;
    void* const me = coop::players::Registry::Get().Local();
    if (g_step == 0 && t >= 20000u) {
        g_step = 1;
        if (host) {
            void* cls = OI::ClassByName(ClassName());
            if (!cls) cls = ue_wrap::asset_load::LoadObjectByPath(ClassPath());
            ue_wrap::FVector at{};
            void* const item = cls && me && E::TryGetActorLocation(me, at) ? E::SpawnActor(cls, {at.X + 120.f, at.Y, at.Z + 40.f})
                                                                        : nullptr;
            UE_LOGI("[TRIP] host: spawned the item (ok=%d)", item ? 1 : 0);
        }
    }
    if (acting && g_step == 1 && t >= 24000u) {
        g_step = 2;
        void* const item = Nearest();
        if (Can()) {
            UE_LOGI("[TRIP] actor: opened the can (ran=%d)", Call(item, L"Open", me) ? 1 : 0);
        } else if (Disc()) {
            if (int32_t* rw = IntAt(item, L"readWrites")) *rw = 20;
            UE_LOGI("[TRIP] actor: spent the disc's uses to 20 on this copy");
        }
    }
    if (acting && g_step == 2 && t >= 27000u) {
        g_step = 3;
        if (Can()) UE_LOGI("[TRIP] actor: ate from the can (ran=%d)", Call(Nearest(), L"eat", me) ? 1 : 0);
    }
    if (acting && g_step == 3 && t >= 32000u) {
        g_step = 4;
        void* const item = Nearest();
        ue_wrap::ParamFrame f(me ? R::FindDispatchFunctionCached(R::ClassOf(me), L"Hold Object") : nullptr);
        const bool ran = item && f.valid() && f.Set<bool>(L"useHold", true) && f.Set<void*>(L"manual", item) &&
                         ue_wrap::Call(me, f);
        UE_LOGI("[TRIP] actor: took the item into the hand through Hold Object (ran=%d collected=%d, hand holds %ls)",
                ran ? 1 : 0, ran && f.Get<bool>(L"collected") ? 1 : 0,
                Obj(me, L"holding_actor") ? R::ClassNameOf(Obj(me, L"holding_actor")).c_str() : L"nothing");
    }
    // An extinguisher sprays from the hand for six seconds through its own LMB, as the player's press and release do.
    if (acting && !Can() && !Disc() && g_step == 4 && t >= 34000u && t < 40000u) {
        static bool s_pressed = false;
        void* const hand = Obj(me, L"holding_actor");
        if (!s_pressed && hand) {
            s_pressed = true;
            UE_LOGI("[TRIP] actor: pressed the hand's LMB (ran=%d) %hs", Call(hand, L"playerHandUse_LMB", me) ? 1 : 0,
                    State(hand).c_str());
        }
    }
    if (acting && !Can() && !Disc() && g_step == 4 && t >= 40000u) {
        g_step = 40;
        void* const hand = Obj(me, L"holding_actor");
        UE_LOGI("[TRIP] actor: released the hand's LMB (ran=%d) %hs", Call(hand, L"playerHandRelease_LMB", me) ? 1 : 0,
                hand ? State(hand).c_str() : "-");
    }
    if (acting && g_step == 40 && t >= 44000u) g_step = 4;
    if (acting && g_step == 4 && t >= (Can() || Disc() ? 38000u : 44000u)) {
        g_step = 5;
        ue_wrap::ParamFrame f(me ? R::FindDispatchFunctionCached(R::ClassOf(me), L"simulateDrop") : nullptr);
        // As the drop key's own call does: not into the inventory (dontCollect), not placed.
        const bool ran = f.valid() && f.Set<bool>(L"dontCollect", true) && ue_wrap::Call(me, f);
        UE_LOGI("[TRIP] actor: dropped what the hand holds (ran=%d)", ran ? 1 : 0);
    }
    if (t < 20000u || t > 75000u || Now() < g_reportAt) return;
    g_reportAt = Now() + 1000;
    Report();
}

}  // namespace coop::dev::item_trip_probe
