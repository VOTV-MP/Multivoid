// harness/autotest/autotest_atv_host.cpp -- TEST-ONLY: what of an ATV the host owns, step by step on two peers.
//
// The host names the world's ATV. The client then refuels it with a gas can and repairs it with a toolbox through the
// ATV's own gascanFuel and toolboxFix (after the host has drained it), sits in it and presses its lights, brake, turbo
// and horn through the ATV's own input events, gets off, and fits a module through the ATV's own playerUsedOn with the
// module in its hand and takes it off through the ATV's own upgradeTake. After each step each peer logs `atv-host: STATE <step>`:
// the fuel, health, modules, lights, brake, turbo, the seat and the module props of the drill's class it holds, and the
// horns this peer heard; a file per step orders the two peers. Gated by VOTVCOOP_RUN_ATV_HOST; the folder is
// VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/interactables/atv_host_sync.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflected_offset.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace A = ue_wrap::atv;

constexpr const wchar_t* kGascanPath  = L"/Game/objects/prop_gascan.prop_gascan_C";
constexpr const wchar_t* kToolboxPath = L"/Game/objects/prop_toolbox.prop_toolbox_C";
constexpr const wchar_t* kModulePath  = L"/Game/objects/prop_atvUpgrade_bigLights.prop_atvUpgrade_bigLights_C";
constexpr const wchar_t* kModuleClass = L"prop_atvUpgrade_bigLights_C";

std::string g_dir;
void* g_atv = nullptr;   // game thread only

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Write(const std::string& name, const std::string& text) {
    std::ofstream(g_dir + "\\" + name, std::ios::binary | std::ios::trunc) << text;
}

bool Await(const std::string& name, std::string* text = nullptr, int seconds = 240) {
    for (int i = 0; i < seconds * 10; ++i) {
        std::ifstream f(g_dir + "\\" + name, std::ios::binary);
        std::string s;
        if (f && std::getline(f, s) && !s.empty()) {
            if (text) *text = s;
            return true;
        }
        ::Sleep(100);
    }
    UE_LOGW("atv-host: VERDICT ABORT -- no '%hs' from the other peer", name.c_str());
    return false;
}

template <class T>
bool Field(void* obj, const wchar_t* name, T& out) {
    const int32_t off = obj && R::IsLive(obj) ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(obj) + off, sizeof(T));
    return true;
}

template <class T>
void SetField(void* obj, const wchar_t* name, const T& v) {
    const int32_t off = obj && R::IsLive(obj) ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off >= 0) std::memcpy(static_cast<uint8_t*>(obj) + off, &v, sizeof(T));
}

std::string Modules(void* atv) {
    const int32_t off = atv ? R::FindPropertyOffset(R::ClassOf(atv), L"Modules") : -1;
    if (off < 0) return "?";
    const uint8_t* base = static_cast<const uint8_t*>(atv) + off;
    const uint8_t* data = *reinterpret_cast<uint8_t* const*>(base);
    const int32_t num = *reinterpret_cast<const int32_t*>(base + 8);
    std::string s = "[";
    for (int32_t i = 0; i < num && data; ++i) s += (i ? "," : "") + std::to_string(data[i]);
    return s + "]";
}

int CountClass(const wchar_t* cls) {
    int n = 0;
    for (void* o : R::FindObjectsByClass(cls))
        if (o && R::IsLive(o)) ++n;
    return n;
}

// The ATV's radio: the child actor of its `radio` component.
void* Radio() {
    void* component = nullptr;
    Field(g_atv, L"radio", component);
    void* radio = nullptr;
    if (component && R::IsLive(component)) Field(component, L"ChildActor", radio);
    return radio && R::IsLive(radio) ? radio : nullptr;
}

