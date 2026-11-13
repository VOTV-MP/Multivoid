// coop/dev/vidcam_hint_drill.cpp -- see coop/dev/vidcam_hint_drill.h.

#include "coop/dev/vidcam_hint_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_hand.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/vidcam_hint.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "coop/session/rig_verdict.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/video_camera.h"
#include "ue_wrap/engine/world_identity.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace coop::dev::vidcam_hint_drill {
namespace {

namespace VC = ue_wrap::video_camera;
using Clock = std::chrono::steady_clock;

enum class Arm : uint8_t { Off, Host, Client };

Arm ArmNow() {
    static const Arm arm = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::vidcam_hint_drill);
        return v == "host" ? Arm::Host : v == "client" ? Arm::Client : Arm::Off;
    }();
    return arm;
}

// Armed: waiting for the peer to be in a world with its player. WaitShown: the camera is in the hand, waiting for the
// overlay state to say the viewfinder. WaitHidden: the hand is empty, waiting for it to say nothing. Done: the verdict
// is said. The latch is per PROCESS: the drill's one verdict is its end.
enum class Phase : uint8_t { Armed, WaitShown, WaitHidden, Done };
Phase g_phase = Phase::Armed;
Clock::time_point g_since{};

// The video camera's list_props row name and its class. Its overlay is shown while the player holds one.
constexpr const wchar_t* kItem = L"vidcam";
constexpr const wchar_t* kClass = L"prop_vidcam_C";

// A fail-safe bound on a wait that is otherwise on the overlay state itself, not a clock the verdict rests on.
constexpr auto kBound = std::chrono::seconds(15);

const char* Me() { return ArmNow() == Arm::Host ? "host" : "client"; }

const char* Name(VC::View v) {
    return v == VC::View::Viewfinder ? "viewfinder" : v == VC::View::Recording ? "recording" : "none";
}

void Finish(coop::rig_verdict::Outcome outcome, const char* why) {
    const char* const word = outcome == coop::rig_verdict::Outcome::Pass   ? "PASS"
                             : outcome == coop::rig_verdict::Outcome::Fail ? "FAIL"
                                                                           : "INVALID";
    UE_LOGI("[VIDCAM-HINT-DRILL] %s %s: %s", Me(), word, why);
    coop::rig_verdict::Say("vidcam-hint", outcome, "%s", why);
    g_phase = Phase::Done;
}

bool Ready(coop::net::Session& s) {
    if (coop::players::Registry::Get().Local() == nullptr) return false;
    if (ue_wrap::world_identity::CurrentWorldKind() != ue_wrap::world_identity::WorldKind::Gameplay) return false;
    return s.role() == coop::net::Role::Host || coop::net_pump::HasAnnouncedWorldReady();
}

}  // namespace

void Tick(coop::net::Session* s) {
    const Arm arm = ArmNow();
    if (arm == Arm::Off || g_phase == Phase::Done || !s || !s->running()) return;
    if ((s->role() == coop::net::Role::Host) != (arm == Arm::Host)) return;
    const VC::View shown = coop::vidcam_hint::Current();
    switch (g_phase) {
    case Phase::Armed:
        if (!Ready(*s)) return;
        if (!coop::dev::drill_hand::TakeIntoHandNow(kItem, kClass))
            return Finish(coop::rig_verdict::Outcome::Invalid, "the video camera could not be put into the hand");
        UE_LOGI("[VIDCAM-HINT-DRILL] %s: camera in hand, overlay state %s", Me(), Name(shown));
        g_since = Clock::now();
        g_phase = Phase::WaitShown;
        return;
    // Each wait ends on the game's own overlay: ReadView reads the widget's visibility the manager's updUI wrote, so
    // the tick that reads the new state is after the updUI that wrote it, and the watch's post callback (which ran at
    // that updUI's end) has had its turn. The HUD's published state then equals it (PASS path) or does not (FAIL, decided
    // there, since nothing later changes it). The bound only says INVALID: the game never changed its overlay.
    case Phase::WaitShown: {
        VC::View truth = VC::View::None;
        if (!VC::ReadView(truth)) truth = VC::View::None;
        if (truth == VC::View::Viewfinder) {
            if (shown != VC::View::Viewfinder)
                return Finish(coop::rig_verdict::Outcome::Fail,
                              (std::string("the game's overlay shows the viewfinder and the HUD's state says ") + Name(shown))
                                  .c_str());
            UE_LOGI("[VIDCAM-HINT-DRILL] %s: the viewfinder shows; emptying the hand", Me());
            if (!coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr))
                return Finish(coop::rig_verdict::Outcome::Invalid, "the hand could not be emptied");
            g_since = Clock::now();
            g_phase = Phase::WaitHidden;
        } else if (Clock::now() - g_since > kBound) {
            Finish(coop::rig_verdict::Outcome::Invalid,
                   (std::string("with the camera in hand the game's overlay never showed the viewfinder (it reads ") +
                    Name(truth) + ")").c_str());
        }
        return;
    }
    case Phase::WaitHidden: {
        VC::View truth = VC::View::None;
        if (!VC::ReadView(truth)) truth = VC::View::None;
        if (truth == VC::View::None) {
            if (shown != VC::View::None)
                return Finish(coop::rig_verdict::Outcome::Fail,
                              (std::string("the game's overlay is gone and the HUD's state says ") + Name(shown)).c_str());
            Finish(coop::rig_verdict::Outcome::Pass, "the viewfinder showed with the camera in hand and cleared with it away");
        } else if (Clock::now() - g_since > kBound) {
            Finish(coop::rig_verdict::Outcome::Invalid,
                   (std::string("with the hand empty the game's overlay never cleared (it reads ") + Name(truth) + ")").c_str());
        }
        return;
    }
    case Phase::Done:
        return;
    }
}

void OnDisconnect() {
    if (g_phase == Phase::WaitShown || g_phase == Phase::WaitHidden) g_phase = Phase::Done;
}

}  // namespace coop::dev::vidcam_hint_drill
