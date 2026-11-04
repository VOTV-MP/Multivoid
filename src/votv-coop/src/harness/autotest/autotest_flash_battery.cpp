// harness/autotest/autotest_flash_battery.cpp -- TEST-ONLY: a flashlight whose battery runs out goes dark on the
// other peer too.
//
// Each peer in turn switches its flashlight on and leaves a sliver of charge, and the game's own tick drains it: at
// empty the tick hides the light and clears the bool without calling a function. The other peer reads its puppet's
// light with the flashlight on and after the drain. Client first, then host; each direction logs
// `flash-battery: VERDICT <owner> PASS|FAIL`. Gated by VOTVCOOP_RUN_FLASH_BATTERY; the peers' shared folder is
// VOTVCOOP_CAN_DIR. Signals count only when written after this drill began, and every wait is on a game state with
// a bound, never on a clock.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/dev/flashlight_setup.h"
#include "coop/player/item_activate.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <atomic>
#include <fstream>
#include <string>

namespace harness::autotest {
namespace {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;

std::string g_dir;
uint64_t g_start = 0;  // FILETIME of this peer's drill start

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Mark(const std::string& name) {
    std::ofstream(g_dir + "\\" + name, std::ios::binary | std::ios::trunc) << "x";
}

// A signal written before this peer's drill began is an earlier run's, so it does not count.
bool WrittenSinceStart(const std::string& path) {
    const int n = ::MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
    if (n <= 0) return false;
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, w.data(), n);
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!::GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &a)) return false;
    const uint64_t t = (static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    return t >= g_start;
}

bool Await(const std::string& name, int seconds = 240) {
    for (int i = 0; i < seconds * 10; ++i) {
        const std::string path = g_dir + "\\" + name;
        std::ifstream f(path, std::ios::binary);
        std::string s;
        if (f && std::getline(f, s) && !s.empty() && WrittenSinceStart(path)) return true;
        ::Sleep(100);
    }
    UE_LOGW("flash-battery: VERDICT ABORT -- no '%hs' from the other peer", name.c_str());
    return false;
}

// Polls a game state every 100 ms for at most boundSeconds. A timeout is an ABORT unless the caller judges it.
template <class Fn>
bool WaitFor(const char* what, int boundSeconds, Fn pred, bool abortOnTimeout = true) {
    for (int i = 0; i < boundSeconds * 10; ++i) {
        if (pred()) return true;
        ::Sleep(100);
    }
    if (abortOnTimeout) UE_LOGW("flash-battery: VERDICT ABORT -- %hs not within %ds", what, boundSeconds);
    return false;
}

// The light's bVisible by its reflected name, the flag the game's SetVisibility writes.
bool Visible(void* light) {
    int32_t byte = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(light), L"bVisible", byte, mask) || byte < 0) return false;
    return (static_cast<const uint8_t*>(light)[byte] & mask) != 0;
}

// The owner's half: on, seen on, drained by the game's tick.
void RunOwner(const std::string& who) {
    void* local = nullptr;
    OnGameThread([&] {
        local = coop::players::Registry::Get().Local();
        if (local) coop::dev::flashlight_setup::EnsureFlashlightReady(local);
    });
    if (!local) { UE_LOGW("flash-battery: VERDICT ABORT -- no local player"); return; }
    if (!WaitFor("the local flashlight readable", 30, [&] {
            bool ok = false;
            OnGameThread([&] {
                E::MainPlayerFlashlightState fl{};
                ok = E::ReadMainPlayerFlashlightState(local, fl);
            });
            return ok;
        }))
        return;
    bool on = coop::item_activate::DebugForceToggle(local);
    if (!on) on = coop::item_activate::DebugForceToggle(local);
    UE_LOGI("flash-battery: %hs switched its flashlight on (on=%d)", who.c_str(), on ? 1 : 0);
    Mark(who + "_on");
    if (!Await(who + "_seen_on")) return;
    OnGameThread([&] { ue_wrap::vitals::Write(ue_wrap::vitals::Field::Battery, 0.02f); });
    if (!WaitFor("the game's tick switching the flashlight off", 30, [&] {
            bool off = false;
            OnGameThread([&] {
                E::MainPlayerFlashlightState fl{};
                off = E::ReadMainPlayerFlashlightState(local, fl) && !fl.flashlight;
            });
            return off;
        }))
        return;
    OnGameThread([&] {
        E::MainPlayerFlashlightState fl{};
        const bool read = E::ReadMainPlayerFlashlightState(local, fl);
        void* const light = E::GetMainPlayerLightR(local);
        UE_LOGI("flash-battery: %hs after the drain: flashlight=%d lightVisible=%d", who.c_str(),
                read && fl.flashlight ? 1 : 0, light && Visible(light) ? 1 : 0);
    });
    Mark(who + "_drained");
}