// The ATV radio's own media player, read raw (its URL's file name and whether it plays), the same on every build.
std::string RadioText() {
    void* radio = Radio();
    if (!radio) return "none";
    void* player = nullptr;
    Field(radio, L"mediaPlayer", player);
    if (!player || !R::IsLive(player)) return "off";
    bool playing = false;
    std::wstring url;
    if (void* f = R::FindDispatchFunctionCached(R::ClassOf(player), L"IsPlaying")) {
        ue_wrap::ParamFrame pf(f);
        if (pf.valid() && ue_wrap::Call(player, pf)) playing = pf.Get<bool>(L"ReturnValue");
    }
    if (void* f = R::FindDispatchFunctionCached(R::ClassOf(player), L"GetUrl")) {
        ue_wrap::ParamFrame pf(f);
        R::FString fs{};
        if (pf.valid() && ue_wrap::Call(player, pf) && pf.GetRaw(L"ReturnValue", &fs, sizeof(fs)) && fs.Data)
            url.assign(fs.Data);
    }
    const size_t slash = url.find_last_of(L"/\\");
    const std::wstring name = slash == std::wstring::npos ? url : url.substr(slash + 1);
    return std::string(name.begin(), name.end()) + (playing ? "/playing" : "/stopped");
}

void LogState(const std::string& step) {
    OnGameThread([&] {
        if (!g_atv || !R::IsLive(g_atv)) {
            UE_LOGW("atv-host: STATE %hs -- no ATV", step.c_str());
            return;
        }
        bool lights = false, turbo = false, brokenn = false, empty = false;
        Field(g_atv, L"lights", lights);
        Field(g_atv, L"turbo", turbo);
        Field(g_atv, L"brokenn", brokenn);
        Field(g_atv, L"Empty", empty);
        UE_LOGI("atv-host: STATE %hs fuel=%.1f health=%.1f empty=%d broken=%d modules=%hs lights=%d brake=%d turbo=%d "
                "driven=%d moduleProps=%d horns=%u radio=%hs", step.c_str(), A::GetFuel(g_atv), A::GetHealth(g_atv), empty ? 1 : 0,
                brokenn ? 1 : 0, Modules(g_atv).c_str(), lights ? 1 : 0, A::GetBrake(g_atv) ? 1 : 0, turbo ? 1 : 0,
                A::IsDriven(g_atv) ? 1 : 0, CountClass(kModuleClass),
                coop::atv_host_sync::HornsPlayed(), RadioText().c_str());
    });
}

void* Me() { return coop::players::Registry::Get().Local(); }

void* SpawnNear(const wchar_t* path, float up) {
    void* cls = ue_wrap::asset_load::LoadObjectByPath(path);
    ue_wrap::FVector at{};
    void* me = Me();
    if (!cls || !me || !E::TryGetActorLocation(me, at)) return nullptr;
    return E::SpawnActor(cls, {at.X, at.Y, at.Z + up});
}

bool CallOn(void* obj, const wchar_t* fn, void (*fill)(ue_wrap::ParamFrame&, void*), void* ctx) {
    void* f = obj ? R::FindFunction(R::ClassOf(obj), fn) : nullptr;
    if (!f) return false;
    ue_wrap::ParamFrame pf(f);
    if (!pf.valid()) return false;
    if (fill) fill(pf, ctx);
    return ue_wrap::Call(obj, pf);
}

// ---- the actions ----

void HostFuel20() {
    OnGameThread([] {
        SetField(g_atv, L"fuel", 20.f);
        UE_LOGI("atv-host: HOST set its fuel to 20");
    });
}

void HostHealth30() {
    OnGameThread([] {
        SetField(g_atv, L"health", 30.f);
        const bool ok = CallOn(g_atv, L"updHealth", nullptr, nullptr);
        UE_LOGI("atv-host: HOST set its health to 30 (updHealth ok=%d)", ok ? 1 : 0);
    });
}

