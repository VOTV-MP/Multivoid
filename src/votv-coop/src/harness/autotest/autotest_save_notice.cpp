// harness/autotest/autotest_save_notice.cpp -- TEST-ONLY: what every player hears of the host's saves.
//
// The host runs the game's own mainGamemode.save three ways with a client joined: with no event, with an event
// counted by the game's own lib_C.setEvent, and with no event again. It also runs save with bypassEvent during the
// event, which is the path that writes without gathering. Each step logs `save-drill: STEP <name> event=<0|1>`.
// The drill judges itself: the host's own outcome counts must equal what the steps expect and the client's heard
// counts must equal the host's, logged as `save-drill: VERDICT PASS|FAIL`. Gated by VOTVCOOP_RUN_SAVE_NOTICE; the
// peers' shared folder is VOTVCOOP_CAN_DIR. Every wait is on a game state with a bound, never on a clock.

#include "harness/autotest.h"
#include "harness/autotest/peer_signal.h"

#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/save/save_notice.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/save_capture.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace PS = harness::autotest::peer_signal;
using PS::Await;
using PS::AwaitText;
using PS::Mark;
using PS::WaitFor;

// Steps 1 and 4 write the world with no event running: two saves that took.
constexpr uint32_t kWantOk = 2;
// Step 2 is refused by the game during the event and step 3 bypasses the event and writes without gathering: both
// report that the save did not take.
constexpr uint32_t kWantFailed = 2;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

coop::save_notice::Counts ReadCounts() {
    coop::save_notice::Counts c;
    OnGameThread([&] { c = coop::save_notice::GetCounts(); });
    return c;
}

// The host's save notices so far, ok and failed together.
uint32_t HostNotices() {
    const auto c = ReadCounts();
    return c.savedOk + c.savedFailed;
}

void SetEvent(bool on) {
    OnGameThread([&] {
        void* const gm = ue_wrap::world_singleton::Gamemode();
        void* const lib = R::FindClassDefaultObject(L"lib_C");
        void* const fn = lib ? R::FindFunction(R::ClassOf(lib), L"setEvent") : nullptr;
        if (!gm || !fn) { UE_LOGW("save-drill: setEvent did not resolve"); return; }
        ue_wrap::ParamFrame f(fn);
        f.Set<bool>(L"isEventActive", on);
        f.Set<bool>(L"deactivateAmbientTrack", false);
        f.Set<void*>(L"__WorldContext", gm);
        const bool ok = f.valid() && ue_wrap::Call(lib, f);
        UE_LOGI("save-drill: setEvent(%d) ran (ok=%d)", on ? 1 : 0, ok ? 1 : 0);
    });
}

// The game counts the event as the drill set it: the next save reads that state.
bool WaitEventState(bool on) {
    const auto want = on ? ue_wrap::save_capture::EventState::On : ue_wrap::save_capture::EventState::Off;
    return WaitFor("the game's event state", 10, [&] {
        auto got = ue_wrap::save_capture::EventState::Unknown;
        OnGameThread([&] { got = ue_wrap::save_capture::GameEventState(); });
        return got == want;
    });
}

// One save call, then the wait for its notice: the game's save runs across latent delays and counts when it reports.
bool Save(const char* step, bool bypassEvent) {
    const uint32_t before = HostNotices();
    OnGameThread([&] {
        void* const gm = ue_wrap::world_singleton::Gamemode();
        void* const fn = gm ? R::FindFunction(R::ClassOf(gm), L"save") : nullptr;
        const auto ev = ue_wrap::save_capture::GameEventState();
        UE_LOGI("save-drill: STEP %hs event=%d bypass=%d", step,
                ev == ue_wrap::save_capture::EventState::On ? 1 : 0, bypassEvent ? 1 : 0);
        if (!fn) { UE_LOGW("save-drill: save did not resolve"); return; }
        ue_wrap::ParamFrame f(fn);
        f.Set<bool>(L"quicksave", false);
        f.Set<bool>(L"bypassEvent", bypassEvent);
        f.Set<bool>(L"overwriteSubsave", false);
        const bool ok = f.valid() && ue_wrap::Call(gm, f);
        UE_LOGI("save-drill: save ran (ok=%d)", ok ? 1 : 0);
    });
    return WaitFor("the save's notice", 30, [&] { return HostNotices() > before; });
}

bool ParsePair(const std::string& text, uint32_t& ok, uint32_t& failed) {
    const char* const begin = text.c_str();
    char* mid = nullptr;
    const unsigned long a = std::strtoul(begin, &mid, 10);
    if (mid == begin) return false;
    char* end = nullptr;
    const unsigned long b = std::strtoul(mid, &end, 10);
    if (end == mid) return false;
    ok = static_cast<uint32_t>(a);
    failed = static_cast<uint32_t>(b);
    return true;
}

void RunHost() {
    if (!Await("client_ready")) return;
    if (!Save("1-no-event", false)) return;
    SetEvent(true);
    if (!WaitEventState(true)) return;
    if (!Save("2-event", false)) return;
    if (!Save("3-event-bypass", true)) return;
    SetEvent(false);
    if (!WaitEventState(false)) return;
    if (!Save("4-no-event-again", false)) return;
    const auto host = ReadCounts();
    Mark("host_done", std::to_string(host.savedOk) + " " + std::to_string(host.savedFailed));
    std::string heardText;
    if (!AwaitText("client_counts", heardText)) return;
    uint32_t heardOk = 0, heardFailed = 0;
    const bool parsed = ParsePair(heardText, heardOk, heardFailed);
    const bool hostRight = host.savedOk == kWantOk && host.savedFailed == kWantFailed;
    const bool clientRight = parsed && heardOk == host.savedOk && heardFailed == host.savedFailed;
    if (hostRight && clientRight) {
        UE_LOGI("save-drill: VERDICT PASS");
    } else {
        UE_LOGW("save-drill: VERDICT FAIL host=%u %u client=%u %u want=%u %u", host.savedOk, host.savedFailed,
                heardOk, heardFailed, kWantOk, kWantFailed);
    }
    UE_LOGI("save-drill: DONE");   // after the verdict: a run ends on DONE
}

void RunClient() {
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
        return;
    Mark("client_ready");
    std::string hostText;
    if (!AwaitText("host_done", hostText)) return;
    uint32_t wantOk = 0, wantFailed = 0;
    if (!ParsePair(hostText, wantOk, wantFailed)) UE_LOGW("save-drill: host_done carries '%hs'", hostText.c_str());
    // A timeout here is the FAIL itself (a notice never arrived), so this wait logs nothing beyond the verdict.
    WaitFor("the host's notices heard", 30, [&] {
        const auto c = ReadCounts();
        return c.heardOk == wantOk && c.heardFailed == wantFailed;
    }, false);
    const auto heard = ReadCounts();
    Mark("client_counts", std::to_string(heard.heardOk) + " " + std::to_string(heard.heardFailed));
    UE_LOGI("save-drill: DONE");
}

}  // namespace

DWORD WINAPI SaveNoticeThread(LPVOID /*arg*/) {
    if (!PS::Begin("save-drill")) return 0;
    if (IsClientRole()) RunClient();
    else                RunHost();
    return 0;
}

}  // namespace harness::autotest
