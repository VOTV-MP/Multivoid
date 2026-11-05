// harness/autotest/autotest_woodchipper.cpp -- TEST-ONLY: the woodchipper's fuel and on/off across two peers.
//
// The host finds the world's woodchipper (or spawns one in front of its player) and names its key. Then: the client
// pours a gas can into it through the chipper's own gascanFuel, switches it on with its button through the chipper's own
// actionOptionIndex and lets it run; the host switches it off, sets its fuel to 30 (a long run's stand-in) and pours a
// can. After each step each peer logs `woodchipper: STATE <step> fuel= active= power=` for the driver to compare; a
// file per step orders the two peers.
// Gated by VOTVCOOP_RUN_WOODCHIPPER; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

constexpr const wchar_t* kChipperPath = L"/Game/objects/woodchipper.woodchipper_C";
constexpr const wchar_t* kGascanPath  = L"/Game/objects/prop_gascan.prop_gascan_C";
constexpr uint8_t kUseAction = 4;   // the button's branch of actionOptionIndex

std::string g_dir;
void* g_chipper = nullptr;   // game thread only

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Write(const char* name, const std::string& text) {
    std::ofstream(g_dir + "\\" + name, std::ios::binary | std::ios::trunc) << text;
}

bool Await(const char* name, std::string* text = nullptr, int seconds = 240) {
    for (int i = 0; i < seconds * 10; ++i) {
        std::ifstream f(g_dir + "\\" + name, std::ios::binary);
        std::string s;
        if (f && std::getline(f, s) && !s.empty()) {
            if (text) *text = s;
            return true;
        }
        ::Sleep(100);
    }
    UE_LOGW("woodchipper: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

template <class T>
bool Field(void* obj, const wchar_t* name, T& out) {
    const int32_t off = obj && R::IsLive(obj) ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    out = *reinterpret_cast<const T*>(static_cast<const uint8_t*>(obj) + off);
    return true;
}

void LogState(const char* step) {
    OnGameThread([&] {
        float fuel = -1.f, power = -1.f;
        uint8_t active = 0xFF;
        Field(g_chipper, L"fuel", fuel);
        Field(g_chipper, L"power", power);
        Field(g_chipper, L"active", active);
        UE_LOGI("woodchipper: STATE %hs fuel=%.2f active=%d power=%.2f", step, fuel,
                active == 0xFF ? -1 : (active != 0 ? 1 : 0), power);
    });
}

// A gas can poured in, as a player's gas can does: a fresh can by the chipper, the chipper's own gascanFuel.
void Pour(const char* who) {
    OnGameThread([&] {
        void* me = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        void* cls = ue_wrap::asset_load::LoadObjectByPath(kGascanPath);
        void* fn = g_chipper ? R::FindFunction(R::ClassOf(g_chipper), L"gascanFuel") : nullptr;
        if (!me || !cls || !fn || !E::TryGetActorLocation(g_chipper, at)) {
            UE_LOGW("woodchipper: %hs cannot pour (player %p, can class %p, gascanFuel %p)", who, me, cls, fn);
            return;
        }
        void* can = E::SpawnActor(cls, {at.X, at.Y, at.Z + 120.f});
        float canFuel = -1.f;
        Field(can, L"fuel", canFuel);
        ue_wrap::ParamFrame f(fn);
        const bool ok = can && f.valid() && f.Set<void*>(L"gascan", can) && f.Set<void*>(L"player", me) &&
                        f.Set<bool>(L"back", false) && ue_wrap::Call(g_chipper, f);
        UE_LOGI("woodchipper: %hs poured a can holding %.2f (ok=%d, fueled=%d)", who, canFuel, ok ? 1 : 0,
                ok && f.Get<bool>(L"fueled") ? 1 : 0);
    });
}

// The on/off button, as a player's press runs it: actionOptionIndex with the use action on button_del.
void Press(const char* who) {
    OnGameThread([&] {
        void* me = coop::players::Registry::Get().Local();
        void* button = nullptr;
        ue_wrap::FVector at{};
        Field(g_chipper, L"button_del", button);
        E::TryGetActorLocation(g_chipper, at);
        const bool ok = me && button &&
                        ue_wrap::prop_use::CallActionOptionIndex(g_chipper, me, button, at, kUseAction, button);
        UE_LOGI("woodchipper: %hs pressed the button (ok=%d)", who, ok ? 1 : 0);
    });
}

void* FindChipper(const std::wstring& key) {
    for (void* o : R::FindObjectsByClass(L"woodchipper_C"))
        if (o && R::IsLive(o) && (key.empty() || ue_wrap::prop::GetInteractableKeyString(o) == key)) {
            const std::wstring k = ue_wrap::prop::GetInteractableKeyString(o);
            if (!k.empty() && k != L"None") return o;
        }
    return nullptr;
}

// The host's copy set to a fuel, as a long run leaves it: the drill's stand-in for burning it down.
void SetFuel(float fuel) {
    OnGameThread([&] {
        const int32_t off = g_chipper ? R::FindPropertyOffset(R::ClassOf(g_chipper), L"fuel") : -1;
        if (off >= 0) std::memcpy(static_cast<uint8_t*>(g_chipper) + off, &fuel, sizeof(fuel));
        UE_LOGI("woodchipper: HOST set its fuel to %.1f", fuel);
    });
}

// One step: the actor does it, then both log; the other waits for the actor's mark and logs after it.
void Step(bool hostActs, const char* name, void (*act)(), int waitMs) {
    const std::string done = std::string(name) + (hostActs ? "_host" : "_client");
    const bool mine = hostActs != IsClientRole();
    if (mine) {
        act();
        ::Sleep(waitMs);
        LogState(name);
        Write(done.c_str(), "x");
        Await((std::string(name) + "_seen").c_str());
    } else {
        if (!Await(done.c_str())) return;
        ::Sleep(1500);
        LogState(name);
        Write((std::string(name) + "_seen").c_str(), "x");
    }
}

void ClientPour() { Pour("CLIENT"); }
void HostPour() { Pour("HOST"); }
void ClientPress() { Press("CLIENT"); }
void HostPress() { Press("HOST"); }
void HostDrain() { SetFuel(30.f); }

void RunSteps() {
    LogState("0-start");
    Step(false, "1-client-poured", &ClientPour, 3000);
    Step(false, "2-client-on", &ClientPress, 10000);
    Step(true, "3-host-off", &HostPress, 3000);
    Step(true, "4-host-fuel-30", &HostDrain, 3000);
    Step(true, "5-host-poured", &HostPour, 3000);
    UE_LOGI("woodchipper: DONE");
}

void RunHost() {
    ::Sleep(20000);
    if (!Await("client_ready")) return;
    bool spawned = false;
    OnGameThread([&] {
        g_chipper = FindChipper(L"");
        if (g_chipper) return;
        void* me = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!me || !E::TryGetActorLocation(me, at)) return;
        const ue_wrap::FVector fwd = E::GetActorForwardVector(me);
        void* cls = ue_wrap::asset_load::LoadObjectByPath(kChipperPath);
        g_chipper = cls ? E::SpawnActor(cls, {at.X + fwd.X * 250.f, at.Y + fwd.Y * 250.f, at.Z + 60.f}) : nullptr;
        spawned = g_chipper != nullptr;
    });
    if (!g_chipper) {
        UE_LOGW("woodchipper: VERDICT ABORT -- no woodchipper in the world and none spawned");
        return;
    }
    std::string key;
    for (int i = 0; i < 100 && key.empty(); ++i) {
        OnGameThread([&] {
            const std::wstring k = ue_wrap::prop::GetInteractableKeyString(g_chipper);
            if (!k.empty() && k != L"None") key.assign(k.begin(), k.end());
        });
        if (key.empty()) ::Sleep(100);
    }
    UE_LOGI("woodchipper: HOST chipper key=%hs (%hs)", key.c_str(), spawned ? "spawned" : "the world's own");
    if (spawned) ::Sleep(5000);   // the spawn reaches the client
    Write("chipper_key", key);
    if (!Await("client_found")) return;
    RunSteps();
}

void RunClient() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(10000);
    Write("client_ready", "x");
    std::string key;
    if (!Await("chipper_key", &key)) return;
    const std::wstring wkey(key.begin(), key.end());
    for (int i = 0; i < 300 && !g_chipper; ++i) {
        OnGameThread([&] { g_chipper = FindChipper(wkey); });
        if (!g_chipper) ::Sleep(100);
    }
    if (!g_chipper) {
        UE_LOGW("woodchipper: VERDICT ABORT -- the client never had the chipper keyed '%hs'", key.c_str());
        return;
    }
    Write("client_found", "x");
    RunSteps();
}

}  // namespace

DWORD WINAPI WoodchipperThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("woodchipper: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (IsClientRole()) RunClient();
    else                RunHost();
    return 0;
}

}  // namespace harness::autotest
