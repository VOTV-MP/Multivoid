// coop/dev/kerfur_toggle.cpp -- see header.

#include "coop/dev/kerfur_toggle.h"

#include "coop/session/ini_config.h"
#include "coop/player/players_registry.h"
#include "coop/session/shutdown.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <windows.h>

#include <cmath>
#include <cstdint>

namespace coop::dev::kerfur_toggle {

namespace GT = ue_wrap::game_thread;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;

namespace {

// Find the nearest kerfur (NPC kerfurOmega_C or prop prop_kerfurOmega_C) to the local
// player and invoke its conversion verb via reflection -- a stand-in for the radial-menu
// turn_off (NPC) / turn_on (prop). The verb's BP body spawns the new form + K2_DestroyActor
// self exactly as the menu would, so kerfur_convert's poll detects the death, claims the
// ghost, and the adopt path runs. Game thread only (ProcessEvent + GUObjectArray walk).
void ToggleNearestKerfur() {
    void* local = coop::players::Registry::Get().Local();
    if (!local) {
        UE_LOGW("kerfur_toggle: no local player resolved -- ignoring");
        return;
    }
    void* npcCls  = R::FindClass(L"kerfurOmega_C");
    void* propCls = R::FindClass(L"prop_kerfurOmega_C");
    if (!npcCls && !propCls) {
        UE_LOGW("kerfur_toggle: kerfur classes not loaded yet -- ignoring");
        return;
    }
    const ue_wrap::FVector ploc = E::GetActorLocation(local);
    void* best = nullptr;
    bool  bestIsNpc = false;
    float bestD2 = 1e30f;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        if (!cls) continue;
        const bool isNpc  = npcCls  && R::IsDescendantOfAny(cls, &npcCls, 1);
        const bool isProp = !isNpc && propCls && R::IsDescendantOfAny(cls, &propCls, 1);
        if (!isNpc && !isProp) continue;
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        const ue_wrap::FVector loc = E::GetActorLocation(obj);
        const float dx = loc.X - ploc.X, dy = loc.Y - ploc.Y, dz = loc.Z - ploc.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = obj; bestIsNpc = isNpc; }
    }
    if (!best) {
        UE_LOGW("kerfur_toggle: no live kerfur found in the world -- ignoring");
        return;
    }
    const wchar_t* verb = bestIsNpc ? L"dropKerfurProp" : L"spawnKerfuro";
    void* fn = R::FindFunction(bestIsNpc ? npcCls : propCls, verb);
    if (!fn) {
        UE_LOGW("kerfur_toggle: verb %ls not found on the kerfur class -- ignoring", verb);
        return;
    }
    UE_LOGI("kerfur_toggle: TEST-toggling nearest kerfur actor=%p (%s, dist=%.0fcm) via %ls -- simulates radial-menu %s",
            best, bestIsNpc ? "NPC->prop" : "prop->NPC", std::sqrt(bestD2), verb,
            bestIsNpc ? "turn_off" : "turn_on");
    uint8_t frame[16] = {};  // dropKerfurProp / spawnKerfuro take no params
    R::CallFunction(best, fn, frame);
}

// Trigger-FILE watcher: mp.py's kerfurtoggle scenario creates the file on the client
// after a host kerfur has mirrored over; we toggle once + delete it (re-create to toggle
// again). The actual verb call is POSTED to the game thread.
DWORD WINAPI FileTriggerThread(LPVOID) {
    wchar_t triggerPath[512] = {};
    ::GetEnvironmentVariableW(L"VOTVCOOP_KERFUR_TOGGLE_TRIGGER", triggerPath, 512);
    if (triggerPath[0] == L'\0') return 0;  // nothing to watch
    int ticks = 0;
    while (!coop::shutdown::IsShuttingDown()) {
        if (++ticks >= 16) {  // ~250 ms (16 * 16 ms)
            ticks = 0;
            if (::GetFileAttributesW(triggerPath) != INVALID_FILE_ATTRIBUTES) {
                UE_LOGI("kerfur_toggle: trigger file seen -> toggling nearest kerfur (autonomous test)");
                GT::Post([] { ToggleNearestKerfur(); });
                ::DeleteFileW(triggerPath);  // one-shot per file creation
            }
        }
        ::Sleep(16);
    }
    return 0;
}

}  // namespace

void Init() {
    wchar_t probe[8] = {};
    if (::GetEnvironmentVariableW(L"VOTVCOOP_KERFUR_TOGGLE_TRIGGER", probe, 8) == 0) {
        return;  // dead unless the test env var is set
    }
    if (!::coop::ini_config::MasterEnabled()) {
        UE_LOGI("kerfur_toggle: disabled by master switch ([dev] enabled=0)");
        return;
    }
    if (HANDLE t = ::CreateThread(nullptr, 0, &FileTriggerThread, nullptr, 0, nullptr)) {
        ::CloseHandle(t);  // detached; loops until shutdown
    }
    UE_LOGI("kerfur_toggle: file-trigger ENABLED (VOTVCOOP_KERFUR_TOGGLE_TRIGGER) -- "
            "toggles nearest kerfur for conversion-adopt testing");
}

}  // namespace coop::dev::kerfur_toggle
