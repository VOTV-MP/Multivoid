// harness/autotest/autotest_prop_fire.cpp -- TEST-ONLY: a burning prop, both ways.
//
// The host spawns a log before the client and lights it: the client's copy must burn. The host puts it out: the copy must go
// out. Then the host drops a second log before the client, which aims a lighter at it and lights it with the lighter's own
// click: its copy
// must not light itself, the host's log must burn, and the host's flame must come back to the copy. Gated by
// VOTVCOOP_RUN_PROP_FIRE; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/dev/director/aim_fan.h"
#include "coop/dev/drill_hand.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_fire.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

namespace harness::autotest {
namespace {

namespace E = ue_wrap::engine;
namespace F = ue_wrap::prop;
namespace R = ue_wrap::reflection;

constexpr const wchar_t* kLogPath = L"/Game/objects/prop_logcut.prop_logcut_C";
constexpr const wchar_t* kLighterPath = L"/Game/objects/prop_lighter.prop_lighter_C";

std::string g_dir;
int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Mark(const char* name, const std::string& body = "x") {
    std::string tmp = g_dir + "\\" + name + ".tmp";
    { std::ofstream(tmp, std::ios::binary | std::ios::trunc) << body; }
    ::MoveFileExA(tmp.c_str(), (g_dir + "\\" + name).c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool Await(const char* name, std::string* body = nullptr, int seconds = 240) {
    for (int i = 0; i < seconds * 10; ++i) {
        std::ifstream f(g_dir + "\\" + name, std::ios::binary);
        if (f) {
            if (body) body->assign(std::istreambuf_iterator<char>(f), {});
            return true;
        }
        ::Sleep(100);
    }
    UE_LOGW("prop-fire: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

void Check(bool ok, const char* what) {
    UE_LOGI("prop-fire: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }
std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

void* ByKey(const std::string& key) { return coop::prop_key_index::Find(Widen(key)); }

bool Burning(const std::string& key) {
    bool on = false;
    OnGameThread([&] {
        F::FireState s;
        void* p = ByKey(key);
        on = p && F::ReadPropFire(p, s) && s.burning;
    });
    return on;
}

bool WaitBurning(const std::string& key, bool want, int seconds = 6) {
    for (int i = 0; i < seconds * 10; ++i) {
        if (Burning(key) == want) return true;
        ::Sleep(100);
    }
    return false;
}

// A log `ahead` cm before `actor` along its forward, dropped from 50 cm up. "" if it does not stand.
std::string SpawnLogBefore(void* actor, float ahead) {
    void* cls = ue_wrap::asset_load::LoadObjectByPath(kLogPath);
    ue_wrap::FVector at{};
    if (!actor || !cls || !E::TryGetActorLocation(actor, at)) return {};
    const ue_wrap::FVector fwd = E::GetActorForwardVector(actor);
    void* log = E::SpawnActor(cls, {at.X + fwd.X * ahead, at.Y + fwd.Y * ahead, at.Z + 50.f});
    return log ? Narrow(F::GetInteractableKeyString(log)) : std::string();
}

// The first log 2 m before the host; the second 150 cm before the client's puppet, where its lighter reaches.
std::string SpawnLog(bool beforeClient) {
    std::string key;
    OnGameThread([&] {
        void* actor = beforeClient ? coop::puppet_drive::Puppet(1).GetActor() : coop::players::Registry::Get().Local();
        if (actor && R::IsLive(actor)) key = SpawnLogBefore(actor, beforeClient ? 150.f : 200.f);
    });
    return key;
}

template <class T>
void WriteField(void* obj, const wchar_t* name, T v) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), name);
    if (off >= 0) std::memcpy(static_cast<uint8_t*>(obj) + off, &v, sizeof(v));
}

void WriteFlag(void* obj, const wchar_t* name, bool v) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(obj), name, off, mask) || off < 0) return;
    uint8_t& b = static_cast<uint8_t*>(obj)[off];
    b = v ? (b | mask) : (b & ~mask);
}

// The client's light: a lighter in the hand, opened, the camera turned through the aim fan round the log's bounds
// centre until the player's own trace takes the log, then the lighter's own left click. True when the click ran.
bool LightWithLighter(const std::string& key) {
    bool held = false;
    OnGameThread([&] {
        ue_wrap::asset_load::LoadObjectByPath(kLighterPath);
        held = coop::dev::drill_hand::TakeIntoHandNow(L"lighter", L"prop_lighter_C");
    });
    if (!held) return false;
    ::Sleep(1000);
    coop::director::AimFan fan;
    bool clicked = false, done = false, said = false;
    while (!done) {
        OnGameThread([&] {
            void* pawn = coop::players::Registry::Get().Local();
            void* log = ByKey(key);
            ue_wrap::FVector at{}, extent{};
            if (!pawn || !log || !E::GetActorBounds(log, true, at, extent)) {
                UE_LOGI("prop-fire: the aim lost the player %p or the log %p", pawn, log);
                done = true;
                return;
            }
            if (!said) {
                said = true;
                const ue_wrap::FVector eye = E::GetCameraLocation();
                UE_LOGI("prop-fire: aim from (%.0f %.0f %.0f) at (%.0f %.0f %.0f) extent (%.0f %.0f %.0f)", eye.X, eye.Y,
                        eye.Z, at.X, at.Y, at.Z, extent.X, extent.Y, extent.Z);
            }
            const auto st = fan.Tick(pawn, at, E::ReadMainPlayerHitActor(pawn) == log);
            if (st == coop::director::AimFan::State::Working) return;
            done = true;
            if (st == coop::director::AimFan::State::Failed) {
                UE_LOGI("prop-fire: no pose of the fan put the trace on the log");
                return;
            }
            E::MainPlayerGrabState gs{};
            void* lighter = E::ReadMainPlayerGrabState(pawn, gs) ? gs.holdingActor : nullptr;
            if (!lighter || !R::IsLive(lighter)) {
                UE_LOGI("prop-fire: the lighter left the hand");
                return;
            }
            UE_LOGI("prop-fire: the trace takes the log after %d poses", fan.Poses());
            WriteFlag(lighter, L"opened", true);
            WriteField<int32_t>(lighter, L"uses", 100);
            void* fn = R::FindDispatchFunctionCached(R::ClassOf(lighter), L"playerHandUse_LMB");
            ue_wrap::ParamFrame f(fn);
            clicked = fn && f.valid() && f.Set<void*>(L"player", pawn) && ue_wrap::Call(lighter, f);
        });
        if (!done) ::Sleep(16);
    }
    return clicked;
}

void RunHost() {
    if (!Await("client_ready", nullptr, 480)) return;
    const std::string first = SpawnLog(false);
    Check(!first.empty(), "a log stands");
    if (first.empty()) return;
    Mark("log", first);
    if (!Await("log_seen", nullptr, 60)) return;

    bool lit = false;
    OnGameThread([&] { void* p = ByKey(first); lit = p && F::CallIgnite(p, 20.f); });
    Check(lit && WaitBurning(first, true), "the host's log burns");
    Mark("lit");
    if (!Await("lit_seen", nullptr, 30)) return;
    OnGameThread([&] { void* p = ByKey(first); if (p) F::CallExtinguish(p); });
    Check(WaitBurning(first, false), "the host's log went out");
    Mark("out");
    if (!Await("out_seen", nullptr, 30)) return;

    const std::string second = SpawnLog(true);
    Check(!second.empty(), "a second log stands");
    if (second.empty()) return;
    Mark("log2", second);
    if (!Await("clicked", nullptr, 90)) return;
    Check(WaitBurning(second, true), "the client's light burns the host's log");
    Mark("lit2");
    Await("client_done", nullptr, 60);
    OnGameThread([&] {
        for (const std::string& k : {first, second})
            if (void* p = ByKey(k)) { F::CallExtinguish(p); E::DestroyActor(p); }
    });
    UE_LOGI("prop-fire: DONE on host fails=%d", g_fails);
}

void RunClient() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::players::Registry::Get().Local() && coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(8000);
    Mark("client_ready");
    std::string first;
    if (!Await("log", &first, 120)) return;
    bool seen = false;
    for (int i = 0; i < 100 && !seen; ++i) {
        OnGameThread([&] { seen = ByKey(first) != nullptr; });
        if (!seen) ::Sleep(100);
    }
    Check(seen, "the host's log crossed");
    Mark("log_seen");
    if (!Await("lit", nullptr, 30)) return;
    Check(WaitBurning(first, true), "the host's flame burns on this copy");
    Mark("lit_seen");
    if (!Await("out", nullptr, 30)) return;
    Check(WaitBurning(first, false), "the host's flame went out on this copy");
    Mark("out_seen");

    std::string second;
    if (!Await("log2", &second, 60)) return;
    seen = false;
    for (int i = 0; i < 100 && !seen; ++i) {
        OnGameThread([&] { seen = ByKey(second) != nullptr; });
        if (!seen) ::Sleep(100);
    }
    Check(seen, "the second log crossed");
    const bool clicked = seen && LightWithLighter(second);
    Check(clicked, "this player's lighter clicked on the log");
    // The copy must not light itself: the light goes to the host, whose flame comes back.
    const bool selfLit = clicked && Burning(second);
    Check(!selfLit, "this copy did not light itself at the click");
    Mark("clicked");
    if (!Await("lit2", nullptr, 30)) return;
    Check(WaitBurning(second, true), "the host's flame from this light burns on this copy");
    OnGameThread([] { coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr); });
    Mark("client_done");
    UE_LOGI("prop-fire: DONE on client fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI PropFireThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("prop-fire: VERDICT ABORT -- VOTVCOOP_CAN_DIR is not set");
        return 0;
    }
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest
