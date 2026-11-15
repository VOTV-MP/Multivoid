// coop/dev/event_fire_drill.cpp -- see coop/dev/event_fire_drill.h.

#include "coop/dev/event_fire_drill.h"

#include "coop/config/config.h"
#include "coop/dev/event_force.h"
#include "coop/dev/event_trigger.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace coop::dev::event_fire_drill {
namespace {

namespace ET = coop::dev::event_trigger;

struct Plan {
    std::string name;
    uint64_t    watchMs = 60000;
    std::wstring standAt;  // a class whose first actor the host player is moved to before the fire
};

const Plan& ThePlan() {
    static const Plan p = [] {
        Plan out;
        const std::string v = coop::config::ResolveString(::coop::config_registry::rows::event_fire_drill);
        const size_t colon = v.find(':');
        out.name = v.substr(0, colon);
        if (colon != std::string::npos) {
            const long s = std::strtol(v.c_str() + colon + 1, nullptr, 10);
            if (s > 0 && s <= 3600) out.watchMs = static_cast<uint64_t>(s) * 1000;
            const size_t second = v.find(':', colon + 1);
            if (second != std::string::npos) out.standAt.assign(v.begin() + second + 1, v.end());
        }
        return out;
    }();
    return p;
}

constexpr uint64_t kSettleMs = 6000;

uint64_t g_readyMs = 0;
uint64_t g_firedMs = 0;
bool     g_done = false;
bool     g_walked = false;  // the host's player was put at event_fire_walk after the fire
bool     g_forced = false;  // the event's level box, when it has one, was forced after the arm

bool ClientInWorld(coop::net::Session& s) {
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot)
        if (s.IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot) &&
            !coop::join_window_baseline::IsLateWindowOpen(slot))
            return true;
    return false;
}

// `host_place` is "x,y,z": where the host's own player is put once its world is up, before anyone joins. A drill
// whose client joins at the host and then walks starts from there, whatever spot the save left the player on.
struct Place {
    bool             set = false;
    ue_wrap::FVector at{};
};
const Place& ThePlace() {
    static const Place p = [] {
        Place out;
        const std::string v = coop::config::ResolveString(::coop::config_registry::rows::host_place);
        float x = 0, y = 0, z = 0;
        if (std::sscanf(v.c_str(), "%f,%f,%f", &x, &y, &z) == 3) {
            out.set = true;
            out.at = {x, y, z};
        }
        return out;
    }();
    return p;
}
uint64_t g_placeSeenMs = 0;
bool     g_placed = false;

void TickPlace() {
    if (g_placed || !ThePlace().set || !coop::roster::LocalIsHost()) return;
    void* player = coop::players::Registry::Get().Local();
    ue_wrap::FVector here{};
    if (!player || !ue_wrap::engine::TryGetActorLocation(player, here)) return;
    const uint64_t now = coop::net::NowMs();
    if (!g_placeSeenMs) g_placeSeenMs = now;
    if (now - g_placeSeenMs < 3000) return;
    g_placed = true;
    const bool ok = ue_wrap::engine::TeleportTo(player, ThePlace().at, {});
    UE_LOGI("[HOST-PLACE] the host's player moved from (%.0f,%.0f,%.0f) to (%.0f,%.0f,%.0f): %d", here.X, here.Y, here.Z,
            ThePlace().at.X, ThePlace().at.Y, ThePlace().at.Z, ok ? 1 : 0);
}

}  // namespace

bool IsEnabled() { return !ThePlan().name.empty() || ThePlace().set; }