void ClientPour() {
    OnGameThread([] {
        void* can = SpawnNear(kGascanPath, 150.f);
        void* me = Me();
        bool fueled = false;
        void* f = R::FindFunction(R::ClassOf(g_atv), L"gascanFuel");
        ue_wrap::ParamFrame pf(f);
        const bool ok = can && me && f && pf.valid() && pf.Set<void*>(L"gascan", can) && pf.Set<void*>(L"player", me) &&
                        pf.Set<bool>(L"back", false) && ue_wrap::Call(g_atv, pf);
        if (ok) fueled = pf.Get<bool>(L"fueled");
        UE_LOGI("atv-host: CLIENT poured a can (ok=%d fueled=%d)", ok ? 1 : 0, fueled ? 1 : 0);
    });
}

void ClientRepair() {
    OnGameThread([] {
        void* box = SpawnNear(kToolboxPath, 150.f);
        void* f = R::FindFunction(R::ClassOf(g_atv), L"toolboxFix");
        ue_wrap::ParamFrame pf(f);
        const bool ok = box && f && pf.valid() && pf.Set<void*>(L"toolbox", box) && ue_wrap::Call(g_atv, pf);
        UE_LOGI("atv-host: CLIENT repaired with a toolbox (ok=%d fixed=%d)", ok ? 1 : 0,
                ok && pf.Get<bool>(L"return") ? 1 : 0);
        if (box) E::DestroyActor(box);
    });
}

void ClientFitModule() {
    OnGameThread([] {
        void* module = SpawnNear(kModulePath, 120.f);
        void* me = Me();
        bool collected = false;
        const bool held = module && me && E::CallMainPlayerHoldObject(me, module, collected) && collected;
        void* hand = nullptr;
        Field(me, L"holding_actor", hand);
        ue_wrap::FVector at{};
        E::TryGetActorLocation(g_atv, at);
        const bool ok = held && hand &&
                        ue_wrap::prop_use::CallPlayerUsedOn(g_atv, me, nullptr, at, nullptr, hand, R::FName{});
        UE_LOGI("atv-host: CLIENT fitted a module (spawned=%d held=%d hand=%ls ok=%d)", module ? 1 : 0, held ? 1 : 0,
                hand ? R::ClassNameOf(hand).c_str() : L"none", ok ? 1 : 0);
    });
}

void ClientTakeModule() {
    OnGameThread([] {
        // The module's name, from upgradesNames, which runs parallel to Modules.
        const int32_t mOff = R::FindPropertyOffset(R::ClassOf(g_atv), L"Modules");
        const int32_t nOff = R::FindPropertyOffset(R::ClassOf(g_atv), L"upgradesNames");
        if (mOff < 0 || nOff < 0) {
            UE_LOGW("atv-host: CLIENT cannot read Modules or upgradesNames");
            return;
        }
        const uint8_t* mBase = static_cast<const uint8_t*>(g_atv) + mOff;
        const int32_t mNum = *reinterpret_cast<const int32_t*>(mBase + 8);
        const uint8_t* nBase = static_cast<const uint8_t*>(g_atv) + nOff;
        const R::FName* names = *reinterpret_cast<R::FName* const*>(nBase);
        const int32_t nNum = *reinterpret_cast<const int32_t*>(nBase + 8);
        if (mNum < 1 || nNum < mNum || !names) {
            UE_LOGW("atv-host: CLIENT has no module to take off (modules %d, names %d)", mNum, nNum);
            return;
        }
        R::FName name = names[mNum - 1];   // the last fitted
        void* f = R::FindFunction(R::ClassOf(g_atv), L"upgradeTake");
        ue_wrap::ParamFrame pf(f);
        const bool ok = f && pf.valid() && pf.Set<R::FName>(L"item", name) && ue_wrap::Call(g_atv, pf);
        void* hand = nullptr;
        Field(Me(), L"holding_actor", hand);
        UE_LOGI("atv-host: CLIENT took a module off (ok=%d, hand=%ls)", ok ? 1 : 0,
                hand ? R::ClassNameOf(hand).c_str() : L"none");
    });
}

