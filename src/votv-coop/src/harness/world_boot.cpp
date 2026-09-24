// harness/world_boot.cpp -- see harness/world_boot.h.

#include "harness/world_boot.h"

#include "harness/pump.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/items/player_inventory_sync.h"
#include "coop/net/end_reason.h"
#include "coop/net/ice_policy.h"   // a host's policy refusal, before its world loads
#include "coop/net/session.h"
#include "coop/player/player_profile_store.h"
#include "coop/save/save_transfer.h"
#include "coop/session/join_progress.h"
#include "coop/session/session_manager.h"
#include "coop/session/shutdown.h"
#include "coop/session/world_load_episode.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/save_browser.h"
#include "ui/host_session_settings.h"   // it paints a failed host's reason on itself
#include "ui/host_window_native.h"      // ...and so does step one
#include "ui/server_browser_surface.h"  // WHICH browser this session uses
#include "ue_wrap/world/game_mode.h"

#include <windows.h>

#include <atomic>
#include <memory>
#include <string>

namespace harness::world_boot {
namespace {

namespace GT  = ue_wrap::game_thread;
namespace cfg = coop::config;

// The process's one session object, read for its role and state and stopped when a join this
// module is driving cannot reach a world.
coop::net::Session& S() { return harness::session_runtime::Session(); }

// A join that reached this machine's own world load and could not finish it. Every exit from the
// boot below either has the host's world or has come through here: the reason is stashed for the
// modal, the cover comes down, the session stops so the pump is not left running the full gameplay
// tick at the menu, and the browser reopens. The shape is the abort branch's, deliberately -- the
// difference between a player cancelling and a world refusing to load is what the dialog says, not
// what the harness does.
void FailJoinNoWorld_(coop::net::EndReason code, const char* detail) {
    coop::join_progress::Fail(code, detail);  // stash the notice and win the abort
    coop::join_progress::Reset();             // cover down; this drains the abort flag too
    if (S().running() && S().role() == coop::net::Role::Client) S().Stop();
    ui::server_browser_surface::Open();
    ue_wrap::log::Flush();
}

// Where a failed host puts the player, decided in one place. The session-settings window and
// the hosting window each poll HostStatus() and paint the reason, and one of them is still on
// screen under the cover just dropped; opening the browser over it hid the explanation and left
// the window a corpse (its tick reconciled itself closed while its widgets stayed, and the
// browser's Back returned dead pixels). So the browser is the destination only when no window of
// ours owns the retry. Decided on the game thread: IsOpen() is game-thread only and frozen once
// the menu is gone; the residual is bounded by the browser intent's 20 s TTL.
void ReturnToMenuAfterFailedHost() {
    if (coop::shutdown::IsShuttingDown()) return;
    GT::Post([] {
        if (coop::shutdown::IsShuttingDown()) return;
        if (ui::host_session_settings::IsOpen() || ui::host_window_native::IsOpen())
            return;   // a window of ours already owns the retry and is showing the reason
        ui::server_browser_surface::Open();
    });
}

// A host the player's ICE policy refuses before its world loads leaves through the steps of a save
// that would not load: the reason, with the refusal's code for a report to quote, where the hosting
// window or the browser paints it; the lobby it announced withdrawn while the cover still stands,
// so no second host can start under a withdrawal in flight; the cover dropped. Unlike those exits
// the status goes first, so the native hosting window paints it during the withdrawal's wait.
void FailHostBeforeLoad_(const coop::net::Refusal& r) {
    const auto& info = coop::net::Describe(r.code);
    UE_LOGW("harness: host-with-save refused before the world load -- [%s] %s", info.id,
            r.detail.c_str());
    std::string status = std::string("Host failed [") + info.id + "]: " + info.text;
    if (!r.detail.empty()) status += " (" + r.detail + ")";
    coop::session_manager::SetHostStatus(status);
    if (!coop::shutdown::IsShuttingDown()) coop::session_manager::EndHostedLobby();
    coop::join_progress::Reset();
    ReturnToMenuAfterFailedHost();
}

}  // namespace

bool BootStorySaveBlocking(bool forceFresh, const wchar_t* slotOverride, int forceGameMode) {
    // ONE world load at a time, whoever asks. Two threads can reach this now -- the TimelineThread
    // driving a join, and the menu-autoload worker -- and two concurrent loads do not corrupt each
    // other's state (nothing here is static) but they do race the engine's `open` and the host-slot
    // string behind it. A second entrant is refused by name rather than left to an undefined
    // window, which is the answer this owes under the mid-activity-join rule.
    static std::atomic<bool> s_loading{false};
    bool idle = false;
    if (!s_loading.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) {
        UE_LOGW("harness: a world load is already running -- refusing a second one ('%ls')",
                slotOverride ? slotOverride : L"<configured slot>");
        return false;
    }
    struct Release { ~Release() { s_loading.store(false, std::memory_order_release); } } _r;

    // A blank New Game is the deterministic baseline the host's snapshot mirrors onto; chosen by
    // `forceFresh`, by VOTVCOOP_FRESH=1 (the test launcher sets it for the client) or by the
    // fresh_boot ini row.
    const bool freshBoot =
        !slotOverride && (forceFresh || cfg::ResolveFlag(coop::config_registry::rows::fresh_boot));
    // A blank New Game has no slot file, so nothing names its mode but the caller. Un-named, it is
    // story: the campaign the game itself opens on. A slot boot derives its own and wants -1.
    const int freshMode =
        ue_wrap::game_mode::IsValid(forceGameMode) ? forceGameMode : ue_wrap::game_mode::kStory;
    // The save slot: an explicit override, else the VOTVCOOP_SAVE / ini `save` row (the test
    // launcher pins the host's per run), else the default.
    std::wstring slot;
    if (slotOverride) {
        slot = slotOverride;
    } else {
        // VOTVCOOP_SAVE rides the row inside ResolveString.
        std::string slotA = cfg::ResolveString(coop::config_registry::rows::save);
        slot.assign(slotA.begin(), slotA.end());  // ASCII slot name
    }
    if (freshBoot)
        UE_LOGI("harness: target FRESH New Game (blank save) in mode %d (%s)", freshMode,
                ue_wrap::game_mode::NameOrOrdinal(freshMode).c_str());
    else
        UE_LOGI("harness: target SAVE slot '%ls' (its name carries its mode)", slot.c_str());
    for (int i = 0; i < 80; ++i) {  // ~120 s cap (boot + omega + level load)
        if (coop::shutdown::IsShuttingDown()) {
            UE_LOGI("harness: BootStorySaveBlocking aborting -- shutdown signaled");
            return false;
        }
        auto st = std::make_shared<std::atomic<int>>(0);  // 0 pending,1 retry,2 ok
        GT::Post([slot, st, freshBoot, forceGameMode, freshMode] {
            const bool inGame =
                freshBoot ? ue_wrap::engine::StartFreshGame(freshMode)
                          : ue_wrap::engine::LoadStorySave(slot.c_str(), forceGameMode);
            st->store(inGame ? 2 : 1);
        });
        // Shutdown-aware, like the host-boot twin in DriveHostBootIfPending: a posted task that
        // the game thread never drains -- which is every moment of a blocking level load -- held
        // this thread for as long as it lasted, teardown included.
        while (st->load() == 0 && !coop::shutdown::IsShuttingDown()) ::Sleep(5);
        if (st->load() == 2) {
            // The world this process would serve if it hosts: the slot it loaded, or no slot for
            // a New Game, which has no file yet -- named all the same, because what is kept per
            // loaded world has to learn that another one took its place. The picker sets its own,
            // and the coop override is a client load that never serves.
            if (!slotOverride) coop::save_transfer::SetHostSlot(freshBoot ? std::wstring() : slot);
            return true;
        }
        ::Sleep(1500);
    }
    UE_LOGW("harness: did not reach gameplay in time (fresh_boot=%d, '%ls')", freshBoot ? 1 : 0, slot.c_str());
    return false;
}

void DriveMenuModeJoinWorldBoot() {
    namespace ST = coop::save_transfer;
    // NO CLOCK ON THIS LOOP. It ends when the transfer reaches a state, when the player cancels,
    // when the link dies -- or when join_progress's phase watchdog fails the join by name and sets
    // the abort this loop drains. The 120 s cap that used to sit here answered a transfer that was
    // merely SLOW by booting a fresh world and finishing the join in it, telling the player
    // nothing: it declared the host's save "unavailable/failed" while the host was still streaming
    // it, and the joiner ended up standing in a world that was not the host's.
    bool aborted = false;
    for (;;) {
        const ST::ClientState st = ST::GetClientState();
        if (st == ST::ClientState::ReadySlotWritten ||
            st == ST::ClientState::NoSaveAvailable ||
            st == ST::ClientState::Failed) break;
        if (coop::shutdown::IsShuttingDown()) return;
        if (coop::join_progress::TakeAbortRequest()) { aborted = true; break; }
        if (!coop::join_progress::Active()) { aborted = true; break; }  // the cover came down
        if (!S().running()) { aborted = true; break; }                  // connect died
        // Feed the loading screen the download's real progress, polled here (this loop runs at ~60
        // Hz on the timeline thread for the whole transfer) rather than pushed from the chunk sink,
        // so the net thread gains no per-chunk work. Without it the longest phase of a join, ~17 s
        // at 1 MB/s, showed a marquee.
        {
            uint32_t doneB = 0, totalB = 0;
            coop::save_transfer::GetProgress(doneB, totalB);
            coop::join_progress::NoteDownload(doneB, totalB);
        }
        // This loop blocks the thread that posts the session tick, and the transfer lives in that
        // tick (the connect edge sends the Request, event_feed delivers Begin, the host pumps
        // chunks): pump it here or the join deadlocks at "Connecting".
        harness::pump::PostMenuTick();
        harness::pump::TickWatchdogs();
        ::Sleep(16);  // ~60 Hz, same cadence as the play loop
    }
    if (aborted) {
        UE_LOGI("harness: menu-mode join aborted during the save transfer");
        // The cover first: a tick posted before this drain must not find the join active over
        // a session the stop below has already driven to Disconnected, and fail it.
        coop::join_progress::Reset();
        if (S().running() && S().role() == coop::net::Role::Client) S().Stop();
        ui::server_browser_surface::Open();
        ue_wrap::log::Flush();
        return;
    }
    // The blob is in, or there was none. Everything past here is the engine loading a world, 30-60
    // s with nothing reporting progress: say so, and drop the byte bar, which pegged at 100% reads
    // as a hang.
    coop::join_progress::BeginWorldLoad();
    // Wait, pumping, for the host's per-player inventory blob before the load, so the
    // pre-materialise hook has this client's inventory at load time. The host pushes it the moment
    // our guid arrives, so it has almost always landed during the transfer wait; this is the safety
    // barrier.
    // True to go on with the load. False means the join has ALREADY been failed and torn down.
    auto waitForApplyBlob = []() -> bool {
        namespace PIS = coop::player_inventory_sync;
        if (PIS::HasPendingApply()) return true;
        const ULONGLONG w0 = ::GetTickCount64();
        while (!PIS::HasPendingApply()) {
            if (coop::shutdown::IsShuttingDown() || !S().running()) return false;
            // The host sends this the moment the slot's guid arrives and retries every tick on a
            // refusal, and it always has something to send -- the stored profile, its file, or the
            // starter kit, never another player's. So a blob that never arrives is a host that is
            // not doing what it owes, and the bound says so by name. It used to load ANYWAY and let
            // the apply hook empty the host's items out of the save object: the player walked into
            // the world with someone else's emptied inventory and was told nothing, and this
            // session then reported no inventory back.
            if (::GetTickCount64() - w0 > 20000) {
                FailJoinNoWorld_(coop::net::EndReason::ProfileNotSent,
                                 "no inventory arrived in 20s, and loading without one would put "
                                 "you in the world with an emptied inventory");
                return false;
            }
            harness::pump::PostMenuTick();
            harness::pump::TickWatchdogs();
            ::Sleep(16);
        }
        UE_LOGI("harness: inventory apply blob ready -- proceeding to load the world");
        return true;
    };

    // Arm the world-load episode before the boot that triggers the game's loadObjects pre-delete:
    // during that load the destroy seam suppresses the keyed-prop destroys this client's world
    // rebuild churns, which would otherwise reach the host and destroy its authoritative copies by
    // key (a bare join once emptied the host from 3,345 keyed props to 1,255).
    // join_membership_sweep ends the episode at load-tail quiescence. The sole, client-only arm
    // site, causally before the burst on every path.
    coop::world_load_episode::Arm();

    if (ST::GetClientState() == ST::ClientState::ReadySlotWritten) {
        // Load the host's world from the downloaded slot; ResetCachedSave first, so a rejoin in
        // this process does not re-register a stale cached save. The slot is per pid; the mode came
        // over the wire.
        const std::wstring slot = ST::CoopSlotName();
        const int mode = static_cast<int>(ST::ReceivedGameMode());
        UE_LOGI("harness: save received -- loading coop slot '%ls' (mode=%d)", slot.c_str(), mode);
        auto rst = std::make_shared<std::atomic<int>>(0);
        GT::Post([rst] { ue_wrap::engine::ResetCachedSave(); rst->store(1); });
        while (rst->load() == 0 && !coop::shutdown::IsShuttingDown()) ::Sleep(5);
        if (!waitForApplyBlob()) return;
        coop::player_inventory_sync::BeginJoinApply();
        if (!BootStorySaveBlocking(/*forceFresh=*/false, slot.c_str(), mode)) {
            // NOT a fresh boot. The world we were given is the world this join is about, and an
            // engine that will not load it has ended the join, not changed its subject.
            FailJoinNoWorld_(coop::net::EndReason::WorldWouldNotLoad,
                             "the engine did not reach gameplay with the world the host sent");
        }
        return;
    }
    if (ST::GetClientState() == ST::ClientState::NoSaveAvailable) {
        // The one legitimate fresh boot: the host HAS no save, said so, and a blank world is the
        // correct answer rather than a substitute for one. (ArmBeginNoSave_ sends this on purpose.)
        const int freshMode = static_cast<int>(ST::ReceivedGameMode());
        UE_LOGI("harness: the host has no save -- fresh-booting the ephemeral baseline in the "
                "host's mode %d, as asked", freshMode);
        if (!waitForApplyBlob()) return;
        coop::player_inventory_sync::BeginJoinApply();
        if (!BootStorySaveBlocking(/*forceFresh=*/true, /*slotOverride=*/nullptr, freshMode))
            FailJoinNoWorld_(coop::net::EndReason::WorldWouldNotLoad,
                             "the engine did not reach gameplay with a fresh world");
        return;
    }
    // ClientState::Failed: the blob arrived damaged (a CRC mismatch or a slot write that failed).
    // This used to fresh-boot too, under a log line that blamed the host.
    FailJoinNoWorld_(coop::net::EndReason::WorldUnusable,
                     "the world arrived damaged: the CRC or the slot write failed");
}

void DriveHostBootIfPending() {
    // The per-boot state lives in a shared_ptr captured by value into the posted task, so a task
    // queued when a shutdown breaks the wait keeps its storage alive. `slot` and `created` carry
    // across the retry loop: the create runs once, then the load is polled.
    struct Boot {
        coop::session_manager::PendingHost ph;
        std::wstring slot;
        bool created = false;
        std::atomic<int> st{0};  // 0 pending,1 retry,2 in-gameplay,3 fail
    };
    // The queue is drained FIRST: this runs every idle tick of the play loop, and allocating the
    // per-boot state before asking whether there is a boot cost one control block and one string
    // per tick, forever, for nothing.
    coop::session_manager::PendingHost pending;
    if (!coop::session_manager::TakePendingHostWithSave(pending)) return;
    // What Session::Start would refuse after the load is refused before it: the same verdict
    // (coop/net/ice_policy.h) on the Config the start will take.
    if (const coop::net::Refusal r =
            coop::net::IcePolicyRefusal(coop::net::ResolveIcePolicy(), pending.cfg);
        r.code != coop::net::EndReason::None) {
        FailHostBeforeLoad_(r);
        return;
    }
    auto b = std::make_shared<Boot>();
    b->ph = std::move(pending);
    if (!b->ph.save.newGame) b->slot.assign(b->ph.save.slot.begin(), b->ph.save.slot.end());  // ASCII
    b->created = !b->ph.save.newGame;  // existing save: nothing to create

    const std::string newGameWhat =
        "NEW " + ue_wrap::game_mode::NameOrOrdinal(b->ph.save.mode) + " game";
    UE_LOGI("harness: host-with-save -- %s '%s' -> load then host",
            b->ph.save.newGame ? newGameWhat.c_str() : "load",
            b->ph.save.newGame ? b->ph.save.newName.c_str() : b->ph.save.slot.c_str());

    for (int i = 0; i < 80 && !coop::shutdown::IsShuttingDown(); ++i) {  // ~120 s cap
        b->st.store(0);
        GT::Post([b] {
            if (b->ph.save.newGame && !b->created) {
                std::wstring wname(b->ph.save.newName.begin(), b->ph.save.newName.end());
                std::wstring outSlot;
                // The choice decides: a name the player typed keeps the exact primitive and its
                // refusal; a derived name is disambiguated, since no human is there to be told it
                // was taken.
                const bool created =
                    b->ph.save.nameIsDerived
                        ? ue_wrap::save_browser::CreateNamedSaveUnique(wname, b->ph.save.mode, outSlot)
                        : ue_wrap::save_browser::CreateNamedSave(wname, b->ph.save.mode, outSlot);
                if (!created) { b->st.store(3); return; }
                b->slot = outSlot;
                b->created = true;
                UE_LOGI("harness: host-with-save created + persisted new save '%ls'", b->slot.c_str());
                // The name was free on disk, which a deleted save's name is too: what its players
                // left beside it belongs to no world, least of all to this new one.
                coop::player_profile_store::ForgetSlot(b->slot);
            }
            if (b->slot.empty()) { b->st.store(3); return; }
            const bool inGame = ue_wrap::engine::LoadStorySave(b->slot.c_str());
            // The picked slot is what this host serves to joiners.
            if (inGame) coop::save_transfer::SetHostSlot(b->slot);
            b->st.store(inGame ? 2 : 1);
        });
        while (b->st.load() == 0 && !coop::shutdown::IsShuttingDown()) ::Sleep(5);
        const int s = b->st.load();
        if (s == 2) {
            UE_LOGI("harness: host-with-save world loaded ('%ls') -- starting host session", b->slot.c_str());
            harness::session_runtime::StartCoopSession(b->ph.cfg);
            coop::join_progress::Reset();  // world up + session started -> drop the host cover -> gameplay
            return;
        }
        if (s == 3) {
            UE_LOGW("harness: host-with-save create/load FAILED -- aborting host");
            // The lobby was announced before the load; cancel it, or a phantom lobby lingers on the
            // master. No HTTP during teardown.
            if (!coop::shutdown::IsShuttingDown()) coop::session_manager::EndHostedLobby();
            // Do not strand the player on a blank menu: drop the cover, surface the failure, return
            // to the menu.
            coop::join_progress::Reset();
            coop::session_manager::SetHostStatus(
                b->ph.save.newGame ? "Host failed: could not create the new save"
                                   : "Host failed: could not load that save");
            ReturnToMenuAfterFailedHost();
            return;
        }
        ::Sleep(1500);  // throttle LoadStorySave's `open` re-issue (matches BootStorySaveBlocking)
    }
    if (!coop::shutdown::IsShuttingDown()) {
        UE_LOGW("harness: host-with-save did not reach gameplay in time -- aborting host");
        coop::session_manager::EndHostedLobby();  // no phantom lobby on a timeout
        coop::join_progress::Reset();        // drop the host cover
        coop::session_manager::SetHostStatus("Host failed: the world did not load in time");
        ReturnToMenuAfterFailedHost();
    }
}

}  // namespace harness::world_boot
