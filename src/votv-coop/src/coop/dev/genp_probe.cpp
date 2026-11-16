// coop/dev/genp_probe.cpp -- [dev] a player fuels and starts a portable generator (VOTVCOOP_GENP_PROBE=host|client
// names the player).
//
// prop_generatorP_C keeps `fuel` and `isActive` in its own record: gascanFuel (a can's pour) runs fuelUp, which takes
// fuel from the can up to 100; action 8 runs tryActivate, which toggles isActive while there is fuel and powers its two
// outlets; its tick burns fuel while active. HOST, 20 s after a client is connected: spawns a generator 150 uu before
// the player. THE PLAYER: at 25 s takes a gas can (fuel 50) into its hand and pours it through the generator's
// gascanFuel; at 30 s presses action 8. EVERY PEER, every second from 20 s to 45 s: each generator's fuel and isActive
// (`[GENP]`).

#include "coop/dev/genp_probe.h"

#include "coop/config/config.h"
#include "coop/dev/drill_hand.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::genp_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;

uint64_t Now() { return ::GetTickCount64(); }

const std::string& Player() {
    static const std::string f = coop::config::ReadEnv("VOTVCOOP_GENP_PROBE");
    return f;
}

std::vector<void*> All(const wchar_t* cls) {
    std::vector<void*> out;
    if (void* const c = OI::ClassByName(cls))
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    return out;
}

void* Nearest(void* me) {
    ue_wrap::FVector at{};
    if (!me || !E::TryGetActorLocation(me, at)) return nullptr;
    void* best = nullptr;
    float bestD = 1e30f;
    for (void* g : All(L"prop_generatorP_C")) {
        ue_wrap::FVector p{};
        if (!E::TryGetActorLocation(g, p)) continue;
        const float d = (p.X - at.X) * (p.X - at.X) + (p.Y - at.Y) * (p.Y - at.Y) + (p.Z - at.Z) * (p.Z - at.Z);
        if (d < bestD) {
            bestD = d;
            best = g;
        }
    }
    return best;
}

float Float(void* o, const wchar_t* name) {
    const int32_t off = o ? R::FindPropertyOffset(R::ClassOf(o), name) : -1;
    return off >= 0 ? *reinterpret_cast<const float*>(static_cast<const uint8_t*>(o) + off) : -1.f;
}

int Bool(void* o, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!o || !R::FindBoolProperty(R::ClassOf(o), name, off, mask)) return -1;
    return (static_cast<const uint8_t*>(o)[off] & mask) ? 1 : 0;
}

// A call with object parameters and byte parameters set by name, the rest left zero.
bool Call(void* o, const wchar_t* fnName, std::initializer_list<std::pair<const wchar_t*, void*>> objs,
          std::initializer_list<std::pair<const wchar_t*, uint8_t>> bytes) {
    void* const fn = o ? R::FindDispatchFunctionCached(R::ClassOf(o), fnName) : nullptr;
    if (!fn) return false;
    std::vector<uint8_t> frame(static_cast<size_t>(R::FunctionFrameSize(fn)), 0u);
    for (const auto& a : objs) {
        const int32_t off = R::FindParamOffset(fn, a.first);
        if (off < 0) return false;
        std::memcpy(frame.data() + off, &a.second, sizeof(void*));
    }
    for (const auto& b : bytes) {
        const int32_t off = R::FindParamOffset(fn, b.first);
        if (off < 0) return false;
        frame[static_cast<size_t>(off)] = b.second;
    }
    return R::CallFunction(o, fn, frame.data());
}

uint64_t g_startAt = 0, g_nextLog = 0, g_canAt = 0;
int g_step = 0;

}  // namespace

void Tick(coop::net::Session* session) {
    if (Player().empty() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    const bool player = (Player() == "host") == host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[GENP] probe armed role=%s player=%d", host ? "host" : "client", player ? 1 : 0);
    }
    const uint64_t t = Now() - g_startAt;
    void* const me = coop::players::Registry::Get().Local();
    if (host && g_step == 0 && t >= 20000u) {
        void* who = me;
        if (!player) {
            coop::RemotePlayer& rp = coop::puppet_drive::Puppet(1);
            who = rp.valid() ? rp.GetActor() : nullptr;
        }
        ue_wrap::FVector at{};
        if (who && E::TryGetActorLocation(who, at)) {
            g_step = 1;
            const ue_wrap::FVector f = E::GetActorForwardVector(who);
            void* const cls = OI::ClassByName(L"prop_generatorP_C");
            void* const g = cls ? E::SpawnActor(cls, {at.X + f.X * 150.f, at.Y + f.Y * 150.f, at.Z}) : nullptr;
            UE_LOGI("[GENP] host: spawned a generator before the player (ok=%d)", g ? 1 : 0);
        }
    }
    if (!host && g_step == 0 && t >= 20000u) g_step = 1;
    if (player && g_step == 1 && t >= 25000u) {
        g_step = 2;
        // The can in the hand, as a player pours one (the host's judge wants a gas can in the pourer's hand).
        static const float kCanFuel = 50.f;
        coop::dev::drill_hand::TakeIntoHandNow(L"gascan", L"prop_gascan_C", &kCanFuel);
        g_canAt = Now();
    }
    if (player && g_step == 2 && g_canAt && Now() - g_canAt >= 1500u) {
        g_canAt = 0;
        void* const g = Nearest(me);
        void* const can = coop::hand_item::LocalHandActor();
        const bool ran = g && can && Call(g, L"gascanFuel", {{L"gascan", can}, {L"player", me}}, {});
        UE_LOGI("[GENP] player: poured the can in its hand into the generator (ran=%d, can %d)", ran ? 1 : 0, can ? 1 : 0);
    }
    if (player && g_step == 2 && t >= 30000u) {
        g_step = 3;
        void* const g = Nearest(me);
        UE_LOGI("[GENP] player: pressed start (ran=%d)",
                g && Call(g, L"actionOptionIndex", {{L"player", me}}, {{L"action", 8}}) ? 1 : 0);
    }
    if (t >= 20000u && t < 45000u && Now() >= g_nextLog) {
        g_nextLog = Now() + 1000;
        std::string line;
        for (void* g : All(L"prop_generatorP_C")) {
            char b[64];
            std::snprintf(b, sizeof(b), " [fuel %.1f active %d]", Float(g, L"fuel"), Bool(g, L"isActive"));
            line += b;
        }
        UE_LOGI("[GENP] t=%llu generators:%s", t / 1000ull, line.empty() ? " none" : line.c_str());
    }
}

}  // namespace coop::dev::genp_probe
