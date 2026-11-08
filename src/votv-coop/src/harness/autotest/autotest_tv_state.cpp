// harness/autotest/autotest_tv_state.cpp -- TEST-ONLY: a placed TV across two peers.
//
// The host spawns a TV and names it in a folder both peers read; the driver has put two silent .mp4 files in the game's TV
// folder. The client opens the interface, takes a channel, picks a track, stops and plays it by the interface's own
// buttons and changes the picture; the host then takes another channel and track. Each step is read on both, and the
// client rejoins and reads them. Gated by VOTVCOOP_RUN_TV_STATE; the folder is VOTVCOOP_CAN_DIR; VOTVCOOP_TV_REJOIN=1 runs
// the rejoined client's half.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_use.h"
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
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace MD = ue_wrap::media_device;

constexpr uint8_t kActionOpen = 4;
constexpr int64_t kTimeSlackTicks = 50000000;  // 5 s between two peers' reads of one playing track

const wchar_t* const kStopButton = L"BndEvt__umg_onlineRadio_Button_stop_K2Node_ComponentBoundEvent_1_OnButtonClickedEvent__DelegateSignature";
const wchar_t* const kPlayButton = L"BndEvt__umg_onlineRadio_Button_play_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature";

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
    UE_LOGW("tv-drill: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

void Verdict(const char* step, bool pass, const std::string& detail) {
    if (!pass) ++g_fails;
    UE_LOGI("tv-drill: %hs %hs %hs", step, pass ? "PASS" : "FAIL", detail.c_str());
}

void* FindTv(const std::string& key) {
    const std::wstring w(key.begin(), key.end());
    return coop::prop_key_index::Find(w);
}

constexpr float kBrightness = 0.7f, kSqueezeX = 7.f, kSqueezeY = 9.f;

std::string Describe(const MD::State& s) {
    return "channel=" + std::to_string(s.channel) + " track=" + std::to_string(s.track) + " playing=" + std::to_string(s.playing) +
           " volume=" + std::to_string(s.volume) + " brightness=" + std::to_string(s.brightness) +
           " squeeze=" + std::to_string(s.squeezeX) + "," + std::to_string(s.squeezeY) +
           " seconds=" + std::to_string(s.ticks / 10000000);
}

MD::State Check(const char* step, const std::string& key, int32_t channel, uint64_t track, bool playing, float volume = -1.f,
                float brightness = -1.f) {
    MD::State s;
    bool read = false;
    const auto good = [&] {
        return read && s.channel == channel && s.track == track && s.playing == playing && (volume < 0.f || s.volume == volume) &&
               (brightness < 0.f || (std::fabs(s.brightness - brightness) < 0.01f && s.squeezeX == kSqueezeX && s.squeezeY == kSqueezeY));
    };
    for (int i = 0; i < 40; ++i) {
        OnGameThread([&] { read = MD::Read(FindTv(key), s); });
        if (good()) break;
        ::Sleep(250);
    }
    Verdict(step, good(), read ? Describe(s) : std::string("no such TV, or opening"));
    return s;
}

void* Interface() {
    void* player = coop::players::Registry::Get().Local();
    const int32_t off = player ? ue_wrap::reflected_offset::MainPlayer_activeInterface() : -1;
    void* widget = nullptr;
    if (off >= 0) std::memcpy(&widget, static_cast<const uint8_t*>(player) + off, sizeof(widget));
    return widget && R::IsLive(widget) ? widget : nullptr;
}

void OpenInterface(const std::string& key) {
    OnGameThread([&] {
        void* tv = FindTv(key);
        void* local = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!tv || !local || !E::TryGetActorLocation(tv, at)) return;
        ue_wrap::prop_use::CallActionOptionIndex(tv, local, ue_wrap::prop::GetStaticMesh(tv), at, kActionOpen, ue_wrap::prop::GetStaticMesh(tv));
    });
}

