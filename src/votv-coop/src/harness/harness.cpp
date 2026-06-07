#include "harness/harness.h"

#include "harness/autotest.h"
#include "harness/autotest_dispatch.h"
#include "harness/config.h"
#include "harness/harness_diag.h"
#include "harness/screenshot.h"
#include "harness/sdk_check.h"
#include "coop/dev/force_weather.h"
#include "coop/dev/freecam.h"
#include "coop/dev/restore_vitals.h"
#include "coop/dev/spawn_npc.h"
#include "coop/dev/teleport_client.h"
#include "coop/event_feed.h"
#include "coop/garbage_sync.h"
#include "coop/grab_observer.h"
#include "coop/item_activate.h"
#include "coop/players_registry.h"
#include "coop/ban_list.h"
#include "coop/moderation.h"
#include "coop/ini_config.h"
#include "coop/session_manager.h"
#include "coop/nameplate.h"
#include "coop/roster.h"
#include "coop/net/session.h"
#include "coop/net_pump.h"
#include "coop/npc_sync.h"
#include "coop/prop_lifecycle.h"
#include "coop/prop_snapshot.h"
#include "coop/remote_player.h"
#include "coop/remote_prop.h"
#include "coop/save_guard.h"
#include "coop/shutdown.h"
#include "coop/weather_sync.h"
#include "ui/dev_menu.h"
#include "ui/imgui_overlay.h"
#include "ui/console.h"
#include "ui/server_browser.h"
#include "coop/join_progress.h"
#include "coop/multiplayer_menu.h"
#include "coop/dev/menu_proceed.h"
#include "coop/dev/save_probe.h"
#include "ue_wrap/call.h"
#include "ue_wrap/hud_feed.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/save_browser.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/reflected_offset.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace harness {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

namespace cfg = harness::config;

// Diagnostic dumps (Report / DumpComponents / DumpLiveWidgets / DumpParams) live in
// harness/harness_diag.cpp; bring them into scope so the scenario call sites stay unqualified.
using namespace harness::diag;

// The single coop networking session (Phase 3). Host binds the LAN port;
// client targets the host. Drives the remote puppet from received pose
// snapshots and sends the local player's pose. Off unless a scenario
// starts it.
coop::net::Session g_session;

// Phase 2 host moderation: the accept predicate wired into the host Session
// (Session::SetAcceptFilter). Returns true to allow an incoming IP, false to
// reject a banned one. A plain free function so it converts to the
// Session::AcceptFilterFn function pointer. Read on the net thread; reads only
// coop::ban_list's own mutexed state.
bool BanAcceptFilter(const char* remoteIp) {
    return !coop::ban_list::IsBanned(remoteIp);
}

// ReadLocalPose extracted to coop/net_pump.cpp (PR-4.13).

// Diagnostic dumps (DumpParams / Report / DumpComponents / DumpLiveWidgets, + their
// ContainsCI / DumpClassFunctions helpers) extracted to harness/harness_diag.cpp
// (2026-06-06 modular file-size audit). They are reached unqualified here via
// `using namespace harness::diag` below.

// Puppet array + per-slot edge state + held-prop edge detector + the local-
// pose read + the per-tick observer orchestrator + the main NetPumpTick body
// extracted to coop/net_pump.cpp (PR-4.13). Harness reaches the puppets via
// coop::net_pump::Puppet(slot) and calls coop::net_pump::Tick(g_session, ...)
// from the timeline tick lambdas; coop::net_pump::OnSessionStart() resets
// edge-detector state on each session.Start.

// Standalone shutdown hooks for the timeline tick. NOT gated on the local
// player being live -- runs regardless of possession state. Idempotent.
// Kept in harness because the HWND subclass + window title must work
// BEFORE the local player has been possessed (e.g. on OMEGA splash where
// the user might X-close before gameplay).
void TickShutdownHooks() {
    coop::shutdown::Install(&g_session);
    coop::shutdown::UpdateWindowTitle();
}

// Autonomous grab test moved to harness/autotest.cpp.

// NetPumpTick body extracted to coop/net_pump.cpp (PR-4.13). Harness call
// sites in the timeline tick lambdas dispatch via
// coop::net_pump::Tick(g_session, displayOffsetX) instead.

void Post(GT::Task t) { GT::Post(std::move(t)); }

// Spawn the 2nd player the INSTANT the local mainPlayer_C exists -- no fixed
// timer. Polls on the game thread (engine state can only be read there) every
// ~100 ms; the moment the local player is present, spawns and returns. The shared
// flag is a shared_ptr so it outlives the worker loop even if a posted check is
// still queued (no use-after-free).
void SpawnSecondPlayerWhenReady() {
    UE_LOGI("play: waiting for STORY gameplay, spawn 2nd player the instant it's ready");
    for (int i = 0; i < 1200; ++i) {  // ~120 s safety cap
        if (coop::shutdown::IsShuttingDown()) {
            UE_LOGI("play: SpawnSecondPlayerWhenReady aborting -- shutdown signaled");
            return;
        }
        auto state = std::make_shared<std::atomic<int>>(0);  // 0 pending,1 not-ready,2 ok,3 failed
        Post([state, i] {
            if (coop::net_pump::Puppet(1).valid()) { state->store(2); return; }
            void* local = coop::players::Registry::Get().Local();
            const bool diag = (i % 20 == 0);  // ~every 2 s
            if (!local) {
                if (diag) {
                    void* w = R::FindObjectByClass(P::name::WorldClass);
                    UE_LOGI("play[wait %d]: no mainPlayer_C; world=%ls objs=%d", i,
                            w ? R::ToString(R::NameOf(w)).c_str() : L"(none)", R::NumObjects());
                }
                state->store(1); return;
            }
            // A mainPlayer_C ALSO exists at the menu (the 'preLoad' world) sitting
            // at the ORIGIN. Spawning against it puts the puppet in the menu world,
            // which the level load then destroys -> "no one spawns". Gate on the
            // player being placed in the real level: a non-origin location.
            const ue_wrap::FVector p = ue_wrap::engine::GetActorLocation(local);
            if (std::abs(p.X) + std::abs(p.Y) + std::abs(p.Z) < 100.f) {
                if (diag) UE_LOGI("play[wait %d]: mainPlayer_C @ORIGIN (%.0f,%.0f,%.0f) -- waiting for real gameplay",
                                  i, p.X, p.Y, p.Z);
                state->store(1);  // still the origin menu player; wait for gameplay
                return;
            }
            UE_LOGI("play: mainPlayer_C ready @ (%.0f,%.0f,%.0f) -- spawning puppet", p.X, p.Y, p.Z);
            state->store(coop::net_pump::Puppet(1).Spawn() ? 2 : 3);
        });
        while (state->load() == 0) ::Sleep(5);  // let the posted check run (~1 frame)
        const int s = state->load();
        if (s == 2) {
            UE_LOGI("play: 2nd player spawned the moment the local player was ready");
            return;
        }
        if (s == 3) UE_LOGW("play: spawn attempt failed; retrying");
        // No sandbox `open` fallback: coop targets STORY mode, loaded via the save
        // system (LoadStorySave), never the sandbox map. We just wait for gameplay.
        ::Sleep(100);  // local player not in world yet -> poll again
    }
    UE_LOGW("play: gave up waiting for local mainPlayer_C");
}

// Boot STORY gameplay (the coop target). LoadStorySave (re)issues `open untitled_1`
// each tick while at preLoad/OMEGA/menu (a single early open during preLoad is
// dropped -> must retry) and returns true once gameplay is reached; ~1.5 s/tick
// throttles the opens. Blocks (worker thread) until loaded or the ~120 s cap.
// `forceFresh` forces the BLANK New Game path regardless of the ini -- the menu-mode
// browser CLIENT join uses it to fresh-boot the ephemeral-client baseline before connecting.
bool BootStorySaveBlocking(bool forceFresh = false) {
    // FRESH-BOOT (2026-06-04, project-ephemeral-client-host-authoritative-world): a BLANK New
    // Game (StartFreshGame) instead of loading a save slot is the deterministic baseline the
    // host's connect-snapshot mirrors onto. Driven by `forceFresh` (the menu-mode client join,
    // 2026-06-06) OR the `fresh_boot=1` ini test gate.
    const bool freshBoot = forceFresh || (cfg::ReadIniValue("fresh_boot", "0") == "1");
    // STORY save slot, from votv-coop.ini "save=<slot>" (defaults s_may2026). Coop
    // targets story mode, so we never boot the sandbox map fresh.
    const std::string slotA = cfg::ReadIniValue("save", "s_may2026");
    const std::wstring slot(slotA.begin(), slotA.end());  // ASCII slot name
    UE_LOGI("harness: target %s '%ls'", freshBoot ? "FRESH New Game (blank save)" : "STORY save", slot.c_str());
    for (int i = 0; i < 80; ++i) {  // ~120 s cap (boot + omega + level load)
        if (coop::shutdown::IsShuttingDown()) {
            UE_LOGI("harness: BootStorySaveBlocking aborting -- shutdown signaled");
            return false;
        }
        auto st = std::make_shared<std::atomic<int>>(0);  // 0 pending,1 retry,2 ok
        Post([slot, st, freshBoot] {
            const bool inGame = freshBoot ? ue_wrap::engine::StartFreshGame(/*storyMode=*/true)
                                          : ue_wrap::engine::LoadStorySave(slot.c_str());
            st->store(inGame ? 2 : 1);
        });
        while (st->load() == 0) ::Sleep(5);
        if (st->load() == 2) return true;
        ::Sleep(1500);
    }
    UE_LOGW("harness: did not reach gameplay in time (fresh_boot=%d, '%ls')", freshBoot ? 1 : 0, slot.c_str());
    return false;
}

