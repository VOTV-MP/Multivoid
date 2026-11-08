// harness/autotest/autotest_radio_state.cpp -- TEST-ONLY: a placed internet radio across two peers.
//
// The host spawns a radio and names it in a folder both peers read; the driver has put two test tones in the game's radio
// folder. The client opens the interface and picks a track, stops it, plays it and changes the mode by the interface's
// own buttons; the host then picks another track. Each step is read on both. The client rejoins and reads the track.
// A peer's picking is the radio's own openLink. Gated by VOTVCOOP_RUN_RADIO_STATE; the folder is VOTVCOOP_CAN_DIR;
// VOTVCOOP_RADIO_REJOIN=1 runs the rejoined client's half.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflected_offset.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/media_device.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <string>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace RD = ue_wrap::media_device;

constexpr int64_t kTimeSlackTicks = 50000000;  // 5 s between two peers' reads of one playing track

const wchar_t* const kStopButton = L"BndEvt__umg_onlineRadio_Button_stop_K2Node_ComponentBoundEvent_1_OnButtonClickedEvent__DelegateSignature";
const wchar_t* const kPlayButton = L"BndEvt__umg_onlineRadio_Button_play_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature";
const wchar_t* const kModeButton = L"BndEvt__umg_onlineRadio_Button_104_K2Node_ComponentBoundEvent_2_OnButtonClickedEvent__DelegateSignature";

std::string g_dir;
int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Mark(const char* name, const std::string& body = "x") {
    std::ofstream(g_dir + "\\" + name, std::ios::binary | std::ios::trunc) << body;
}