void CloseInterface() {
    OnGameThread([&] {
        if (void* widget = Interface()) ue_wrap::component_calls::CallParamlessNamed(widget, L"quit");
    });
}

void Button(const wchar_t* name) {
    OnGameThread([&] {
        if (void* widget = Interface()) ue_wrap::component_calls::CallParamlessNamed(widget, name);
    });
}

// The interface's own setChannel, its channel buttons' one call.
void SetChannel(int32_t channel) {
    OnGameThread([&] {
        void* widget = Interface();
        void* fn = widget ? R::FindDispatchFunctionCached(R::ClassOf(widget), L"setChannel") : nullptr;
        if (!fn) {
            UE_LOGW("tv-drill: no interface open to take a channel");
            return;
        }
        ue_wrap::ParamFrame f(fn);
        if (f.valid() && f.Set<int32_t>(L"add", channel)) ue_wrap::Call(widget, f);
    });
}

void Pick(const std::string& key, uint64_t track) {
    OnGameThread([&] {
        MD::State s;
        MD::Read(FindTv(key), s);
        s.track = track;
        s.ticks = 0;
        s.playing = true;
        MD::ApplyState(FindTv(key), s);
    });
}

void SetPicture(const std::string& key, float volume, float brightness) {
    OnGameThread([&] {
        MD::State s;
        if (!MD::Read(FindTv(key), s)) return;
        s.volume = volume;
        s.brightness = brightness;
        s.squeezeX = kSqueezeX;
        s.squeezeY = kSqueezeY;
        MD::ApplyState(FindTv(key), s);
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
        else UE_LOGW("tv-drill: the Custom Content rule did not resolve");
    });
}

// Powered by a write, since a cord is another lane; the TV's openLink needs it.
void PowerOn(const std::string& key) {
    OnGameThread([&] {
        void* tv = FindTv(key);
        int32_t off = -1;
        uint8_t mask = 0;
        if (!tv || !R::FindBoolProperty(R::ClassOf(tv), L"powered", off, mask) || off < 0 || !mask) return;
        static_cast<uint8_t*>(tv)[off] |= mask;
    });
}

