// coop/dev/dingus_probe.cpp -- [dev] the dingus (Maxwell) found, pocketed and dropped (VOTVCOOP_DINGUS_PROBE=host|client
// names the finder), for Ghezra/Multivoid issue 37.
//
// prop_dingus_C plays its music on `ws1`, an audio component active from its spawn, until it is found: the first grab
// (playerGrabbed -> Touch) sets `isused` and deactivates ws1, and its loadData, which reads `isused` from the save
// record, deactivates ws1 when it is set. HOST, once the finder's player stands, 20 s after a client is connected:
// spawns a dingus 100 uu before the finder. THE FINDER: from 25 s grabs it through the use key's own grab chain (the
// director's aimed grab) and at 29 s lets go (dropGrabObject); at 33 s takes it into its inventory through the player's
// own collectObject; at 38 s drops it through playerContainer.getObject, as the inventory's drop button does. EVERY
// PEER, every second from 20 s to 60 s: each dingus, its key, `isused` and whether ws1 is active (`[DINGUS]`).

#include "coop/dev/dingus_probe.h"

#include "coop/config/config.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::dingus_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;

constexpr const wchar_t* kClass = L"prop_dingus_C";
constexpr const wchar_t* kPath  = L"/Game/objects/prop_dingus.prop_dingus_C";

uint64_t Now() { return ::GetTickCount64(); }

const std::string& Finder() {
    static const std::string f = coop::config::ReadEnv("VOTVCOOP_DINGUS_PROBE");
    return f;
}

void* Obj(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const char*>(obj) + off) : nullptr;
}

int Bool(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return -1;
    return (static_cast<const uint8_t*>(obj)[off] & mask) ? 1 : 0;
}

std::vector<void*> All() {
    std::vector<void*> out;
    if (void* const cls = OI::ClassByName(kClass))
        OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
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
    for (void* d : All()) {
        ue_wrap::FVector p{};
        if (!E::TryGetActorLocation(d, p)) continue;
        const float dd = (p.X - at.X) * (p.X - at.X) + (p.Y - at.Y) * (p.Y - at.Y) + (p.Z - at.Z) * (p.Z - at.Z);
        if (dd < bestD) { bestD = dd; best = d; }
    }
    return best;
}

std::wstring g_key;   // the dingus's key, as it went into the inventory
uint64_t g_startAt = 0, g_reportAt = 0;
int g_step = 0;
coop::director::AimedGrab* g_grab = nullptr;

}  // namespace

void Tick(coop::net::Session* session) {
    if (Finder().empty() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    const bool finder = (Finder() == "host") == host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[DINGUS] probe armed role=%s finder=%d", host ? "host" : "client", finder ? 1 : 0);
    }
    const uint64_t t = Now() - g_startAt;
    void* const me = coop::players::Registry::Get().Local();
    if (host && g_step == 0 && t >= 20000u) {
        void* who = me;
        if (Finder() == "client") {
            coop::RemotePlayer& rp = coop::puppet_drive::Puppet(1);
            who = rp.valid() ? rp.GetActor() : nullptr;
        }
        ue_wrap::FVector at{};
        if (who && E::TryGetActorLocation(who, at)) {
            g_step = 1;
            const ue_wrap::FVector f = E::GetActorForwardVector(who);
            void* cls = OI::ClassByName(kClass);
            if (!cls) cls = ue_wrap::asset_load::LoadObjectByPath(kPath);
            void* const d = cls ? E::SpawnActor(cls, {at.X + f.X * 100.f, at.Y + f.Y * 100.f, at.Z}) : nullptr;
            UE_LOGI("[DINGUS] host: spawned a dingus before the finder (ok=%d)", d ? 1 : 0);
        }
    }
    if (!host && g_step == 0 && t >= 20000u) g_step = 1;
    if (finder && g_step == 1 && t >= 25000u) {
        void* const d = Nearest(me);
        if (!g_grab && d && me) g_grab = new coop::director::AimedGrab(me, d);
        if (g_grab) {
            const coop::director::GrabState g = g_grab->Tick();
            if (g != coop::director::GrabState::Working) {
                UE_LOGI("[DINGUS] finder: grabbed the dingus (%hs%hs)", g == coop::director::GrabState::Grabbed ? "ok" : "failed: ",
                        g == coop::director::GrabState::Grabbed ? "" : g_grab->Why());
                delete g_grab;
                g_grab = nullptr;
                g_step = 2;
            }
        }
    }
    if (finder && g_step == 2 && t >= 29000u) {
        g_step = 3;
        UE_LOGI("[DINGUS] finder: let go (ran=%d)",
                me && R::CallFunction(me, R::FindDispatchFunctionCached(R::ClassOf(me), L"dropGrabObject"), nullptr) ? 1 : 0);
    }
    if (finder && g_step == 3 && t >= 33000u) {
        g_step = 4;
        void* const d = Nearest(me);
        g_key = d ? ue_wrap::prop::GetInteractableKeyString(d) : L"";
        const int32_t laOff = me ? R::FindPropertyOffset(R::ClassOf(me), L"lookAtActor") : -1;
        const int32_t lcOff = me ? R::FindPropertyOffset(R::ClassOf(me), L"lookAtComponent") : -1;
        if (laOff >= 0) *reinterpret_cast<void**>(static_cast<char*>(me) + laOff) = d;
        if (lcOff >= 0) *reinterpret_cast<void**>(static_cast<char*>(me) + lcOff) = d ? Obj(d, L"StaticMesh") : nullptr;
        const bool ran = d && R::CallFunction(me, R::FindDispatchFunctionCached(R::ClassOf(me), L"collectObject"), nullptr);
        UE_LOGI("[DINGUS] finder: collectObject on '%ls' (ran=%d)", g_key.c_str(), ran ? 1 : 0);
    }
    if (finder && g_step == 4 && t >= 38000u) {
        g_step = 5;
        ue_wrap::inventory::LivePersonalStore store;
        int32_t index = -1;
        if (ue_wrap::inventory::ReadLivePersonalStore(store))
            for (size_t i = 0; i < store.records.size(); ++i)
                if (store.records[i].className == kClass) index = static_cast<int32_t>(i);
        void* const container = Obj(ue_wrap::world_singleton::Gamemode(), L"playerContainer");
        void* out = nullptr;
        if (index >= 0 && container) {
            ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(container), L"getObject"));
            const ue_wrap::FVector none{};
            if (f.valid() && f.Set<int32_t>(L"index", index) && f.Set(L"customLoc", false) &&
                f.SetRaw(L"loc", &none, sizeof(none)) && ue_wrap::Call(container, f))
                out = f.Get<void*>(L"OutputPin");
        }
        UE_LOGI("[DINGUS] finder: dropped it from the inventory (record %d, spawned=%d)", index, out ? 1 : 0);
    }
    if (t < 20000u || t > 60000u || Now() < g_reportAt) return;
    g_reportAt = Now() + 1000;
    for (void* d : All()) {
        void* const ws1 = Obj(d, L"ws1");
        UE_LOGI("[DINGUS] dingus key=%ls isused=%d ws1_active=%d", ue_wrap::prop::GetInteractableKeyString(d).c_str(),
                Bool(d, L"isused"), Bool(ws1, L"bIsActive"));
    }
}

}  // namespace coop::dev::dingus_probe