bool Await(const char* name, std::string* body = nullptr, int seconds = 240) {
    for (int i = 0; i < seconds * 10; ++i) {
        std::ifstream f(g_dir + "\\" + name, std::ios::binary);
        std::string s;
        if (f && std::getline(f, s) && !s.empty()) {
            if (body) *body = s;
            return true;
        }
        ::Sleep(100);
    }
    UE_LOGW("radio-drill: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

void Verdict(const char* step, bool pass, const std::string& detail) {
    if (!pass) ++g_fails;
    UE_LOGI("radio-drill: %hs %hs %hs", step, pass ? "PASS" : "FAIL", detail.c_str());
}

void* FindRadio(const std::string& key) {
    const std::wstring w(key.begin(), key.end());
    return coop::prop_key_index::Find(w);
}

std::string Describe(const RD::State& s) {
    return "track=" + std::to_string(s.track) + " playing=" + std::to_string(s.playing) + " mode=" + std::to_string(s.mode) +
           " seconds=" + std::to_string(s.ticks / 10000000);
}

// The radio's state must reach what is wanted within a few seconds.
RD::State Check(const char* step, const std::string& key, uint64_t track, bool playing, int mode) {
    RD::State s;
    bool read = false;
    for (int i = 0; i < 40; ++i) {
        OnGameThread([&] { read = RD::Read(FindRadio(key), s); });
        if (read && s.track == track && s.playing == playing && s.mode == mode) break;
        ::Sleep(250);
    }
    Verdict(step, read && s.track == track && s.playing == playing && s.mode == mode,
            read ? Describe(s) : std::string("no such radio, or opening"));
    return s;
}

void* Interface(void* player) {
    const int32_t off = player ? ue_wrap::reflected_offset::MainPlayer_activeInterface() : -1;
    void* widget = nullptr;
    if (off >= 0) std::memcpy(&widget, static_cast<const uint8_t*>(player) + off, sizeof(widget));
    return widget;
}

void OpenInterface(const std::string& key) {
    OnGameThread([&] {
        void* radio = FindRadio(key);
        void* local = coop::players::Registry::Get().Local();
        void* fn = radio ? R::FindDispatchFunctionCached(R::ClassOf(radio), L"useRadio") : nullptr;
        if (!fn || !local) return;
        ue_wrap::ParamFrame f(fn);
        if (f.valid() && f.Set<void*>(L"target", local)) ue_wrap::Call(radio, f);
    });
}

void CloseInterface() {
    OnGameThread([&] {
        void* widget = Interface(coop::players::Registry::Get().Local());
        if (widget && R::IsLive(widget)) ue_wrap::component_calls::CallParamlessNamed(widget, L"quit");
    });
}

void Button(const wchar_t* name) {
    OnGameThread([&] {
        void* widget = Interface(coop::players::Registry::Get().Local());
        if (widget && R::IsLive(widget)) ue_wrap::component_calls::CallParamlessNamed(widget, name);
    });
}

// The game's Custom Content rule, in this process's rules and never saved: openLink refuses without it.
void AllowCustomContent() {
    OnGameThread([&] {
        void* instance = R::FindObjectByClass(L"mainGameInstance_C");
        void* cls = instance ? R::ClassOf(instance) : nullptr;
        const int32_t rules = cls ? R::FindPropertyOffset(cls, L"gameRules") : -1;
        void* type = cls ? R::PropertyInnerStruct(cls, L"gameRules") : nullptr;
        const int32_t rule = type ? R::FindPropertyOffsetByPrefix(type, L"customContent_") : -1;
        if (rules >= 0 && rule >= 0) static_cast<uint8_t*>(instance)[rules + rule] = 1;
        else UE_LOGW("radio-drill: the Custom Content rule did not resolve");
    });
}

void Pick(const std::string& key, uint64_t track) {
    OnGameThread([&] {
        RD::State s;
        RD::Read(FindRadio(key), s);  // the mode it has
        s.track = track;
        s.ticks = 0;
        s.playing = true;
        RD::ApplyState(FindRadio(key), s);
    });
}

void RunHost() {
    if (!Await("client_ready")) return;
    AllowCustomContent();
    ::Sleep(3000);
    std::string key;
    uint64_t a = 0, b = 0;
    OnGameThread([&] {
        void* cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/prop_radio_internet.prop_radio_internet_C");
        void* local = coop::players::Registry::Get().Local();
        ue_wrap::FVector me{};
        if (!cls || !local || !E::TryGetActorLocation(local, me)) return;
        void* radio = E::SpawnActor(cls, {me.X + 120.f, me.Y, me.Z + 40.f});
        const std::wstring k = radio ? ue_wrap::prop::GetInteractableKeyString(radio) : L"";
        key.assign(k.begin(), k.end());
        a = radio ? RD::CatalogTrack(radio, 0) : 0;
        b = radio ? RD::CatalogTrack(radio, 1) : 0;
    });
    if (key.empty() || !a || !b || a == b) {
        UE_LOGW("radio-drill: VERDICT ABORT -- radio '%hs', tracks %llu and %llu", key.c_str(),
                static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
        Mark("keys", "none");
        return;
    }
    ::Sleep(2000);
    Mark("keys", key + " " + std::to_string(a) + " " + std::to_string(b));

    if (!Await("c1")) return;
    Check("01-client-picks-a", key, a, true, 0);
    Mark("h1");
    if (!Await("c2")) return;
    Check("02-client-stops", key, a, false, 0);
    Mark("h2");
    if (!Await("c3")) return;
    Check("03-client-plays", key, a, true, 0);
    Mark("h3");
    if (!Await("c4")) return;
    RD::State after = Check("04-client-changes-mode", key, a, true, 1);
    Mark("h4");
    if (!Await("c5")) return;
    OpenInterface(key);
    ::Sleep(500);
    Pick(key, b);
    after = Check("05-host-picks-b", key, b, true, 1);
    CloseInterface();
    Mark("h5");
    if (!Await("c5b")) return;
    OnGameThread([&] { RD::Read(FindRadio(key), after); });
    Mark("h5b", std::to_string(after.ticks));
    if (!Await("c6")) return;
    UE_LOGI("radio-drill: DONE on host fails=%d", g_fails);
    if (!Await("rejoin_ready", nullptr, 600)) return;
    OnGameThread([&] { RD::Read(FindRadio(key), after); });
    Mark("h7", std::to_string(after.ticks));
    if (!Await("rejoin_done", nullptr, 120)) return;
    UE_LOGI("radio-drill: REJOIN DONE on host fails=%d", g_fails);
}

void WaitForWorld() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(8000);
}

bool ReadKeys(std::string& key, uint64_t& a, uint64_t& b) {
    std::string line;
    if (!Await("keys", &line) || line == "none") return false;
    const size_t one = line.find(' '), two = line.find(' ', one + 1);
    key = line.substr(0, one);
    a = std::stoull(line.substr(one + 1, two - one - 1));
    b = std::stoull(line.substr(two + 1));
    return true;
}

void RunClient() {
    WaitForWorld();
    AllowCustomContent();
    Mark("client_ready");
    std::string key;
    uint64_t a = 0, b = 0;
    if (!ReadKeys(key, a, b)) return;
    for (int i = 0; i < 100; ++i) {
        void* radio = nullptr;
        OnGameThread([&] { radio = FindRadio(key); });
        if (radio) break;
        ::Sleep(100);
    }
    OpenInterface(key);
    ::Sleep(1000);
    Pick(key, a);
    Check("01-client-picks-a", key, a, true, 0);
    Mark("c1");
    if (!Await("h1")) return;
    Button(kStopButton);
    Check("02-client-stops", key, a, false, 0);
    Mark("c2");
    if (!Await("h2")) return;
    Button(kPlayButton);
    Check("03-client-plays", key, a, true, 0);
    Mark("c3");
    if (!Await("h3")) return;
    Button(kModeButton);
    Check("04-client-changes-mode", key, a, true, 1);
    Mark("c4");
    if (!Await("h4")) return;
    CloseInterface();
    ::Sleep(500);
    Mark("c5");
    if (!Await("h5")) return;
    RD::State s = Check("05-host-picks-b", key, b, true, 1);
    OnGameThread([&] { RD::Read(FindRadio(key), s); });
    Mark("c5b");
    std::string hostTicks;
    if (!Await("h5b", &hostTicks)) return;
    const int64_t apart = s.ticks - std::stoll(hostTicks);
    Verdict("05-times-close", apart < kTimeSlackTicks && apart > -kTimeSlackTicks, std::to_string(apart / 10000000) + " s apart");
    UE_LOGI("radio-drill: DONE on client fails=%d", g_fails);
    Mark("c6");
}

void RunRejoin() {
    WaitForWorld();
    AllowCustomContent();
    std::string key;
    uint64_t a = 0, b = 0;
    if (!ReadKeys(key, a, b)) return;
    ::Sleep(6000);
    const RD::State s = Check("07-joiner-reads-b", key, b, true, 1);
    Mark("rejoin_ready");
    std::string hostTicks;
    if (Await("h7", &hostTicks)) {
        const int64_t apart = s.ticks - std::stoll(hostTicks);
        Verdict("07-times-close", apart < kTimeSlackTicks && apart > -kTimeSlackTicks, std::to_string(apart / 10000000) + " s apart");
    }
    UE_LOGI("radio-drill: REJOIN DONE fails=%d", g_fails);
    Mark("rejoin_done");
}

}  // namespace

DWORD WINAPI RadioStateThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("radio-drill: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (!IsClientRole()) RunHost();
    else if (coop::config::ReadEnv("VOTVCOOP_RADIO_REJOIN") == "1") RunRejoin();
    else RunClient();
    return 0;
}

}  // namespace harness::autotest