void RunHost() {
    if (!Await("client_ready")) return;
    AllowCustomContent();
    ::Sleep(3000);
    std::string key;
    uint64_t a = 0, b = 0;
    OnGameThread([&] {
        void* cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/prop_tv2.prop_tv2_C");
        void* local = coop::players::Registry::Get().Local();
        ue_wrap::FVector me{};
        if (!cls || !local || !E::TryGetActorLocation(local, me)) return;
        void* tv = E::SpawnActor(cls, {me.X + 150.f, me.Y, me.Z + 40.f});
        const std::wstring k = tv ? ue_wrap::prop::GetInteractableKeyString(tv) : L"";
        key.assign(k.begin(), k.end());
        a = tv ? MD::CatalogTrack(tv, 0) : 0;
        b = tv ? MD::CatalogTrack(tv, 1) : 0;
    });
    if (key.empty() || !a || !b || a == b) {
        UE_LOGW("tv-drill: VERDICT ABORT -- TV '%hs', tracks %llu and %llu", key.c_str(), static_cast<unsigned long long>(a),
                static_cast<unsigned long long>(b));
        Mark("keys", "none");
        return;
    }
    PowerOn(key);
    ::Sleep(2000);
    Mark("keys", key + " " + std::to_string(a) + " " + std::to_string(b));

    if (!Await("c1")) return;
    Check("01-client-takes-channel-1", key, 1, 0, false);
    Mark("h1");
    if (!Await("c2")) return;
    Check("02-client-picks-a", key, 1, a, true);
    Mark("h2");
    if (!Await("c3")) return;
    Check("03-client-stops", key, 1, a, false);
    Mark("h3");
    if (!Await("c4")) return;
    Check("04-client-plays", key, 1, a, true);
    Mark("h4");
    if (!Await("c5")) return;
    Check("05-client-changes-picture", key, 1, a, true, 30.f, kBrightness);
    Mark("h5");
    if (!Await("c6")) return;
    OpenInterface(key);
    ::Sleep(500);
    SetChannel(2);
    ::Sleep(500);
    Pick(key, b);
    Check("06-host-takes-channel-2-and-b", key, 2, b, true, 30.f, kBrightness);
    CloseInterface();
    Mark("h6");
    if (!Await("c6b")) return;
    MD::State after;
    OnGameThread([&] { MD::Read(FindTv(key), after); });
    Mark("h6b", std::to_string(after.ticks));
    if (!Await("c7")) return;
    UE_LOGI("tv-drill: DONE on host fails=%d", g_fails);
    if (!Await("rejoin_ready", nullptr, 600)) return;
    OnGameThread([&] { MD::Read(FindTv(key), after); });
    Mark("h8", std::to_string(after.ticks));
    if (!Await("rejoin_done", nullptr, 120)) return;
    UE_LOGI("tv-drill: REJOIN DONE on host fails=%d", g_fails);
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
        void* tv = nullptr;
        OnGameThread([&] { tv = FindTv(key); });
        if (tv) break;
        ::Sleep(100);
    }
    PowerOn(key);
    OpenInterface(key);
    ::Sleep(1000);
    SetChannel(1);
    Check("01-client-takes-channel-1", key, 1, 0, false);
    Mark("c1");
    if (!Await("h1")) return;
    Pick(key, a);
    Check("02-client-picks-a", key, 1, a, true);
    Mark("c2");
    if (!Await("h2")) return;
    Button(kStopButton);
    Check("03-client-stops", key, 1, a, false);
    Mark("c3");
    if (!Await("h3")) return;
    Button(kPlayButton);
    Check("04-client-plays", key, 1, a, true);
    Mark("c4");
    if (!Await("h4")) return;
    SetPicture(key, 30.f, kBrightness);
    Check("05-client-changes-picture", key, 1, a, true, 30.f, kBrightness);
    Mark("c5");
    if (!Await("h5")) return;
    CloseInterface();
    ::Sleep(500);
    Mark("c6");
    if (!Await("h6")) return;
    MD::State s = Check("06-host-takes-channel-2-and-b", key, 2, b, true, 30.f, kBrightness);
    OnGameThread([&] { MD::Read(FindTv(key), s); });
    Mark("c6b");
    std::string hostTicks;
    if (!Await("h6b", &hostTicks)) return;
    const int64_t apart = s.ticks - std::stoll(hostTicks);
    Verdict("06-times-close", apart < kTimeSlackTicks && apart > -kTimeSlackTicks, std::to_string(apart / 10000000) + " s apart");
    UE_LOGI("tv-drill: DONE on client fails=%d", g_fails);
    Mark("c7");
}

void RunRejoin() {
    WaitForWorld();
    AllowCustomContent();
    std::string key;
    uint64_t a = 0, b = 0;
    if (!ReadKeys(key, a, b)) return;
    PowerOn(key);
    ::Sleep(6000);
    const MD::State s = Check("08-joiner-reads-all", key, 2, b, true, 30.f, kBrightness);
    Mark("rejoin_ready");
    std::string hostTicks;
    if (Await("h8", &hostTicks)) {
        const int64_t apart = s.ticks - std::stoll(hostTicks);
        Verdict("08-times-close", apart < kTimeSlackTicks && apart > -kTimeSlackTicks, std::to_string(apart / 10000000) + " s apart");
    }
    UE_LOGI("tv-drill: REJOIN DONE fails=%d", g_fails);
    Mark("rejoin_done");
}

}  // namespace

DWORD WINAPI TvStateThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("tv-drill: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (!IsClientRole()) RunHost();
    else if (coop::config::ReadEnv("VOTVCOOP_TV_REJOIN") == "1") RunRejoin();
    else RunClient();
    return 0;
}

}  // namespace harness::autotest
