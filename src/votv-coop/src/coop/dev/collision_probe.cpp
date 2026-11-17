// coop/dev/collision_probe.cpp -- see coop/dev/collision_probe.h.

#include "coop/dev/collision_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::collision_probe {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;
namespace E = ue_wrap::engine;

// ECollisionChannel values and names, for the channels a prop's meets.
struct Channel {
    uint8_t     value;
    const char* tag;
    const char* name;
};
constexpr Channel kChannels[] = {{5, "PB", "PhysicsBody"}, {2, "Pawn", "Pawn"}, {1, "WD", "WorldDynamic"},
                                 {0, "WS", "WorldStatic"}, {6, "Veh", "Vehicle"}};

const char* ChannelName(int v) {
    switch (v) {
        case 0: return "WorldStatic";
        case 1: return "WorldDynamic";
        case 2: return "Pawn";
        case 3: return "Visibility";
        case 4: return "Camera";
        case 5: return "PhysicsBody";
        case 6: return "Vehicle";
        case 7: return "Destructible";
        default: return "?";
    }
}
char ResponseChar(int r) { return r == 0 ? 'i' : r == 1 ? 'o' : r == 2 ? 'b' : '?'; }

// A uint8 return of a UPrimitiveComponent function with an optional uint8 argument; -1 when it does not resolve or run.
int CompU8(void* comp, const wchar_t* fn, const wchar_t* argName, int arg) {
    void* f = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), fn) : nullptr;
    ue_wrap::ParamFrame frame(f);
    if (!f || !frame.valid()) return -1;
    if (argName && !frame.Set<uint8_t>(argName, static_cast<uint8_t>(arg))) return -1;
    if (!ue_wrap::Call(comp, frame)) return -1;
    return frame.Get<uint8_t>(L"ReturnValue");
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t ch : w) s.push_back(ch < 128 ? static_cast<char>(ch) : '?');
    return s;
}
std::string NameText(void* obj) { return obj && R::IsLive(obj) ? Narrow(R::ToString(R::NameOf(obj))) : std::string("?"); }

std::string ProfileOf(void* comp) {
    void* f = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"GetCollisionProfileName") : nullptr;
    ue_wrap::ParamFrame frame(f);
    if (!f || !frame.valid() || !ue_wrap::Call(comp, frame)) return "?";
    return Narrow(R::ToString(frame.Get<R::FName>(L"ReturnValue")));
}

bool Overlapping(void* comp, void* other) {
    void* f = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"IsOverlappingComponent") : nullptr;
    ue_wrap::ParamFrame frame(f);
    if (!f || !frame.valid() || !frame.Set<void*>(L"OtherComp", other) || !ue_wrap::Call(comp, frame)) return false;
    return frame.Get<bool>(L"ReturnValue");
}

// The primitive components `actor` owns: its Blueprint chain's component variables, and the native character's capsule and mesh
// (not Blueprint variables). Each once.
std::vector<void*> ComponentsOf(void* actor) {
    std::vector<void*> out;
    if (!actor || !R::IsLive(actor)) return out;
    auto add = [&](void* comp) {
        if (!comp || !R::IsLive(comp) || R::OuterOf(comp) != actor || !E::IsPrimitiveComponent(comp)) return;
        for (void* c : out)
            if (c == comp) return;
        out.push_back(comp);
    };
    for (void* c = R::ClassOf(actor); c; c = R::SuperStructOf(c)) {
        for (const R::StructFieldInfo& f : R::EnumerateStructFields(c)) {
            if (!(f.flags & P::cpf::InstancedReference) || f.size != static_cast<int32_t>(sizeof(void*))) continue;
            add(*reinterpret_cast<void* const*>(static_cast<const char*>(actor) + f.offset));
        }
    }
    add(*reinterpret_cast<void* const*>(static_cast<const char*>(actor) + P::off::ACharacter_CapsuleComponent));
    add(*reinterpret_cast<void* const*>(static_cast<const char*>(actor) + P::off::ACharacter_Mesh));
    return out;
}

struct Seen {
    void*    actor = nullptr;
    uint64_t firstMs = 0;
    bool     later = false;
};
std::vector<Seen> g_seen;
bool g_worldReadySaid = false;