void Tick(coop::net::Session* session) {
    if (session) TickPlace();
    if (ThePlan().name.empty() || !session || g_done || !coop::roster::LocalIsHost() || !session->connected()) return;
    const uint64_t now = coop::net::NowMs();
    if (!g_firedMs) {
        if (!ClientInWorld(*session)) return;
        if (!g_readyMs) g_readyMs = now;
        if (now - g_readyMs < kSettleMs) return;
        const ET::EventInfo* found = nullptr;
        for (const ET::EventInfo& ev : ET::Events())
            if (ThePlan().name == ev.name) { found = &ev; break; }
        if (!found) {
            UE_LOGW("[EVENT-FIRE-DRILL] [H] FAIL no event named '%s' in the dev event table", ThePlan().name.c_str());
            g_done = true;
            return;
        }
        if (!ThePlan().standAt.empty()) {
            void* at = ue_wrap::reflection::FindObjectByClass(ThePlan().standAt.c_str());
            void* player = coop::players::Registry::Get().Local();
            ue_wrap::FVector loc{};
            const bool moved = at && player && ue_wrap::engine::TryGetActorLocation(at, loc) &&
                               ue_wrap::engine::TeleportTo(player, {loc.X, loc.Y, loc.Z + 60.f}, {});
            UE_LOGI("[EVENT-FIRE-DRILL] [H] host player moved to the first '%ls' at (%.0f,%.0f,%.0f): %d",
                    ThePlan().standAt.c_str(), loc.X, loc.Y, loc.Z, moved ? 1 : 0);
        }
        const bool ok = ET::Trigger(*found);
        g_firedMs = now;
        UE_LOGI("[EVENT-FIRE-DRILL] [H] fired '%s' (%d); watching %llu s", found->name, ok ? 1 : 0,
                static_cast<unsigned long long>(ThePlan().watchMs / 1000));
        return;
    }
    // An event that only arms a level box (the obelisk) waits for a player to walk in: the box is forced 3 s on.
    if (!g_forced && now - g_firedMs >= 3000) {
        g_forced = true;
        if (coop::dev::event_force::ForceNow(ThePlan().name.c_str()))
            UE_LOGI("[EVENT-FIRE-DRILL] [H] forced the level box of '%s'", ThePlan().name.c_str());
    }
    // An event whose actor waits for a player to walk into it: the host's player is put at event_fire_walk 5 s on.
    if (!g_walked && now - g_firedMs >= 5000) {
        g_walked = true;
        const std::string v = coop::config::ResolveString(::coop::config_registry::rows::event_fire_walk);
        float x = 0, y = 0, z = 0;
        void* player = coop::players::Registry::Get().Local();
        bool have = std::sscanf(v.c_str(), "%f,%f,%f", &x, &y, &z) == 3;
        // `<class>.<member>`: the place is that component of the class's first live actor (the hole's box).
        const size_t dot = v.find('.');
        if (!have && dot != std::string::npos) {
            const std::wstring cls(v.begin(), v.begin() + dot), member(v.begin() + dot + 1, v.end());
            for (void* obj : ue_wrap::reflection::FindObjectsByClass(cls.c_str())) {
                if (!obj || !ue_wrap::reflection::IsLive(obj)) continue;
                const int32_t off = ue_wrap::reflection::FindPropertyOffset(ue_wrap::reflection::ClassOf(obj), member.c_str());
                void* comp = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(obj) + off) : nullptr;
                ue_wrap::FVector at{};
                if (!comp || !ue_wrap::engine::TryGetComponentLocation(comp, at)) continue;
                x = at.X; y = at.Y; z = at.Z;
                have = true;
                break;
            }
        }
        if (player && have) {
            const bool ok = ue_wrap::engine::TeleportTo(player, {x, y, z}, {});
            UE_LOGI("[EVENT-FIRE-DRILL] [H] host player put at (%.0f,%.0f,%.0f): %d", x, y, z, ok ? 1 : 0);
        }
    }
    if (now - g_firedMs < ThePlan().watchMs) return;
    UE_LOGI("[EVENT-FIRE-DRILL] [H] DONE '%s' after %llu s", ThePlan().name.c_str(),
            static_cast<unsigned long long>((now - g_firedMs) / 1000));
    g_done = true;
}

void OnDisconnect() {
    g_readyMs = 0;
    g_firedMs = 0;
    g_done = false;
    g_forced = false;
    g_walked = false;
}

}  // namespace coop::dev::event_fire_drill