// Bring up a coop session on g_session: reset per-session edge state, wire every sync
// subsystem, (host) back up the save + install the LanDirect ban filter, then Start.
// ONE code path for "start a coop session" (RULE 2) -- called by BOTH the env-configured
// boot (ReadNetConfig) AND a browser action consumed via session_manager::
// TakePendingStart. Runs on the TimelineThread: g_session.Start spawns the net thread,
// and the host save-backup is a blocking file copy -- neither belongs on the game thread.
// Returns Start()'s success: the browser-join path Fails the join (drops the loading
// screen + reopens the browser) when a synchronous Start failure means no async connect
// edge will ever arrive. Other callers (env play / host-with-save / netloopback) ignore it.
bool StartCoopSession(const coop::net::Config& netCfg) {
    // Never start once teardown has begun: g_session.Start would spawn a net thread
    // AFTER DoShutdown's single Stop(), to be joined at static destruction -- the
    // loader-lock join-from-teardown hazard the project forbids (audit P2).
    if (coop::shutdown::IsShuttingDown()) {
        UE_LOGW("harness: StartCoopSession ignored -- shutdown in progress");
        return false;
    }
    coop::event_feed::SetLocalNickname(cfg::ReadNickname());
    coop::event_feed::OnSessionStart();
    // Reset net_pump edge-detector state so a Stop()/Start() cycle on the same process
    // doesn't carry stale "was connected" / "was holding prop" entries into the new
    // session (phantom disconnect edge / suppressed connect replay / stale prop key).
    coop::net_pump::OnSessionStart();
    coop::prop_lifecycle::SetSession(&g_session);
    coop::npc_sync::SetSession(&g_session);
    coop::prop_snapshot::SetSession(&g_session);
    coop::dev::restore_vitals::SetSession(&g_session);
    coop::dev::teleport_client::SetSession(&g_session);
    coop::dev::force_weather::SetSession(&g_session);
    coop::moderation::SetSession(&g_session);
    if (netCfg.role == coop::net::Role::Host) {
        // Snapshot the canonical save BEFORE coop injects state (host-only; clients are
        // save-blocked). Synchronous on this bringup thread -> completes before Start.
        coop::save_guard::BackupSaveOnSessionStart();
        // LanDirect ONLY: the IP-keyed ban filter FAIL-CLOSES on P2P (empty remote addr
        // at the Connecting edge; a peer's public IP is the wrong key anyway). P2P bans
        // are identity-based (Stage 6).
        if (netCfg.topology == coop::net::Topology::LanDirect) {
            coop::ban_list::Load();
            g_session.SetAcceptFilter(&BanAcceptFilter);
        }
    }
    // NOTE: the CLIENT connecting/loading state (join_progress::BeginConnect) is NOT raised
    // here. It is raised by the BROWSER connect actions only (session_manager::JoinLobby /
    // ConnectDirect), so the loading screen is browser-join-only -- the env/.bat/autotest
    // client boot reaches this function directly and must show nothing (regression A,
    // 2026-06-06). multiplayer_menu hides the menu widgets while a join is Active and the
    // console auto-shows the connect log; both key off join_progress, raised at the click.
    const bool ok = g_session.Start(netCfg);
    UE_LOGI("harness: ==== COOP SESSION START (%s / %s)%s ====",
            netCfg.role == coop::net::Role::Host ? "host" : "client",
            netCfg.topology == coop::net::Topology::P2P ? "p2p" : "lan-direct",
            ok ? "" : " -- START FAILED");
    return ok;
}

// Host-Game save-picker orchestration. If a host-with-save was queued
// (session_manager::HostWithSave, from the picker's "Host selected save" / "New Game &
// Host"), LOAD the chosen world (engine::LoadStorySave) or CREATE the new save
// (save_browser::CreateNamedSave) FIRST -- polling like BootStorySaveBlocking (the
// `open` re-issue is throttled to ~1.5 s, NOT per-50ms-tick) -- then StartCoopSession.
// No-op (returns immediately) when nothing is queued. Runs on the TimelineThread (where
// the blocking load + StartCoopSession belong). The picker fires at the MENU, so the
// chosen world loads from the menu here, then hosting begins. ONE place owns load->host.
void DriveHostBootIfPending() {
    // ALL the per-boot state lives in a shared_ptr-held struct captured BY VALUE into the
    // posted game-thread task -- so the task can NEVER outlive its backing storage. If a
    // shutdown breaks the wait below before a queued task has run, this function returns
    // but the task keeps `b` alive via its captured shared_ptr (no use-after-free of stack
    // locals; audit CRITICAL-1). `slot`/`created` carry across the retry loop (the new-save
    // create runs once, then we poll the load).
    struct Boot {
        coop::session_manager::PendingHost ph;
        std::wstring slot;
        bool created = false;
        std::atomic<int> st{0};  // 0 pending,1 retry,2 in-gameplay,3 fail
    };
    auto b = std::make_shared<Boot>();
    if (!coop::session_manager::TakePendingHostWithSave(b->ph)) return;
    if (!b->ph.save.newGame) b->slot.assign(b->ph.save.slot.begin(), b->ph.save.slot.end());  // ASCII
    b->created = !b->ph.save.newGame;  // existing save: nothing to create

    UE_LOGI("harness: host-with-save -- %s '%s' -> load then host",
            b->ph.save.newGame ? "NEW story game" : "load",
            b->ph.save.newGame ? b->ph.save.newName.c_str() : b->ph.save.slot.c_str());

    for (int i = 0; i < 80 && !coop::shutdown::IsShuttingDown(); ++i) {  // ~120 s cap
        b->st.store(0);
        Post([b] {
            if (b->ph.save.newGame && !b->created) {
                std::wstring wname(b->ph.save.newName.begin(), b->ph.save.newName.end());
                std::wstring outSlot;
                if (!ue_wrap::save_browser::CreateNamedSave(wname, b->ph.save.mode, outSlot)) {
                    b->st.store(3); return;  // create failed (name taken / save system unresolved)
                }
                b->slot = outSlot;
                b->created = true;
                UE_LOGI("harness: host-with-save created + persisted new save '%ls'", b->slot.c_str());
            }
            if (b->slot.empty()) { b->st.store(3); return; }
            b->st.store(ue_wrap::engine::LoadStorySave(b->slot.c_str()) ? 2 : 1);
        });
        while (b->st.load() == 0 && !coop::shutdown::IsShuttingDown()) ::Sleep(5);
        const int s = b->st.load();
        if (s == 2) {
            UE_LOGI("harness: host-with-save world loaded ('%ls') -- starting host session", b->slot.c_str());
            StartCoopSession(b->ph.cfg);
            return;
        }
        if (s == 3) {
            UE_LOGW("harness: host-with-save create/load FAILED -- aborting host");
            // The lobby was already announced (HostWithSave announces before the load) --
            // cancel it so no phantom lobby lingers on the master (audit HIGH-1). Skip the
            // /leave HTTP during teardown.
            if (!coop::shutdown::IsShuttingDown()) coop::session_manager::AbortHost();
            return;
        }
        ::Sleep(1500);  // throttle LoadStorySave's `open` re-issue (matches BootStorySaveBlocking)
    }
    if (!coop::shutdown::IsShuttingDown()) {
        UE_LOGW("harness: host-with-save did not reach gameplay in time -- aborting host");
        coop::session_manager::AbortHost();  // HIGH-1: don't leave a phantom lobby on timeout
    }
}