void ClientSit() {
    for (int i = 0; i < 6; ++i) {
        bool driven = false;
        OnGameThread([&] {
            if (A::IsDriven(g_atv) && A::GetOccupantPlayer(g_atv) == Me()) {
                driven = true;
                return;
            }
            std::wstring verb = L"sit";
            R::FString fs{verb.data(), static_cast<int32_t>(verb.size()) + 1, static_cast<int32_t>(verb.size()) + 1};
            void* f = R::FindFunction(R::ClassOf(g_atv), L"actionName");
            ue_wrap::ParamFrame pf(f);
            if (f && pf.valid() && pf.Set<void*>(L"player", Me()) && pf.Set<R::FString>(L"name", fs)) ue_wrap::Call(g_atv, pf);
            driven = A::IsDriven(g_atv) && A::GetOccupantPlayer(g_atv) == Me();
        });
        UE_LOGI("atv-host: CLIENT sit try %d -> driven=%d", i + 1, driven ? 1 : 0);
        if (driven) return;
        ::Sleep(2000);
    }
}

void Input(const wchar_t* event) {
    OnGameThread([&] {
        const bool ok = CallOn(g_atv, event, nullptr, nullptr);
        UE_LOGI("atv-host: CLIENT %ls (ok=%d)", event, ok ? 1 : 0);
    });
}

void ClientLights() { Input(L"InpActEvt_atv_lights_K2Node_InputActionEvent_3"); }
void ClientBrake() { Input(L"InpActEvt_atv_brake_K2Node_InputActionEvent_22"); }
void ClientTurbo() { Input(L"InpActEvt_atv_turbo_K2Node_InputActionEvent_12"); }
void ClientHonk() {
    Input(L"InpActEvt_atv_honk_K2Node_InputActionEvent_14");
    ::Sleep(400);
    Input(L"InpActEvt_atv_honk_K2Node_InputActionEvent_15");
}
void ClientOff() {
    OnGameThread([] {
        const bool ok = A::Dismount(g_atv);
        UE_LOGI("atv-host: CLIENT got off (ok=%d)", ok ? 1 : 0);
    });
}

// The seated client's copy as its driving leaves it: its fuel burnt down to 60, which its blocks carry to the host.
void ClientBurn60() {
    OnGameThread([] {
        SetField(g_atv, L"fuel", 60.f);
        UE_LOGI("atv-host: CLIENT driver's fuel burnt to 60");
    });
}

// The host's seat takes empty hands: the drill's own module, which the host's take-off handed over, is removed first.
void HostSitClean() {
    OnGameThread([] {
        void* hand = nullptr;
        Field(Me(), L"holding_actor", hand);
        if (hand) E::DestroyActor(hand);
    });
    // The client left the brake off and the turbo on; a host seated on that rolls away and is thrown, so the brake goes
    // on and the turbo off first, through the ATV's own keys.
    bool brake = false, turbo = false;
    OnGameThread([&] {
        brake = A::GetBrake(g_atv);
        Field(g_atv, L"turbo", turbo);
    });
    if (turbo) Input(L"InpActEvt_atv_turbo_K2Node_InputActionEvent_13");   // the release refuses with the brake on
    if (!brake) Input(L"InpActEvt_atv_brake_K2Node_InputActionEvent_22");
    ::Sleep(1000);
    ClientSit();
}

// The game's Custom Content rule, in this process's rules and never saved: the radio's openLink refuses without it.
void AllowCustomContent() {
    OnGameThread([] {
        void* instance = R::FindObjectByClass(L"mainGameInstance_C");
        void* cls = instance ? R::ClassOf(instance) : nullptr;
        const int32_t rules = cls ? R::FindPropertyOffset(cls, L"gameRules") : -1;
        void* type = cls ? R::PropertyInnerStruct(cls, L"gameRules") : nullptr;
        const int32_t rule = type ? R::FindPropertyOffsetByPrefix(type, L"customContent_") : -1;
        if (rules >= 0 && rule >= 0) static_cast<uint8_t*>(instance)[rules + rule] = 1;
    });
}

