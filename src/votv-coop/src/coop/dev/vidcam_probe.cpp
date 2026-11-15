// coop/dev/vidcam_probe.cpp -- [dev] a camcorder recording driven through the game's own calls
// (VOTVCOOP_VIDCAM_PROBE=host|client).
//
// The camcorder (prop_vidcam_C) records through the gamemode's camera manager (rcm_manager, an RCM_cameraManager_C on
// the MSC_Record plugin's RecordCameraManager): the camcorder's LMB runs its beginRecord, which starts the capture, turns
// the player's microphone component on when the audio settings allow it and moves the audio listener to the camera; the
// manager's stopRecord finishes the capture, and the plugin's merger writes the mp4. THE RECORDER, 20 s after a client
// is connected: spawns a camcorder before its player and picks it up through `Hold Object`; at 23 s fills the
// manager's battery and presses the hand's camcorder's LMB; at VOTVCOOP_VIDCAM_STOP s (default 33) runs stopRecord.
// It logs, every quarter second from 22 s to 90 s, the recorder's flags, its export progress and its file names
// (`[VIDCAM]`). THE OTHER PEER, with VOTVCOOP_VIDCAM_AWAY=<uu>: at 21 s steps that far along world X.

#include "coop/dev/vidcam_probe.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <string>

namespace coop::dev::vidcam_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;

constexpr const wchar_t* kCamClass = L"prop_vidcam_C";

uint64_t Now() { return ::GetTickCount64(); }

const std::string& Who() {
    static const std::string w = coop::config::ReadEnv("VOTVCOOP_VIDCAM_PROBE");
    return w;
}

uint64_t StopAtMs() {
    static const uint64_t s = [] {
        const std::string v = coop::config::ReadEnv("VOTVCOOP_VIDCAM_STOP");
        return 1000ull * (v.empty() ? 33ull : static_cast<uint64_t>(std::atoll(v.c_str())));
    }();
    return s;
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

float* FloatAt(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? reinterpret_cast<float*>(static_cast<char*>(obj) + off) : nullptr;
}

std::wstring Str(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? ue_wrap::field_io::ReadFStringAt(obj, off) : L"?";
}

void* Manager() { return Obj(ue_wrap::world_singleton::Gamemode(), L"rcm_manager"); }

bool CallWithPlayer(void* obj, const wchar_t* fn, void* player) {
    void* const f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    ue_wrap::ParamFrame p(f);
    if (!p.valid()) return false;
    if (player && R::FindParamOffset(f, L"player") >= 0) p.Set<void*>(L"player", player);
    return ue_wrap::Call(obj, p);
}

void Report() {
    void* const m = Manager();
    const float* const progress = FloatAt(m, L"ExportProgress");
    const float* const battery = FloatAt(m, L"batteryPower");
    UE_LOGI("[VIDCAM] recorder recording=%d paused=%d exporting=%d progress=%.3f battery=%.1f micEnabled=%d "
            "micPermission=%d file='%ls' out='%ls' capDir='%ls'",
            Bool(m, L"bIsRecording"), Bool(m, L"bIsPaused"), Bool(m, L"bIsExporting"), progress ? *progress : -1.f,
            battery ? *battery : -1.f, Bool(m, L"micEnabled"), Bool(m, L"micPermission"), Str(m, L"Filename").c_str(),
            Str(m, L"OutputDirectory").c_str(), Str(m, L"VideoCapDir").c_str());
}

uint64_t g_startAt = 0, g_reportAt = 0;
int g_step = 0;
int g_pauseStep = 0;

}  // namespace