// The main loop (TimelineThread). Each tick: (1) if no session is running, poll
// session_manager for a browser-initiated start (Host/Join/Direct) and boot it HERE
// (Start + host save-backup must not run on the game thread); (2) post the per-tick
// pump -- net_pump::Tick while a session is running (env- OR browser-booted); (3)
// ~2 s LAN-test stats while running. ONE loop for the env-configured "play" path AND
// the native "menu" path (RULE 2).
//
// `idleInGameplay` distinguishes the two idle states (when no session is running):
//   true  ("play" scenario): we booted STRAIGHT INTO gameplay -> the idle state is
//          solo gameplay, so keep the local observers + nameplate/roster live.
//   false ("menu" scenario, native launch): the idle state is VOTV's MAIN MENU --
//          the gameplay BP classes aren't loaded, so installing the gameplay
//          observers every tick is premature churn. Skip them at the menu; they
//          install when a session actually starts (net_pump::Tick installs them).
//          We deliberately do NOT add a per-tick FindObjectByClass(World) "are we
//          in gameplay" probe here -- a per-frame GUObjectArray scan is the exact
//          FPS anti-pattern the perf rule forbids; the scenario flag is free.
void RunPlayLoop(bool idleInGameplay) {
    int tick = 0;
    while (!coop::shutdown::IsShuttingDown()) {
        // Client join ABORT (Cancel button OR a connect failure). Stop the session here
        // (the timeline thread -- where Stop, which joins the net thread, belongs), drop the
        // loading state, and reopen the browser so the user can pick again. multiplayer_menu
        // restores the hidden menu once join_progress goes inactive. ONE drain for both the
        // user-cancel and the auto-fail paths (regression C, 2026-06-06).
        if (coop::join_progress::TakeAbortRequest()) {
            UE_LOGI("harness: join aborted (cancelled or failed) -- stopping session + reopening the browser");
            if (g_session.running()) g_session.Stop();
            coop::join_progress::Reset();
            ui::server_browser::Open();
            // A failed/cancelled join is a milestone a real user's bug report must capture --
            // land the whole abort sequence (Fail/aborted/Stop/Reset) on disk now rather than
            // leaving it in the buffered INFO stream (which a force-kill or a quiet idle menu
            // would lose). Mirrors the boot-ready Flush. (2026-06-06.)
            ue_wrap::log::Flush();
        }
        if (!g_session.running() && !coop::shutdown::IsShuttingDown()) {
            // Host-Game save picker: load the chosen world (or create the new save) THEN
            // host. Blocks here until done; no-op if nothing is queued.
            DriveHostBootIfPending();
            // Browser Join / Direct connect: start immediately on the current world.
            if (!g_session.running()) {
                coop::net::Config pending;
                if (coop::session_manager::TakePendingStart(pending)) {
                    // Stale-start guard (audit Issue 6, 2026-06-06): a browser CLIENT join
                    // raises join_progress at the click; the async master round-trip can
                    // finish + QueueStart AFTER the user already Cancelled (or it Failed). If
                    // the join is no longer Active by the time we consume the queued start,
                    // DISCARD it instead of ghost-starting a session the user backed out of.
                    // Host starts carry no join_progress (Active stays false) -> always proceed.
                    if (pending.role == coop::net::Role::Client && !coop::join_progress::Active()) {
                        UE_LOGI("harness: discarding stale browser client start -- join no longer active (cancelled/failed)");
                    } else {
                        UE_LOGI("harness: browser-initiated coop session");
                        // Menu-mode CLIENT join (2026-06-06 client world-entry): a client that
                        // clicked Connect from the MAIN MENU has no gameplay world yet, so the
                        // host's connect-snapshot would have nowhere to land. Fresh-boot a New
                        // Game -- the validated ephemeral-client baseline (project-ephemeral-
                        // client-host-authoritative-world) -- INTO gameplay FIRST, THEN connect;
                        // the host then streams its whole world onto the fresh client. Blocks
                        // here (~2 s travel) on the TimelineThread, like DriveHostBootIfPending.
                        // An already-in-gameplay browser join (idleInGameplay: the env/autotest
                        // play path, or a future in-game join) skips this -- it has a world.
                        if (pending.role == coop::net::Role::Client && !idleInGameplay) {
                            UE_LOGI("harness: menu-mode client join -- fresh-booting into gameplay before connect");
                            BootStorySaveBlocking(/*forceFresh=*/true);
                        }
                        // A synchronous Start failure (bad address / GNS init) means no async
                        // connect edge will ever arrive to clear the loading screen the browser
                        // raised -- so Fail the join here (drops the cover + reopens the browser).
                        // No-op on the host (join_progress not Active). (regression C, 2026-06-06.)
                        if (!StartCoopSession(pending))
                            coop::join_progress::Fail("could not start the connection");
                    }
                }
            }
        }
        const bool running = g_session.running();
        Post([running, idleInGameplay] {
            if (running) {
                coop::net_pump::Tick(g_session, 0.f);
            } else if (idleInGameplay) {
                // Solo gameplay, no session yet: keep the local observers live.
                coop::net_pump::InstallObservers(g_session);
            }
            // nameplate/roster need a live world+player; skip them at the menu.
            if (running || idleInGameplay) {
                coop::nameplate::Update();
                coop::roster::Refresh();
            }
            // ALWAYS: the HWND close subclass + window title must work at the menu
            // too (the user may X-close before ever hosting -- the teardown path).
            TickShutdownHooks();
        });
        if (running && ++tick % 120 == 0) {  // ~every 2 s at 60 Hz: stats for the LAN tests
            Post([] {
                UE_LOGI("net stats: state=%d sent=%llu recv=%llu puppet=%d",
                        static_cast<int>(g_session.state()),
                        static_cast<unsigned long long>(g_session.packetsSent()),
                        static_cast<unsigned long long>(g_session.packetsRecv()),
                        coop::net_pump::Puppet(1).valid() ? 1 : 0);
                if (void* lp = coop::players::Registry::Get().Local()) {
                    const auto loc = ue_wrap::engine::GetActorLocation(lp);
                    const auto rot = ue_wrap::engine::GetActorRotation(lp);
                    ue_wrap::FRotator cRot{};
                    if (void* c = ue_wrap::engine::GetController(lp)) cRot = ue_wrap::engine::GetControlRotation(c);
                    UE_LOGI("pos diag: local actor=(%.0f,%.0f,%.0f) actorYaw=%.1f ctrl(P=%.1f Y=%.1f)",
                            loc.X, loc.Y, loc.Z, rot.Yaw, cRot.Pitch, cRot.Yaw);
                }
                if (coop::net_pump::Puppet(1).valid()) {
                    const auto p = coop::net_pump::Puppet(1).GetLocation();
                    UE_LOGI("pos diag: puppet world=(%.0f,%.0f,%.0f)", p.X, p.Y, p.Z);
                }
            });
        }
        ::Sleep(running ? 16 : 50);  // 60 Hz pump when active; 20 Hz idle poll otherwise
    }
}