void Watch(const char* who, void* actor) {
    if (!actor || !R::IsLive(actor)) return;
    const uint64_t now = ::GetTickCount64();
    for (Seen& s : g_seen) {
        if (s.actor != actor) continue;
        if (!s.later && now - s.firstMs >= 2000) {
            s.later = true;
            std::string label = std::string(who) + " +2s";
            LogActor(label.c_str(), actor);
        }
        return;
    }
    g_seen.push_back(Seen{actor, now, false});
    std::string label = std::string(who) + " first seen";
    LogActor(label.c_str(), actor);
}

}  // namespace

bool IsEnabled() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::collision_probe);
    return on;
}

void Tick() {
    if (!IsEnabled()) return;
    coop::players::Registry& reg = coop::players::Registry::Get();
    void* me = reg.Local();
    Watch("local pawn", me);
    if (me && !g_worldReadySaid && coop::net_pump::HasAnnouncedWorldReady()) {
        g_worldReadySaid = true;
        LogActor("local pawn world-ready", me);
    }
    for (uint8_t slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        coop::RemotePlayer* rp = reg.Puppet(slot);
        if (rp && rp->valid()) Watch("puppet", rp->GetActor());
    }
}

void LogActor(const char* label, void* actor) {
    if (!IsEnabled() || !actor) return;
    const std::vector<void*> comps = ComponentsOf(actor);
    UE_LOGI("[COLLISION-PROBE] %s %s@%p: %zu primitive component(s)", label, Narrow(R::ClassNameOf(actor)).c_str(), actor,
            comps.size());
    for (void* comp : comps) {
        char resp[160];
        int used = 0;
        for (const Channel& ch : kChannels)
            used += std::snprintf(resp + used, sizeof(resp) - static_cast<size_t>(used), "%s=%c ", ch.tag,
                                  ResponseChar(CompU8(comp, L"GetCollisionResponseToChannel", L"Channel", ch.value)));
        UE_LOGI("[COLLISION-PROBE] %s %s@%p comp=%s class=%s enabled=%d type=%s profile=%s %s", label,
                Narrow(R::ClassNameOf(actor)).c_str(), actor, NameText(comp).c_str(), Narrow(R::ClassNameOf(comp)).c_str(),
                CompU8(comp, L"GetCollisionEnabled", nullptr, 0), ChannelName(CompU8(comp, L"GetCollisionObjectType", nullptr, 0)),
                ProfileOf(comp).c_str(), resp);
    }
}

void LogPair(const char* label, void* pawn, void* propRoot) {
    if (!IsEnabled() || !pawn || !propRoot) return;
    const int propType = CompU8(propRoot, L"GetCollisionObjectType", nullptr, 0);
    const int propEnabled = CompU8(propRoot, L"GetCollisionEnabled", nullptr, 0);
    for (void* comp : ComponentsOf(pawn)) {
        const int type = CompU8(comp, L"GetCollisionObjectType", nullptr, 0);
        const int enabled = CompU8(comp, L"GetCollisionEnabled", nullptr, 0);
        const int pawnToProp = CompU8(comp, L"GetCollisionResponseToChannel", L"Channel", propType);
        const int propToPawn = CompU8(propRoot, L"GetCollisionResponseToChannel", L"Channel", type);
        const int mutual = pawnToProp < 0 || propToPawn < 0 ? -1 : pawnToProp < propToPawn ? pawnToProp : propToPawn;
        const bool physics = enabled == 2 || enabled == 3;
        const bool propPhysics = propEnabled == 2 || propEnabled == 3;
        ue_wrap::FVector at{};
        const float dist = E::TryGetComponentLocation(comp, at) ? E::GetComponentDistanceToCollision(propRoot, at) : -1.f;
        UE_LOGI("[COLLISION-PROBE] %s pair pawn comp=%s (%s, enabled %d) vs prop %s (%s, enabled %d): pawn-to-prop=%c prop-to-pawn=%c "
                "mutual=%s collides=%d overlapping=%d dist=%.1f cm", label, NameText(comp).c_str(), ChannelName(type), enabled,
                NameText(propRoot).c_str(), ChannelName(propType), propEnabled, ResponseChar(pawnToProp), ResponseChar(propToPawn),
                mutual == 2 ? "block" : mutual == 1 ? "overlap" : mutual == 0 ? "ignore" : "?",
                (mutual == 2 && physics && propPhysics) ? 1 : 0, Overlapping(comp, propRoot) ? 1 : 0, static_cast<double>(dist));
    }
}

}  // namespace coop::dev::collision_probe