void Tick(coop::net::Session* session) {
    if (Who().empty() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    // The other peer, with VOTVCOOP_VIDCAM_AWAY=<uu>: at 21 s steps that far along world X from where it stands, so the
    // camera hears it from a distance.
    if ((Who() == "host") != host) {
        static uint64_t s_armed = 0;
        static bool s_moved = false;
        if (!s_armed) s_armed = Now();
        const float away = static_cast<float>(std::atof(coop::config::ReadEnv("VOTVCOOP_VIDCAM_AWAY").c_str()));
        void* const me = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!s_moved && away != 0.f && Now() - s_armed >= 21000u && me && E::TryGetActorLocation(me, at)) {
            s_moved = true;
            UE_LOGI("[VIDCAM] other: stepped %.0f uu away (ok=%d)", away,
                    E::SetActorLocation(me, {at.X + away, at.Y, at.Z}) ? 1 : 0);
        }
        return;
    }
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[VIDCAM] probe armed role=%s", host ? "host" : "client");
    }
    const uint64_t t = Now() - g_startAt;
    void* const me = coop::players::Registry::Get().Local();
    if (g_step == 0 && t >= 20000u) {
        g_step = 1;
        // A camcorder spawned before the player and picked up through `Hold Object`, as a bought one is: its data (the
        // frame rate, the quality) goes into the hand with it. The drill hand's empty hold slot records at 0 fps.
        // VOTVCOOP_VIDCAM_CLASS=<model> takes prop_vidcam_<model>_C (supershit, low, mid, high, superhigh) instead.
        const std::string model = coop::config::ReadEnv("VOTVCOOP_VIDCAM_CLASS");
        const std::wstring leaf = model.empty() ? L"prop_vidcam" : L"prop_vidcam_" + std::wstring(model.begin(), model.end());
        void* cls = ue_wrap::object_index::ClassByName((leaf + L"_C").c_str());
        if (!cls) cls = ue_wrap::asset_load::LoadObjectByPath((L"/Game/objects/" + leaf + L"." + leaf + L"_C").c_str());
        ue_wrap::FVector at{};
        const ue_wrap::FVector f = me ? E::GetActorForwardVector(me) : ue_wrap::FVector{};
        void* const cam = cls && me && E::TryGetActorLocation(me, at)
                              ? E::SpawnActor(cls, {at.X + f.X * 80.f, at.Y + f.Y * 80.f, at.Z})
                              : nullptr;
        bool collected = false;
        const bool held = cam && E::CallMainPlayerHoldObject(me, cam, collected);
        void* const hand = Obj(me, L"holding_actor");
        UE_LOGI("[VIDCAM] recorder: picked up a spawned camcorder (spawned=%d, Hold Object ran=%d, the hand holds %ls)",
                cam ? 1 : 0, held ? 1 : 0, hand ? R::ClassNameOf(hand).c_str() : L"nothing");
    }
    if (g_step == 1 && t >= 23000u) {
        g_step = 2;
        void* const m = Manager();
        if (float* const battery = FloatAt(m, L"batteryPower")) *battery = 100.f;
        void* const cam = Obj(me, L"holding_actor");
        UE_LOGI("[VIDCAM] recorder: pressed the camcorder's LMB (camcorder %p, manager %p, ran=%d)", cam, m,
                CallWithPlayer(cam, L"playerHandUse_LMB", me) ? 1 : 0);
    }
    // VOTVCOOP_VIDCAM_PAUSE=1: the camcorder's LMB again at 26 s (a pause, as its own LMB pauses a recording) and at
    // 29 s (the resume).
    static const bool s_pause = coop::config::ReadEnv("VOTVCOOP_VIDCAM_PAUSE") == "1";
    if (s_pause && g_step == 2 && (g_pauseStep == 0 && t >= 26000u || g_pauseStep == 1 && t >= 29000u)) {
        ++g_pauseStep;
        UE_LOGI("[VIDCAM] recorder: pressed the camcorder's LMB to %hs (ran=%d)", g_pauseStep == 1 ? "pause" : "resume",
                CallWithPlayer(Obj(me, L"holding_actor"), L"playerHandUse_LMB", me) ? 1 : 0);
    }
    if (g_step == 2 && t >= StopAtMs()) {
        g_step = 3;
        void* const m = Manager();
        UE_LOGI("[VIDCAM] recorder: ran the manager's stopRecord (ran=%d)",
                m && R::CallFunction(m, R::FindDispatchFunctionCached(R::ClassOf(m), L"stopRecord"), nullptr) ? 1 : 0);
    }
    if (t < 22000u || t > 90000u || Now() < g_reportAt) return;
    g_reportAt = Now() + 250;
    Report();
}

}  // namespace coop::dev::vidcam_probe