// Background timeline. Sleeps for pacing; every engine touch is posted to the
// game thread. Mirrors the Lua harness's newgame timeline.
DWORD WINAPI TimelineThread(LPVOID param) {
    const std::string scenario = *static_cast<std::string*>(param);
    delete static_cast<std::string*>(param);

    UE_LOGI("harness: timeline start, scenario='%s'", scenario.c_str());

    // The OMEGA WARNING is on screen during the FIRST few seconds (the intro/menu
    // world), BEFORE we `open` gameplay. Sample widgets across that window so the
    // dump catches the omega gate (a later single dump only sees gameplay widgets,
    // since VOTV preloads its UMG and the omega widget is gone by then). Each Post
    // runs on the game thread as soon as the pump is live (which is while the omega
    // screen ticks UMG), so these land during the intro.
    const bool storyBoot = (scenario == "play" || scenario == "netloopback");
    const bool menuMode  = (scenario == "menu");
    if (storyBoot) {
        // Sample widgets across the first ~3 s -- catches the OMEGA gate before we
        // `open` gameplay. This is an AUTOTEST diagnostic: it logs ~10k widget lines
        // (walks the whole UObject array 7x) and saturates the game-thread task pump.
        // It must NOT run on the native MENU path -- log spam + a stalled pump (it
        // delayed the menu bring-up by ~20 s in the first menu-boot smoke).
        for (int k = 0; k < 7; ++k) {  // ~2.8 s of coverage
            Post([k] { UE_LOGI("widgets: == intro dump pass %d ==", k); DumpLiveWidgets(); });
            ::Sleep(400);
        }
    } else if (menuMode) {
        ::Sleep(1500);  // native menu boot: a brief settle, then to RunPlayLoop (quiet log)
    } else {
        ::Sleep(8000);  // other scenarios: let the engine fully init
    }
    Post([] { Report("menu"); });
    // Param-offset reflection validator (scenario "paramdump"): logs a UFunction's
    // FProperty layout (names/offsets/sizes/flags) to check against the known
    // signature when bringing up a new function or game build.
    if (scenario == "paramdump") {
        Post([] {
            DumpParams(L"Actor", L"K2_SetActorLocation");
            DumpParams(L"GameplayStatics", L"BeginDeferredActorSpawnFromClass");
            DumpParams(L"GameplayStatics", L"FinishSpawningActor");
        });
    }

    const bool wantGameplay = (scenario == "newgame" || scenario == "orphan" ||
                               scenario == "skin" || scenario == "show" ||
                               scenario == "play" || scenario == "netloopback");
    // Autonomous scenarios boot to the menu then `open` + wait a fixed time. The
    // story-boot scenarios (play/netloopback) load via LoadStorySave in their own
    // branch, so they skip this sandbox `open`.
    if (wantGameplay && !storyBoot) {
        ::Sleep(4000);
        Post([] {
            UE_LOGI("harness: skip-to-gameplay (open %ls)", P::name::GameplayLevel);
            std::wstring cmd = L"open ";
            cmd += P::name::GameplayLevel;
            ue_wrap::engine::ExecuteConsoleCommand(cmd.c_str());
        });
        ::Sleep(25000);  // level load + BeginPlay (mainPlayer_C spawns)
        Post([] { Report("post-load"); });
    }
    // NOTE: in-game HighResShot is BANNED -- it pops a "screenshot saved" toast
    // (bottom-right) that distracts the human tester. For agent-side visual
    // verification use the external tools/capture-window.ps1 (Windows GDI grab,
    // no in-game notification) instead.

    if (scenario == "orphan") {
        // C++ port of the Phase 2.1 orphan derisk: spawn a 2nd mainPlayer_C via
        // our own CallFunction path, confirm the count goes 1->2, pose-drive it
        // by absolute teleport (the network snapshot path), then soak.
        ::Sleep(2000);
        Post([] { Report("pre-spawn"); });
        Post([] {
            UE_LOGI("harness: === spawn coop::RemotePlayer (2nd mainPlayer_C) ===");
            coop::net_pump::Puppet(1).Spawn();
        });
        ::Sleep(2000);
        Post([] { Report("post-spawn"); });
        Post([] {
            if (coop::net_pump::Puppet(1).valid()) {
                ue_wrap::FVector p = coop::net_pump::Puppet(1).GetLocation();
                UE_LOGI("harness: orphan post-spawn pos=(%.0f,%.0f,%.0f)", p.X, p.Y, p.Z);
            }
        });

        // Pose-drive: teleport the orphan in +X steps, read back each time.
        for (int i = 1; i <= 5; ++i) {
            ::Sleep(3000);
            Post([i] {
                if (!coop::net_pump::Puppet(1).valid()) { UE_LOGW("harness: drive %d -- no orphan", i); return; }
                ue_wrap::FVector p = coop::net_pump::Puppet(1).GetLocation();
                p.X += 150.f;
                const bool ok = coop::net_pump::Puppet(1).SetLocation(p);
                ue_wrap::FVector got = coop::net_pump::Puppet(1).GetLocation();
                UE_LOGI("harness: drive step %d set X=%.0f ok=%d -> read (%.0f,%.0f,%.0f)",
                        i, p.X, ok, got.X, got.Y, got.Z);
            });
        }

        ::Sleep(5000);
        Post([] { Report("post-drive soak"); });
        UE_LOGI("harness: ==== AUTONOMOUS ORPHAN TIMELINE DONE ====");
    } else if (scenario == "play") {
        // Hands-on test. Coop targets STORY mode: auto-load the story save via
        // VOTV's own load path (LoadStorySave -> open untitled_1), which also
        // skips the omega/menu (the travel happens as soon as the GameInstance is
        // up). NOT the sandbox `open`. The puppet spawns the instant gameplay live.
        // (Intro widget dumps already ran during the first ~3 s above.)
        BootStorySaveBlocking();
        // Verify the SDK profile resolves against the running VOTV build.
        // Called AFTER BootStorySaveBlocking so VOTV BP classes (loaded on
        // first gameplay-level transition) are present. Logs a one-line
        // summary + per-failure detail; result feeds adaptation when VOTV
        // updates rename/remove content.
        Post([] { harness::sdk_check::Run(); });
        // Coop networking: if votv-coop.ini configures net.role, the puppet is
        // network-driven (auto-spawned on the first peer pose) and we send our pose;
        // otherwise the puppet is spawned locally + static (the pre-net behaviour).
        bool netEnabled = false;
        const coop::net::Config netCfg = cfg::ReadNetConfig(netEnabled);
        if (netEnabled) {
            // Two post-load teleport paths:
            //   * Autonomous-test mode (env VOTVCOOP_AUTOTEST_X/Y/Z/YAW/PITCH set):
            //     position + camera-rotate the local pawn to the role-specific
            //     verified pose so each test instance's screenshot can SEE the
            //     other's puppet (per [[project-autotest-spawn-pose]]).
            //   * Production client mode (no env set, role=Client): land the
            //     client at the КПП guard checkpoint -- user rule 2026-05-23
            //     so both peers spawn near each other in regular play.
            // Either path teleports ONCE post-load, BEFORE session.Start so the
            // very first pose packet already carries the right position.
            const std::string xs = cfg::ReadEnv("VOTVCOOP_AUTOTEST_X");
            const std::string ys = cfg::ReadEnv("VOTVCOOP_AUTOTEST_Y");
            const std::string zs = cfg::ReadEnv("VOTVCOOP_AUTOTEST_Z");
            if (!xs.empty() && !ys.empty() && !zs.empty()) {
                const float ax = static_cast<float>(std::atof(xs.c_str()));
                const float ay = static_cast<float>(std::atof(ys.c_str()));
                const float az = static_cast<float>(std::atof(zs.c_str()));
                const std::string yaws   = cfg::ReadEnv("VOTVCOOP_AUTOTEST_YAW");
                const std::string pitchs = cfg::ReadEnv("VOTVCOOP_AUTOTEST_PITCH");
                const float ayaw   = yaws.empty()   ? 0.f : static_cast<float>(std::atof(yaws.c_str()));
                const float apitch = pitchs.empty() ? 0.f : static_cast<float>(std::atof(pitchs.c_str()));
                // Audit H8 (2026-05-27): use VOTV's `teleportWObackrooms` via
                // coop::dev::teleport_client::ApplyLocally. That path bypasses the
                // CMC constraints K2_TeleportTo loses to (the same root-cause
                // fix shipped in teleport_client.cpp:60-71). The retry loop
                // still wraps it because Registry::Get().Local() can be null
                // for the first few ticks (local player isn't spawned yet);
                // once Local() exists, ApplyLocally's teleport sticks on first
                // try, so the loop exits early.
                const ue_wrap::FVector target{ax, ay, az};
                bool teleported = false;
                for (int attempt = 0; attempt < 50 && !teleported; ++attempt) {
                    auto okFlag = std::make_shared<std::atomic<int>>(0);  // 0=pending,1=ok,2=nope
                    Post([ax, ay, az, ayaw, apitch, target, okFlag] {
                        void* local = coop::players::Registry::Get().Local();
                        if (!local) { okFlag->store(2); return; }
                        coop::dev::teleport_client::ApplyLocally({ax, ay, az, apitch, ayaw, 0.f});
                        const auto cur = ue_wrap::engine::GetActorLocation(local);
                        const float dx = cur.X - target.X, dy = cur.Y - target.Y, dz = cur.Z - target.Z;
                        const bool ok = std::fabs(dx) < 200.f && std::fabs(dy) < 200.f && std::fabs(dz) < 200.f;
                        okFlag->store(ok ? 1 : 2);
                    });
                    while (okFlag->load() == 0) ::Sleep(2);
                    teleported = (okFlag->load() == 1);
                    if (!teleported) ::Sleep(100);
                }
                Post([ax, ay, az, ayaw, apitch, teleported] {
                    void* local = coop::players::Registry::Get().Local();
                    const auto cur = local ? ue_wrap::engine::GetActorLocation(local) : ue_wrap::FVector{};
                    UE_LOGI("autotest teleport: target=(%.0f,%.0f,%.0f) yaw=%.1f pitch=%.1f "
                            "-> actual=(%.0f,%.0f,%.0f) settled=%d",
                            ax, ay, az, ayaw, apitch, cur.X, cur.Y, cur.Z, teleported ? 1 : 0);
                });
                ::Sleep(100);
            }
            // NOTE: a CLIENT no longer teleports to a fixed КПП checkpoint here. A joining client
            // appears at the HOST's position (user 2026-06-04): on the connect edge the HOST sends
            // its own player pose to the joiner (net_pump -> teleport_client::TeleportSlotToHost,
            // ReliableKind::TeleportClient -- the existing teleport-to-host mechanism, reused), and
            // the client applies it on its local player. No КПП constant, no puppet-polling crutch.
            StartCoopSession(netCfg);

            // Autonomous autotest dispatch: spawn each VOTVCOOP_RUN_*_TEST worker
            // thread whose env flag is set (each self-gates on role internally).
            harness::autotest::SpawnEnvGatedTests(netCfg.role);
        } else if (coop::ini_config::IsIniKeyTrue("static_2nd_player")) {
            // Opt-in dev aid ([dev] static_2nd_player=1): a static slot-1 puppet for solo
            // visual tests. OFF by default (audit P1) -- it would collide with a browser-
            // booted HOST session's slot-1 NETWORK puppet (net_pump's auto-spawn is gated
            // on !Puppet(1).valid(), so the static one would be pose-driven but never
            // registered in the roster), and a shipping solo game shouldn't show a phantom
            // player. The real 2nd player arrives via the MULTIPLAYER browser; RunPlayLoop
            // installs the solo observers regardless.
            SpawnSecondPlayerWhenReady();
        }
        UE_LOGI("harness: ==== PLAY READY ====");
        ue_wrap::log::Flush();  // boot-ready milestone: land the boot sequence on disk now
        // Unified play loop (env- or browser-driven). Replaces the two prior per-branch
        // loops + the stale Z-trace debug block (RULE 2: one loop, one start path).
        // idleInGameplay=true: "play" booted straight into gameplay (BootStorySaveBlocking).
        RunPlayLoop(/*idleInGameplay=*/true);
    } else if (scenario == "netloopback") {
        // PR-2 (2026-05-28): the single-process loopback scenario no longer
        // applies. GNS connection topology is host listens / client dials --
        // one process can't be its own peer through one Session. Autonomous
        // LAN testing now uses the two-process mp.py smoke flow. This branch
        // remains so the existing votv-coop.ini scenario= names parse; it
        // simply starts a host session that waits for a real client.
        BootStorySaveBlocking();
        Post([] { harness::sdk_check::Run(); });
        coop::net::Config cfg;
        cfg.role = coop::net::Role::Host;
        cfg.peerIp = "127.0.0.1";
        StartCoopSession(cfg);  // host LanDirect -- same wiring as the play env path
        UE_LOGI("harness: ==== NETLOOPBACK running (self UDP on %u) ====", cfg.port);
        int tick = 0;
        while (!coop::shutdown::IsShuttingDown()) {
            Post([] { coop::net_pump::Tick(g_session, 250.f); coop::nameplate::Update(); coop::roster::Refresh(); TickShutdownHooks(); });
            if (++tick % 120 == 0) {  // ~every 2 s at 60 Hz
                Post([] {
                    UE_LOGI("netloopback: state=%d sent=%llu recv=%llu puppet=%d",
                            static_cast<int>(g_session.state()),
                            static_cast<unsigned long long>(g_session.packetsSent()),
                            static_cast<unsigned long long>(g_session.packetsRecv()),
                            coop::net_pump::Puppet(1).valid() ? 1 : 0);
                });
            }
            ::Sleep(16);  // ~60 Hz pump for smooth Tick() interp (see play-net branch)
        }
    } else if (scenario == "show") {
        // Autonomous visual confirm: spawn the puppet in front, hold idle, then
        // drive a walk (speed) for a few seconds to confirm the AnimBP animates
        // from our direct variable writes. NOTE: this scenario does NOT exercise
        // the receiver-side INTERPOLATION (each SetTargetPose here either snaps
        // -- first call -- or has zero positional delta -- subsequent walk/idle
        // at same loc). The interp linear LERP path is exercised by netloopback
        // and the LAN test.
        ::Sleep(2000);
        Post([] {
            UE_LOGI("show: === spawn skin-puppet ===");
            coop::net_pump::Puppet(1).Spawn();
        });
        ::Sleep(3000);
        Post([] {
            if (!coop::net_pump::Puppet(1).valid()) { UE_LOGW("show: no puppet"); return; }
            const ue_wrap::FVector at = coop::net_pump::Puppet(1).GetLocation();
            UE_LOGI("show: drive WALK in place (speed=200) to test AnimBP locomotion");
            // Same loc/yaw, just bump speed -- the first SetTargetPose since spawn
            // snaps (hasPose_ false), then Tick applies. AnimBP locomotion picks it up.
            coop::net::PoseSnapshot s{at.X, at.Y, at.Z, /*yaw*/0.f, /*pitch*/0.f, /*speed*/200.f};
            coop::net_pump::Puppet(1).SetTargetPose(s);
            coop::net_pump::Puppet(1).Tick();
        });
        ::Sleep(4000);
        Post([] {
            if (!coop::net_pump::Puppet(1).valid()) return;
            const ue_wrap::FVector at = coop::net_pump::Puppet(1).GetLocation();
            coop::net::PoseSnapshot s{at.X, at.Y, at.Z, /*yaw*/0.f, /*pitch*/0.f, /*speed*/0.f};
            coop::net_pump::Puppet(1).SetTargetPose(s);
            coop::net_pump::Puppet(1).Tick();
            UE_LOGI("show: back to idle (speed=0)");
        });
        UE_LOGI("harness: ==== SHOW DONE ====");
    } else if (scenario == "skin") {
        // Investigate the player's visible-body setup: enumerate components of
        // the local pawn and a spawned orphan, and confirm SuperStruct offset.
        ::Sleep(2000);
        Post([] {
            R::DebugProbeSuperStructOffset();
            void* local = coop::players::Registry::Get().Local();
            DumpComponents("local mainPlayer_C", local);
            coop::net_pump::Puppet(1).Spawn();
        });
        ::Sleep(2000);
        Post([] { DumpComponents("orphan mainPlayer_C", coop::net_pump::Puppet(1).actor()); });
        UE_LOGI("harness: ==== SKIN INSPECT DONE ====");
    } else if (scenario == "newgame") {
        ::Sleep(5000);
        Post([] { Report("post-shot"); });
        UE_LOGI("harness: ==== AUTONOMOUS NEWGAME TIMELINE DONE ====");
    } else if (scenario == "menu") {
        // NATIVE launch (no test env -> ReadScenario defaults to "menu"). Boot to
        // VOTV's OWN main menu and let the user drive coop from the MULTIPLAYER
        // button (server browser + Host-Game save picker). NO auto-load into
        // gameplay -- that is TEST-only behaviour (mp.py / play-coop.bat set
        // VOTVCOOP_SCENARIO=play). This fixes the user-reported 2026-06-06 bug:
        // a native VotV.exe launch was reading a leftover scenario.txt="play" and
        // booting straight into gameplay. RunPlayLoop(idleInGameplay=false) drains
        // browser-initiated sessions (Host/Join/Direct) + keeps the shutdown hooks
        // live while at the menu; gameplay observers install when a session starts.
        UE_LOGI("harness: ==== MENU mode (native launch) -- MULTIPLAYER button drives coop ====");
        ue_wrap::log::Flush();  // boot-ready milestone: land the boot sequence on disk now
        RunPlayLoop(/*idleInGameplay=*/false);
    } else {
        UE_LOGI("harness: scenario '%s' -- no automatic actions", scenario.c_str());
    }
    return 0;
}

}  // namespace