// A track picked on the ATV radio as a player picks it: its interface opened by the radio's own useRadio, and the
// interface's own select given the `index`th track slot's row, which runs the radio's own openLink.
void PickSlot(int index) {
    OnGameThread([&] {
        void* radio = Radio();
        void* fn = radio ? R::FindDispatchFunctionCached(R::ClassOf(radio), L"useRadio") : nullptr;
        if (fn) {
            ue_wrap::ParamFrame f(fn);
            if (f.valid() && f.Set<void*>(L"target", Me())) ue_wrap::Call(radio, f);
        }
    });
    ::Sleep(1500);
    OnGameThread([&] {
        const int32_t off = ue_wrap::reflected_offset::MainPlayer_activeInterface();
        void* widget = nullptr;
        if (off >= 0) std::memcpy(&widget, static_cast<const uint8_t*>(Me()) + off, sizeof(widget));
        void* slot = nullptr;
        int32_t count = -1;
        const int32_t slotsOff = widget && R::IsLive(widget) ? R::FindPropertyOffset(R::ClassOf(widget), L"slots") : -1;
        if (slotsOff >= 0) {
            const uint8_t* base = static_cast<const uint8_t*>(widget) + slotsOff;
            void* const* data = *reinterpret_cast<void* const* const*>(base);
            count = *reinterpret_cast<const int32_t*>(base + 8);
            if (data && index < count) slot = data[index];
        }
        // The slot's track row, handed to the interface's own select(data, pausePlayer), which writes the station and
        // runs the radio's openLink.
        bool ok = false;
        const int32_t dataOff = slot && R::IsLive(slot) ? R::FindPropertyOffset(R::ClassOf(slot), L"data") : -1;
        void* select = widget ? R::FindFunction(R::ClassOf(widget), L"select") : nullptr;
        const int32_t size = select ? R::FindParamSize(select, L"data") : 0;
        if (dataOff >= 0 && select && size > 0) {
            ue_wrap::ParamFrame pf(select);
            ok = pf.valid() && pf.SetRaw(L"data", static_cast<const uint8_t*>(slot) + dataOff, size) &&
                 pf.Set<bool>(L"pausePlayer", false) && ue_wrap::Call(widget, pf);
        }
        UE_LOGI("atv-host: %hs picked slot %d of %d on the ATV radio (interface=%d ok=%d)",
                IsClientRole() ? "CLIENT" : "HOST", index, count, widget ? 1 : 0, ok ? 1 : 0);
    });
    ::Sleep(2500);
    OnGameThread([] {
        const int32_t off = ue_wrap::reflected_offset::MainPlayer_activeInterface();
        void* widget = nullptr;
        if (off >= 0) std::memcpy(&widget, static_cast<const uint8_t*>(Me()) + off, sizeof(widget));
        if (widget && R::IsLive(widget)) ue_wrap::component_calls::CallParamlessNamed(widget, L"quit");
    });
}

void ClientRadioA() { PickSlot(0); }
void HostRadioB() { PickSlot(1); }

// One step: the actor does it, then both log; the other waits for the actor's mark and logs after it.
void Step(bool hostActs, const char* name, void (*act)(), int waitMs) {
    const std::string done = std::string(name) + (hostActs ? "_host" : "_client");
    const bool mine = hostActs != IsClientRole();
    if (mine) {
        act();
        ::Sleep(waitMs);
        LogState(name);
        Write(done, "x");
        Await(std::string(name) + "_seen");
    } else {
        if (!Await(done)) return;
        ::Sleep(1500);
        LogState(name);
        Write(std::string(name) + "_seen", "x");
    }
}

