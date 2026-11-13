// coop/dev/pause_quit_drill.cpp -- see coop/dev/pause_quit_drill.h.

#include "coop/dev/pause_quit_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/run_end_travel.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "coop/session/rig_verdict.h"
#include "ui/multiplayer_menu.h"    // IsPauseMenuOpen

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/save_capture.h"
#include "ue_wrap/world/world_singleton.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::pause_quit_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;
using Clock = std::chrono::steady_clock;

enum class Arm : uint8_t { Off, Client, Host };

Arm ArmNow() {
    static const Arm arm = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::pause_quit_drill);
        return v == "client" ? Arm::Client : v == "host" ? Arm::Host : Arm::Off;
    }();
    return arm;
}

// Armed: waiting for the session to settle. WaitMenu: Escape sent, waiting for the pause menu.
// Done: the click was made, or the drill gave up. The latch is per PROCESS: the quit ends the
// session, and a drill that started over with the next one would quit again.
// With pause_quit_event the drill first starts an event and waits for the game to count it (WaitEvent).
enum class Phase : uint8_t { Armed, WaitEvent, WaitMenu, Done };
Phase g_phase = Phase::Armed;

bool EventArm() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::pause_quit_event);
    return on;
}
Clock::time_point g_escapeAt{};

// A fail-safe bound on a wait that is otherwise on readiness (the menu opening), not a clock the
// drill's verdict rests on.
constexpr auto kAbortAfter = std::chrono::seconds(30);

const char* Me() { return ArmNow() == Arm::Host ? "host" : "client"; }

void Abort(const char* why) {
    UE_LOGW("[PAUSE-QUIT] %s ABORT: %s", Me(), why);
    coop::rig_verdict::Say("pause-quit", coop::rig_verdict::Outcome::Invalid, "%s", why);
    g_phase = Phase::Done;
}

// The game's own event count raised the way a creature or a recording raises it: lib_C::setEvent(true). The
// pause menu's Main menu button then meets a running event, which is what the watch in coop/player/leave_in_event
// answers. False when the call did not resolve or run.
bool StartEvent() {
    void* const gm = ue_wrap::world_singleton::Gamemode();
    void* const lib = R::FindClassDefaultObject(L"lib_C");
    void* const fn = lib ? R::FindFunction(R::ClassOf(lib), L"setEvent") : nullptr;
    if (!gm || !fn) return false;
    ue_wrap::ParamFrame f(fn);
    f.Set<bool>(L"isEventActive", true);
    f.Set<bool>(L"deactivateAmbientTrack", false);
    f.Set<void*>(L"__WorldContext", gm);
    return f.valid() && ue_wrap::Call(lib, f);
}

// What `rig: READY joined` stands on, the settled test the chat drill's client uses.
bool ClientReady(coop::net::Session& s) {
    return s.connected() && coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// Some slot's join traffic is DELIVERED (the settled-client test of the ban drill), so the host
// quits a joined peer's session, not one mid-join.
bool HostReady(coop::net::Session& s) {
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        if (s.IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot) &&
            !coop::join_window_baseline::IsLateWindowOpen(slot) && s.SlotReliableIdle(slot))
            return true;
    }
    return false;
}

// A zeroed parameter frame for `fn`, as the reflected input-event calls take.
std::vector<uint8_t> ZeroFrame(void* fn) {
    const int32_t size = R::FunctionFrameSize(fn);
    return std::vector<uint8_t>(size > 0 ? static_cast<size_t>(size) : 0, 0u);
}

// The player's Escape event, which opens the pause menu unless the player is dreaming, asleep, out
// of bounds, dead or ragdolled. The local player is the registry's, never a lookup by class: a
// puppet is a mainPlayer_C too.
void SendEscape() {
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;  // not yet; the next tick asks again
    void* fn = R::FindFunction(R::ClassOf(player), P::name::MainPlayerEscapeFn);
    if (!fn) return Abort("the Escape event was not found on mainPlayer_C");
    std::vector<uint8_t> frame = ZeroFrame(fn);
    R::CallFunction(player, fn, frame.empty() ? nullptr : frame.data());
    UE_LOGI("[PAUSE-QUIT] %s: Escape sent", Me());
    g_escapeAt = Clock::now();
    g_phase = Phase::WaitMenu;
}

// The click handler of the "Main menu" button. It is synchronous: its script calls
// lib_C::loadLevel("menu") unless an event is active or the camera is out of bounds, and the
// run-ending watch counts that call before this returns, so an unchanged count is a refusal.
void PressMainMenu(void* menu) {
    void* fn = R::FindFunction(R::ClassOf(menu), P::name::PauseMenuQuitClickFn);
    if (!fn) return Abort("the Main menu click handler was not found on ui_menu_C");
    const unsigned long long before = coop::player::run_end_travel::MenuTravelsSeen();
    const bool eventNow = ue_wrap::save_capture::GameEventState() == ue_wrap::save_capture::EventState::On;
    std::vector<uint8_t> frame = ZeroFrame(fn);
    R::CallFunction(menu, fn, frame.empty() ? nullptr : frame.data());
    g_phase = Phase::Done;
    if (coop::player::run_end_travel::MenuTravelsSeen() == before) {
        UE_LOGW("[PAUSE-QUIT] %s FAIL: the click did not reach loadLevel (an event is active, or out "
                "of bounds)", Me());
        coop::rig_verdict::Say("pause-quit", coop::rig_verdict::Outcome::Fail,
                               "the Main menu press did not reach the menu travel (the game counts an event: %d)",
                               eventNow ? 1 : 0);
        return;
    }
    UE_LOGI("[PAUSE-QUIT] %s: Main menu pressed", Me());
    coop::rig_verdict::Say("pause-quit", coop::rig_verdict::Outcome::Pass,
                           "the Main menu press reached the menu travel (the game counts an event: %d)", eventNow ? 1 : 0);
}

void WaitForMenu() {
    if (Clock::now() - g_escapeAt > kAbortAfter)
        return Abort("the pause menu did not open (in bed, sitting, an interface open?)");
    if (!coop::multiplayer_menu::IsPauseMenuOpen()) return;
    // The game's own reference to the menu; null for a tick or two after a level change, then asked again.
    if (void* menu = coop::multiplayer_menu::PauseMenu()) PressMainMenu(menu);
}

}  // namespace

void Tick(coop::net::Session* s) {
    const Arm arm = ArmNow();
    if (arm == Arm::Off || g_phase == Phase::Done || !s || !s->running()) return;
    const bool host = s->role() == coop::net::Role::Host;
    if (host != (arm == Arm::Host)) return;
    if (g_phase == Phase::Armed) {
        if (!(host ? HostReady(*s) : ClientReady(*s))) return;
        if (!EventArm()) return SendEscape();
        if (!StartEvent()) return Abort("lib_C::setEvent did not resolve or run");
        UE_LOGI("[PAUSE-QUIT] %s: event started", Me());
        g_escapeAt = Clock::now();
        g_phase = Phase::WaitEvent;
        return;
    }
    if (g_phase == Phase::WaitEvent) {
        // The game's own getEvent, asked until it counts the event; a bound, not a clock the verdict rests on.
        if (ue_wrap::save_capture::GameEventState() == ue_wrap::save_capture::EventState::On) return SendEscape();
        if (Clock::now() - g_escapeAt > kAbortAfter) Abort("the game did not count the event the drill started");
        return;
    }
    WaitForMenu();
}

void OnDisconnect() {
    if (g_phase == Phase::WaitMenu || g_phase == Phase::WaitEvent) g_phase = Phase::Done;
}

}  // namespace coop::dev::pause_quit_drill