void Start() {
    // F12 -> screenshot (toast-free, saved to coop-screenshots/). Always on,
    // independent of the scenario, so it's available during hands-on testing.
    screenshot::StartHotkeyWatcher();

    // Dev free-flying camera. HOME toggles it (kept by user request) when
    // [dev] freecam=1; the F1 menu (Player > Movement) also toggles it under
    // [dev] devkeys. No-op at boot otherwise.
    coop::dev::freecam::Init();

    // The other dev features (snow, restore vitals, teleport clients, pos/cam
    // overlay, spawn NPC) are now driven from the F1 ImGui menu -- their hotkey
    // threads were RETIRED (RULE feedback-dev-features-in-imgui-menu). SetSession
    // for restore_vitals / teleport_client / force_weather was already called
    // above (their menu actions need the Session role).

    // Dear ImGui overlay -- the F1 menu host (dev features + future MP server
    // browser). dev_menu::Init reads the dev switch off the render thread; the
    // overlay installs the DXGI present hook (ImGui brings up on the first frame).
    // Visible to all players; dev categories gate on [dev] devkeys inside the menu.
    ui::dev_menu::Init();
    // Player-list scoreboard (a second overlay surface, shown to everyone, on tilde). The
    // roster snapshot reads this session; Refresh() runs in the game-thread ticks.
    coop::roster::SetSession(&g_session);
    if (!ui::imgui_overlay::Init()) {
        UE_LOGW("harness: imgui_overlay::Init failed -- F1 menu unavailable this run");
    }
    // In-game console: register the logger sink now so it captures the mod log from here on
    // (the connect log/errors the loading state surfaces). Auto-shows during a client join.
    ui::console::Init();

    // MULTIPLAYER entry point: inject the native button above NEW GAME in VOTV's
    // main menu; clicking it opens the ImGui server browser (a third overlay
    // surface). Resolves ui_menu_C lazily (bounded retry if the BP isn't loaded
    // yet at boot). Shipping feature -- default on; [coop] multiplayer_menu_off=1
    // disables it.
    coop::multiplayer_menu::Init();

    // TEST-ONLY (VOTVCOOP_MENU_PROCEED=1): auto-advance past the begin/OMEGA
    // content-warning screen so an autonomous run reaches the MAIN MENU for the
    // button screenshot. Never on by default (the warning is a real gate).
    coop::dev::menu_proceed::Init();

    // The VOTVCOOP_SPAWN_TRIGGER file watcher (autonomous NPC-spawn path that
    // exercises host AllocAndInstall + broadcast + client mirror Install). Hands-on
    // spawning is the F1 menu (Game > Entities). No-op unless the trigger env is set.
    coop::dev::spawn_npc::Init();

    // TEST-ONLY (VOTVCOOP_TEST_SAVE_ENUM=1): verify the native save browser
    // (ue_wrap::save_browser drives VOTV's loadSlots) at the menu before the ImGui
    // Host-Game picker is layered on it. No-op unless the env is set.
    coop::dev::save_probe::Init();

    // Install WM_CLOSE subclass on the game HWND so X-close runs our
    // cleanup BEFORE the engine's teardown PE calls fire. Without this
    // the PE detour stays live through UE4 shutdown, faults on
    // half-destroyed UObjects, and the process hangs at 99% RAM. The
    // window might not exist yet at this moment -- Install() retries
    // via TimelineThread's tick loop (see CoopShutdownRetry below).
    coop::shutdown::Install(&g_session);

    auto* scenario = new std::string(cfg::ReadScenario());
    if (HANDLE t = ::CreateThread(nullptr, 0, TimelineThread, scenario, 0, nullptr)) {
        ::CloseHandle(t);
    } else {
        delete scenario;
        UE_LOGE("harness: failed to start timeline thread");
    }
}

}  // namespace harness