void RunSteps() {
    AllowCustomContent();
    LogState("0-start");
    Step(true, "1-host-fuel-20", &HostFuel20, 3000);
    Step(false, "2-client-poured", &ClientPour, 4000);
    Step(true, "3-host-health-30", &HostHealth30, 3000);
    Step(false, "4-client-repaired", &ClientRepair, 4000);
    Step(false, "5-client-seated", &ClientSit, 3000);
    Step(false, "5b-client-drives-fuel-60", &ClientBurn60, 3000);
    Step(false, "6-client-lights", &ClientLights, 3000);
    Step(false, "7-client-brake", &ClientBrake, 3000);
    Step(false, "8-client-turbo", &ClientTurbo, 3000);
    Step(false, "9-client-honk", &ClientHonk, 3000);
    Step(false, "10-client-off", &ClientOff, 4000);
    Step(false, "11-client-module-on", &ClientFitModule, 4000);
    Step(false, "12-client-module-off", &ClientTakeModule, 4000);
    Step(true, "13-host-module-on", &ClientFitModule, 4000);
    Step(true, "14-host-module-off", &ClientTakeModule, 4000);
    Step(true, "15-host-seated", &HostSitClean, 3000);
    Step(true, "16-host-lights", &ClientLights, 3000);
    Step(true, "17-host-honk", &ClientHonk, 3000);
    Step(true, "18-host-off", &ClientOff, 4000);
    Step(false, "19-client-radio-pick", &ClientRadioA, 3000);
    Step(true, "20-host-radio-pick", &HostRadioB, 3000);
    UE_LOGI("atv-host: DONE");
}

void FirstAtv(void* ctx, void* actor, int32_t) {
    void** out = static_cast<void**>(ctx);
    if (!*out) *out = actor;
}

void RunHost() {
    ::Sleep(20000);
    if (!Await("client_ready")) return;
    std::string key;
    OnGameThread([&] {
        if (!A::EnsureResolved()) return;
        A::ForEachAtv(&FirstAtv, &g_atv);
        if (g_atv) {
            const std::wstring k = A::GetKeyString(g_atv);
            key.assign(k.begin(), k.end());
        }
    });
    if (!g_atv || key.empty()) {
        UE_LOGW("atv-host: VERDICT ABORT -- no ATV in the host's world");
        return;
    }
    UE_LOGI("atv-host: HOST ATV key=%hs", key.c_str());
    Write("atv_key", key);
    if (!Await("client_found")) return;
    RunSteps();
}

struct Find {
    std::wstring key;
    void* actor = nullptr;
};

void ByKey(void* ctx, void* actor, int32_t) {
    Find* f = static_cast<Find*>(ctx);
    if (!f->actor && A::GetKeyString(actor) == f->key) f->actor = actor;
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
    if (!Await("atv_key", &key)) return;
    Find find{std::wstring(key.begin(), key.end())};
    for (int i = 0; i < 300 && !find.actor; ++i) {
        OnGameThread([&] {
            if (A::EnsureResolved()) A::ForEachAtv(&ByKey, &find);
        });
        if (!find.actor) ::Sleep(100);
    }
    g_atv = find.actor;
    if (!g_atv) {
        UE_LOGW("atv-host: VERDICT ABORT -- the client never had the ATV keyed '%hs'", key.c_str());
        return;
    }
    // A refuel, a repair and a module need the player at the ATV, as the host judges a client's: the player gets there by
    // the ATV's own seat, which needs no walk, and its own dismount, which leaves it beside the ATV. A teleport into the
    // world can drop a player through it, which sends it to the backrooms.
    ClientSit();
    ClientOff();
    ::Sleep(3000);
    Write("client_found", "x");
    RunSteps();
}

}  // namespace

DWORD WINAPI AtvHostThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("atv-host: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (IsClientRole()) RunClient();
    else                RunHost();
    return 0;
}

}  // namespace harness::autotest
