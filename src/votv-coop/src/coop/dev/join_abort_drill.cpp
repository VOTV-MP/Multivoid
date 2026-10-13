// coop/dev/join_abort_drill.cpp -- see coop/dev/join_abort_drill.h.

#include "coop/dev/join_abort_drill.h"

#include "coop/config/config.h"
#include "coop/items/player_inventory_sync.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/session_manager.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/world_identity.h"
#include "ui/server_browser_surface.h"

#include <string>

namespace coop::dev::join_abort_drill {
namespace {

namespace JP = coop::join_progress;
namespace PIS = coop::player_inventory_sync;

enum class Arm { Off, Download, World };

Arm WhichArm() {
    static const Arm a = [] {
        const std::string s = coop::config::ResolveEnum(::coop::config_registry::rows::join_abort);
        if (s == "download") return Arm::Download;
        if (s == "world") return Arm::World;
        return Arm::Off;
    }();
    return a;
}

// Game thread only (both call sites are the pump composite / PostMenuTick composite), so plain state.
bool g_joinSeen = false;        // a join became active once -- so an end before a press is INCONCLUSIVE, not idle
bool g_pressed = false;         // Cancel pressed at the arm's window
bool g_announceJudged = false;  // world arm: the post-abort announce-clear check passed
bool g_atMenu = false;          // world arm: the abort's end reached the menu with the browser
bool g_rejoinDialed = false;    // world arm: the rejoin ConnectDirect fired
bool g_done = false;            // terminal: the verdict is printed, nothing more to do

}  // namespace

void OnFrameTail() {
    const Arm arm = WhichArm();
    if (arm == Arm::Off || g_done) return;

    if (JP::Active()) g_joinSeen = true;

    // The rejoin's success (world arm): the second join reaches world-ready.
    if (g_rejoinDialed && coop::net_pump::HasAnnouncedWorldReady()) {
        UE_LOGI("[JOIN-ABORT] rejoin PASS");
        g_done = true;
        return;
    }

    if (g_pressed) return;  // one press per run

    if (arm == Arm::Download) {
        // Window A: the world blob is streaming and this client's apply blob is held; the host's
        // stall_world_stream_at_pct keeps us here so the press lands inside the window.
        if (JP::CurrentPhase() == JP::Phase::Downloading && PIS::HasPendingApply()) {
            JP::RequestCancel();
            g_pressed = true;
            UE_LOGI("[JOIN-ABORT] download: pressed Cancel (phase=Downloading, apply blob held)");
        }
    } else {  // Arm::World
        // Window B: this client announced world-ready and the join is still active; the host's
        // hold_snapshot_sec keeps the stretch open.
        if (coop::net_pump::HasAnnouncedWorldReady() && JP::Active()) {
            JP::RequestCancel();
            g_pressed = true;
            UE_LOGI("[JOIN-ABORT] world: pressed Cancel (announced world-ready, join active)");
        }
    }
}

void OnNoSession(bool leaving) {
    const Arm arm = WhichArm();
    if (arm == Arm::Off || g_done) return;

    // The no-session branch (so nothing is running) with no press yet: the window the arm needed never
    // opened -- the host knob was not set, or the join ended first. Guarded by g_joinSeen so the menu
    // before the first join is not judged, and by !Active() so a join still in flight is not.
    if (!g_pressed) {
        if (g_joinSeen && !JP::Active()) {
            UE_LOGW("[JOIN-ABORT] %s INCONCLUSIVE: the join ended before its press window",
                    arm == Arm::Download ? "download" : "world");
            g_done = true;
        }
        return;
    }

    // Pressed, and no session now runs: EndClientSession stopped it and ran TearDownSession before
    // this branch. Judge the teardown by what it owed, on the post-teardown state.
    if (arm == Arm::Download) {
        // The per-player apply blob must be gone (the teardown's DisconnectAll clears it). On the bytes
        // before the seam the abort only Stop()'d, so the blob survives and the next join inherits it.
        if (!PIS::HasPendingApply())
            UE_LOGI("[JOIN-ABORT] download PASS");
        else
            UE_LOGE("[JOIN-ABORT] download FAIL: apply-blob-survived");
        g_done = true;  // no rejoin leg: the host's stall hits every joiner
        return;
    }

    // World arm. First, the world-ready announce state must be clear after the stop (the teardown's
    // ForgetWorldReadyAnnounce). On the bytes before the seam it stays stale.
    if (!g_announceJudged) {
        if (coop::net_pump::HasAnnouncedWorldReady()) {
            UE_LOGE("[JOIN-ABORT] world FAIL: announce-stale");
            g_done = true;  // red: no rejoin
            return;
        }
        UE_LOGI("[JOIN-ABORT] world: announce cleared after the abort");
        g_announceJudged = true;
    }

    // Then where the player ended: the abort leaves the host's world for the menu, where the browser opens.
    // The first pass here comes after the stop site returned, so a leave the end began is already under way;
    // on bytes where the end begins none, that pass finds the world standing and nothing leaving it. Waits
    // while a leave runs, or while the menu stands and the browser has not shown yet.
    if (!g_atMenu) {
        namespace WI = ue_wrap::world_identity;
        const WI::WorldKind kind = WI::CurrentWorldKind();
        if (kind != WI::WorldKind::Other) {
            if (leaving) return;
            UE_LOGE("[JOIN-ABORT] world FAIL: stayed-in-world");
            g_done = true;
            return;
        }
        if (!ui::server_browser_surface::IsOpen()) return;
        UE_LOGI("[JOIN-ABORT] world: at the menu with the browser after the abort -- rejoining");
        g_atMenu = true;
    }

    // Then dial the host again (once), as rehost_rejoin does; the pass is the second world-ready, noted
    // in OnFrameTail. The start it queues is drained by the play loop's no-session block.
    if (!g_rejoinDialed) {
        const std::string addr =
            coop::config::ResolveString(::coop::config_registry::rows::net_peer) + ":" +
            std::to_string(coop::config::ResolveInt(::coop::config_registry::rows::net_port));
        const bool accepted = coop::session_manager::ConnectDirect(addr);
        UE_LOGI("[JOIN-ABORT] world: dialing %s again (accepted=%d)",
                ue_wrap::log::Addr(addr).c_str(), accepted ? 1 : 0);
        g_rejoinDialed = true;
    }
}

}  // namespace coop::dev::join_abort_drill