// Logs only when the (visible, intensity>0) pair changes, so a poll keeps the transitions and not the spam.
bool PuppetLit(uint8_t ownerSlot) {
    bool lit = false;
    OnGameThread([&] {
        coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(ownerSlot);
        void* const actor = (p && p->valid()) ? p->GetActor() : nullptr;
        void* const light = actor ? E::GetMainPlayerLightR(actor) : nullptr;
        E::FlashlightSnapshot snap{};
        if (!light || !E::ReadFlashlightSnapshot(light, snap)) return;
        const bool visible = Visible(light);
        lit = visible && snap.intensity > 0.f;
        static int lastVisible = -1;
        static int lastPositive = -1;
        const int vis = visible ? 1 : 0;
        const int pos = snap.intensity > 0.f ? 1 : 0;
        if (vis != lastVisible || pos != lastPositive) {
            lastVisible = vis;
            lastPositive = pos;
            UE_LOGI("flash-battery: puppet light bVisible=%d intensity=%.2f (offset read %d)", vis, snap.intensity,
                    snap.visible ? 1 : 0);
        }
    });
    return lit;
}

// The other peer's half: the owner's puppet, lit and then dark.
void RunWatcher(const std::string& who, uint8_t ownerSlot) {
    if (!Await(who + "_on")) return;
    const bool before = WaitFor("the owner's puppet lit", 30, [&] { return PuppetLit(ownerSlot); });
    UE_LOGI("flash-battery: %hs's puppet with the flashlight on: lit=%d", who.c_str(), before ? 1 : 0);
    Mark(who + "_seen_on");
    if (!Await(who + "_drained")) return;
    // A timeout is the FAIL itself (the light stays lit), so this wait logs nothing beyond the verdict.
    const bool afterDark = WaitFor("the owner's puppet dark", 30, [&] { return !PuppetLit(ownerSlot); }, false);
    UE_LOGI("flash-battery: %hs's puppet after the drain: lit=%d", who.c_str(), afterDark ? 0 : 1);
    UE_LOGI("flash-battery: VERDICT %hs %hs", who.c_str(), before && afterDark ? "PASS" : "FAIL");
    Mark(who + "_judged");
}

}  // namespace

DWORD WINAPI FlashBatteryThread(LPVOID /*arg*/) {
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    g_start = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("flash-battery: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (IsClientRole()) {
        bool ready = false;
        for (int i = 0; i < 480 && !ready; ++i) {
            OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
            if (!ready) ::Sleep(500);
        }
        if (!WaitFor("the host's puppet", 60, [&] {
                bool have = false;
                OnGameThread([&] {
                    coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
                    have = p && p->valid();
                });
                return have;
            }))
            return 0;
        RunOwner("client");
        if (!Await("client_judged")) return 0;
        RunWatcher("host", 0);
    } else {
        RunWatcher("client", 1);
        if (!Await("client_judged")) return 0;
        RunOwner("host");
        if (!Await("host_judged")) return 0;
    }
    UE_LOGI("flash-battery: DONE");
    return 0;
}

}  // namespace harness::autotest
