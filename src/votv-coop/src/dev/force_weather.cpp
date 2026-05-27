#include "dev/force_weather.h"

#include "coop/net/session.h"
#include "coop/shutdown.h"
#include "coop/weather_sync.h"
#include "dev/common.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"

#include <windows.h>

#include <atomic>

namespace dev::force_weather {

namespace GT = ue_wrap::game_thread;

namespace {

std::atomic<coop::net::Session*> g_session{nullptr};

// Local toggle state. F5 flips this, then dispatches DebugForceSnow with the
// new value. Default false (matches the cycle's initial isSnow=false).
std::atomic<bool> g_snowOn{false};

bool KeyDown(int vk) { return (::GetAsyncKeyState(vk) & 0x8000) != 0; }

// Hotkey: polls F5 (rising edge). Host-only -- pressing F5 on the client is
// a no-op (the weather is host-authoritative; clients receive state, never
// emit it). Mirrors teleport_client's host-only gate.
DWORD WINAPI HotkeyThread(LPVOID) {
    bool prevF5 = false;
    while (!coop::shutdown::IsShuttingDown()) {
        const bool f5 = ::dev::IsOurWindowForeground() && KeyDown(VK_F5);
        if (f5 && !prevF5) {
            auto* s = g_session.load(std::memory_order_acquire);
            if (s && s->role() == coop::net::Role::Host) {
                const bool newState = !g_snowOn.load(std::memory_order_acquire);
                g_snowOn.store(newState, std::memory_order_release);
                GT::Post([newState] {
                    const bool ok = coop::weather_sync::DebugForceSnow(newState);
                    if (!ok) {
                        UE_LOGW("force_weather: DebugForceSnow(%d) failed -- "
                                "cycle not yet live or UFunction unresolved",
                                newState ? 1 : 0);
                    }
                });
                UE_LOGI("force_weather: F5 pressed -- snow toggled to %d", newState ? 1 : 0);
            } else if (s) {
                UE_LOGI("force_weather: F5 pressed but local role is Client -- "
                        "weather is host-authoritative; press F5 on host to toggle snow");
            }
        }
        prevF5 = f5;
        ::Sleep(8);
    }
    return 0;
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Init() {
    if (!::dev::MasterEnabled()) {
        UE_LOGI("force_weather: disabled by master switch ([dev] enabled=0)");
        return;
    }
    if (!::dev::IsIniKeyTrue("devkeys")) {
        UE_LOGI("force_weather: disabled (set [dev] devkeys=1 in votv-coop.ini to enable F5)");
        return;
    }
    if (HANDLE t = ::CreateThread(nullptr, 0, &HotkeyThread, nullptr, 0, nullptr)) {
        ::CloseHandle(t);
    }
    UE_LOGI("force_weather: ENABLED (host F5 -> toggle snow on both peers via intComs_triggerSnow)");
}

}  // namespace dev::force_weather
