// harness/session_runtime.cpp -- the coop-session lifecycle driver on the
// TimelineThread: owns g_session + world boot + session bringup + the unified
// play loop. Extracted verbatim from harness/harness.cpp (2026-07-19 s27 cut);
// interfaces + the boundary rationale in harness/session_runtime.h.

#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/comms/chat_feed.h"
#include "coop/creatures/npc_sync.h"
#include "coop/dev/dev_gate.h"
#include "coop/dev/force_weather.h"
#include "coop/dev/object_overlay.h"
#include "coop/dev/ragdoll_bone_overlay.h"
#include "coop/dev/restore_vitals.h"
#include "coop/dispatch/event_feed.h"
#include "coop/items/player_inventory_sync.h"
#include "coop/moderation/ban_list.h"
#include "coop/moderation/moderation.h"
#include "coop/moderation/seen_players.h"
#include "coop/net/session.h"
#include "coop/player/nameplate.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster.h"
#include "coop/props/prop_lifecycle.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/save_guard.h"
#include "coop/save/save_transfer.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/player_handshake.h"
#include "coop/session/session_manager.h"
#include "coop/session/shutdown.h"
#include "coop/session/subsystems.h"
#include "coop/session/teleport_client.h"
#include "coop/session/world_load_episode.h"
#include "ui/server_browser.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/save_browser.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <windows.h>
#define PSAPI_VERSION 2      // K32GetProcessMemoryInfo from kernel32 -- no psapi.lib link
#include <psapi.h>           // GetProcessMemoryInfo (the 30 s mem heartbeat)

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace harness::session_runtime {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

namespace cfg = coop::config;

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


// Puppet array + per-slot edge state + held-prop edge detector + the local-
// pose read + the per-tick observer orchestrator + the main NetPumpTick body
// extracted to coop/net_pump.cpp (PR-4.13). Harness reaches the puppets via
// coop::puppet_drive::Puppet(slot) and calls coop::net_pump::Tick(g_session)
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

// NetPumpTick body extracted to coop/net_pump.cpp (PR-4.13). Harness call
// sites in the timeline tick lambdas dispatch via
// coop::net_pump::Tick(g_session) instead.

void Post(GT::Task t) { GT::Post(std::move(t)); }

// Per-tick pump-composite coalescing (v56 menu-window fix, 2026-06-10). The
// TimelineThread posts the coop pump composite at 60 Hz UNCONDITIONALLY; when
// the game thread drains slower (a blocking world load, a menu-window tick that
// was made expensive by a bug), the queue grows without bound -- the measured
// 35 s connect->Request lag was exactly this backlog (60 Hz posted vs ~7 Hz
// drained). Bound it: skip posting while the previously posted composite has
// not RUN yet. Timestamp (not a bool latch) so a composite dropped by a stalled
// /bypassed pump self-heals after kPumpRepostMs instead of wedging the pump
// forever. Composites are idempotent per-tick logic, so skipped posts are not
// lost work -- the next one reads the same current state.
std::atomic<unsigned long long> g_pumpPostedAtMs{0};  // 0 = none in flight
constexpr unsigned long long kPumpRepostMs = 500;

template <typename Body>
void PostPumpComposite(Body&& body) {
    const unsigned long long now = ::GetTickCount64();
    const unsigned long long inFlight = g_pumpPostedAtMs.load(std::memory_order_relaxed);
    if (inFlight != 0 && now - inFlight < kPumpRepostMs) return;  // previous still queued
    g_pumpPostedAtMs.store(now, std::memory_order_relaxed);
    Post([body = std::forward<Body>(body)] {
        body();
        g_pumpPostedAtMs.store(0, std::memory_order_relaxed);
    });
}

}  // namespace

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
            if (coop::puppet_drive::Puppet(1).valid()) { state->store(2); return; }
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
            state->store(coop::puppet_drive::Puppet(1).Spawn() ? 2 : 3);
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
// browser CLIENT join's FALLBACK (host had no save / transfer failed).
// `slotOverride` (v56 save-transfer): load THIS slot instead of the env/ini one --
// the menu-mode join loads the downloaded zcoop_<pid> slot; `forceGameMode` threads
// the host's wire-carried mode (the zcoop_ prefix can't prefix-match one).
bool BootStorySaveBlocking(bool forceFresh, const wchar_t* slotOverride,
                           int forceGameMode) {
    // FRESH-BOOT (2026-06-04, project-ephemeral-client-host-authoritative-world): a BLANK New
    // Game (StartFreshGame) instead of loading a save slot is the deterministic baseline the
    // host's connect-snapshot mirrors onto. Driven by `forceFresh` (the menu-mode client join,
    // 2026-06-06), the env override VOTVCOOP_FRESH=1 (the test launcher forces the CLIENT fresh
    // every run -- mp.py sets it per role), OR the `fresh_boot=1` ini test gate.
    const bool freshBoot = !slotOverride &&
                           (forceFresh ||
                            (cfg::ReadEnv("VOTVCOOP_FRESH") == "1") ||
                            (cfg::ReadIniValue("fresh_boot", "0") == "1"));
    // STORY save slot: an explicit override (the v56 coop slot) > env VOTVCOOP_SAVE (the test
    // launcher pins the HOST's save per run -- mp.py sets it) > multivoid.ini "save=<slot>" >
    // default s_may2026. Coop targets story mode, so we never boot the sandbox map fresh.
    std::wstring slot;
    if (slotOverride) {
        slot = slotOverride;
    } else {
        std::string slotA = cfg::ReadEnv("VOTVCOOP_SAVE");
        if (slotA.empty()) slotA = cfg::ReadIniValue("save", "s_may2026");
        slot.assign(slotA.begin(), slotA.end());  // ASCII slot name
    }
    UE_LOGI("harness: target %s '%ls'", freshBoot ? "FRESH New Game (blank save)" : "STORY save", slot.c_str());
    for (int i = 0; i < 80; ++i) {  // ~120 s cap (boot + omega + level load)
        if (coop::shutdown::IsShuttingDown()) {
            UE_LOGI("harness: BootStorySaveBlocking aborting -- shutdown signaled");
            return false;
        }
        auto st = std::make_shared<std::atomic<int>>(0);  // 0 pending,1 retry,2 ok
        Post([slot, st, freshBoot, forceGameMode] {
            const bool inGame = freshBoot ? ue_wrap::engine::StartFreshGame(/*storyMode=*/true)
                                          : ue_wrap::engine::LoadStorySave(slot.c_str(), forceGameMode);
            st->store(inGame ? 2 : 1);
        });
        while (st->load() == 0) ::Sleep(5);
        if (st->load() == 2) {
            // v56: a non-fresh, non-override load (env/ini path) is the slot this
            // process would SERVE if it hosts -- remember it for the save transfer.
            // (The browser Host-Game picker sets its own in DriveHostBootIfPending;
            // the zcoop override is a CLIENT load and never serves.)
            if (!freshBoot && !slotOverride) coop::save_transfer::SetHostSlot(slot);
            return true;
        }
        ::Sleep(1500);
    }
    UE_LOGW("harness: did not reach gameplay in time (fresh_boot=%d, '%ls')", freshBoot ? 1 : 0, slot.c_str());
    return false;
}

namespace {

// v56: the menu-mode join's WORLD boot -- wait for the save transfer (the session
// is already connecting/connected at the menu), then load the downloaded zcoop
// slot; any failure falls back to the pre-v56 fresh-boot baseline (the true-up
// handles it, just heavier). Runs on the TimelineThread and blocks it exactly
// like the old pre-connect fresh boot did -- which means RunPlayLoop's abort
// branch cannot run meanwhile, so the Cancel button / cover-timeout / dead
// session are drained HERE (mirroring that branch's client-session reaction).
void DriveMenuModeJoinWorldBoot() {
    namespace ST = coop::save_transfer;
    const ULONGLONG t0 = ::GetTickCount64();
    bool aborted = false;
    for (;;) {
        const ST::ClientState st = ST::GetClientState();
        if (st == ST::ClientState::ReadySlotWritten ||
            st == ST::ClientState::NoSaveAvailable ||
            st == ST::ClientState::Failed) break;
        if (coop::shutdown::IsShuttingDown()) return;
        if (coop::join_progress::TakeAbortRequest()) { aborted = true; break; }
        if (!coop::join_progress::Active()) { aborted = true; break; }  // cover reset (failsafe)
        if (!g_session.running()) { aborted = true; break; }            // connect died
        if (::GetTickCount64() - t0 > 120000) {
            UE_LOGW("harness: save transfer timed out (120 s) -- falling back to a fresh world");
            break;
        }
        // THIS LOOP BLOCKS THE THREAD THAT POSTS net_pump::Tick (RunPlayLoop /
        // the play branch) -- and the transfer itself LIVES in that tick (the
        // connect edge sends the Request, event_feed delivers Begin, the host
        // pumps chunks). Pump it here or the join deadlocks at "Connecting"
        // (2026-06-10 smoke: client armed, in=60 pkt/s, out=0, no Request ever).
        // Coalesced: never stack composites behind a stalled game thread.
        PostPumpComposite([] {
            coop::net_pump::Tick(g_session);
            coop::nameplate::Update();
            coop::dev::object_overlay::Update(); coop::dev::ragdoll_bone_overlay::Update();
            coop::chat_feed::Tick();
            TickShutdownHooks();
        });
        ::Sleep(16);  // ~60 Hz, same cadence as RunPlayLoop
    }
    if (aborted) {
        UE_LOGI("harness: menu-mode join aborted during the save transfer");
        if (g_session.running() && g_session.role() == coop::net::Role::Client) g_session.Stop();
        coop::join_progress::Reset();
        ui::server_browser::Open();
        ue_wrap::log::Flush();
        return;
    }
    // v73: WAIT (pumping net) for the host's per-player apply blob BEFORE loading the world, so the
    // pre-materialize SaveObjectReadyHook has this client's inventory at load time. The host pushes it
    // the moment our GUID arrives (in the Join, right after connect), so it has almost always arrived
    // during the save-transfer wait above -- this is the safety barrier. If it is already here we skip
    // the wait. Both load branches below wait first.
    auto waitForApplyBlob = [] {
        namespace PIS = coop::player_inventory_sync;
        if (PIS::HasPendingApply()) return;
        const ULONGLONG w0 = ::GetTickCount64();
        while (!PIS::HasPendingApply()) {
            if (coop::shutdown::IsShuttingDown() || !g_session.running()) return;
            // 20 s safety cap. In practice the apply blob lands in ~1 RTT -- almost always DURING the
            // save-transfer download above, before this wait even begins (HasPendingApply() is then
            // already true and we never spin here). The cap only fires for a degenerate case: the host
            // has no inventory for us / a version-mismatched host that never sends. On timeout we load
            // anyway and the apply hook SKIPS (keeps the loaded inventory, never wipes).
            if (::GetTickCount64() - w0 > 20000) {
                UE_LOGW("harness: inventory apply blob did not arrive in 20s -- loading with the "
                        "loaded inventory (the apply hook will skip)");
                return;
            }
            PostPumpComposite([] {
                coop::net_pump::Tick(g_session);
                coop::nameplate::Update();
                coop::dev::object_overlay::Update(); coop::dev::ragdoll_bone_overlay::Update();
                coop::chat_feed::Tick();
                TickShutdownHooks();
            });
            ::Sleep(16);
        }
        UE_LOGI("harness: inventory apply blob ready -- proceeding to load the world");
    };

    // v107 (2026-07-08) HOST-WIPE ROOT FIX: arm the world-load episode BEFORE the boot that triggers the
    // game's mainGamemode.loadObjects pre-delete. During that load the destroy seam suppresses the
    // KEYED-prop destroy broadcasts this client's world-rebuild churns -- otherwise the v106 seam carries
    // the destroy half to the host, which destroys its AUTHORITATIVE copies by key and empties its world
    // (measured bare join: host 3345->1255 keyed props). join_membership_sweep ends the episode at
    // load-tail quiescence, after which the legit post-load intent destroys broadcast normally. This is
    // the sole, client-only arm site, CAUSALLY before the burst on every path (the boot below triggers
    // loadObjects, whose pre-delete IS the burst). See coop/session/world_load_episode.h + /qf rounds 0-13.
    coop::world_load_episode::Arm();

    if (ST::GetClientState() == ST::ClientState::ReadySlotWritten) {
        // Load the host's world from the downloaded slot. ResetCachedSave first
        // (B7): a rejoin in this process must not re-register a stale cached
        // save. Slot is per-pid; the GameMode came over the wire.
        const std::wstring slot = ST::CoopSlotName();
        const int mode = static_cast<int>(ST::ReceivedGameMode());
        UE_LOGI("harness: save received -- loading coop slot '%ls' (mode=%d)", slot.c_str(), mode);
        auto rst = std::make_shared<std::atomic<int>>(0);
        Post([rst] { ue_wrap::engine::ResetCachedSave(); rst->store(1); });
        while (rst->load() == 0 && !coop::shutdown::IsShuttingDown()) ::Sleep(5);
        waitForApplyBlob();
        if (!BootStorySaveBlocking(/*forceFresh=*/false, slot.c_str(), mode)) {
            UE_LOGW("harness: coop-slot load did not reach gameplay -- falling back fresh");
            BootStorySaveBlocking(/*forceFresh=*/true);
        }
    } else {
        UE_LOGI("harness: host save unavailable/failed -- fresh-booting the ephemeral baseline");
        waitForApplyBlob();
        BootStorySaveBlocking(/*forceFresh=*/true);
    }
}

}  // namespace

// Bring up a coop session on g_session: reset per-session edge state, wire every sync
// subsystem, (host) back up the save + install the LanDirect ban filter, then Start.
// ONE code path for "start a coop session" (RULE 2) -- called by BOTH the env-configured
// boot (ReadNetConfig) AND a browser action consumed via session_manager::
// TakePendingStart. Runs on the TimelineThread: g_session.Start spawns the net thread,
// and the host save-backup is a blocking file copy -- neither belongs on the game thread.
// Returns Start()'s success: the browser-join path Fails the join (drops the loading
// screen + reopens the browser) when a synchronous Start failure means no async connect
// edge will ever arrive. Other callers (env play / host-with-save) ignore it.
bool StartCoopSession(const coop::net::Config& netCfg) {
    // Never start once teardown has begun: g_session.Start would spawn a net thread
    // AFTER DoShutdown's single Stop(), to be joined at static destruction -- the
    // loader-lock join-from-teardown hazard the project forbids (audit P2).
    if (coop::shutdown::IsShuttingDown()) {
        UE_LOGW("harness: StartCoopSession ignored -- shutdown in progress");
        return false;
    }
    // Nickname source of truth = session_manager (seeded from config at boot, overwritten
    // by the server browser). Apply it here so a browser-set name wins over the config default.
    {
        const std::string n = coop::session_manager::Nickname();
        std::wstring wn;
        wn.reserve(n.size());
        for (char c : n) wn.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        coop::event_feed::SetLocalNickname(wn);
    }
    coop::event_feed::OnSessionStart();
    // Reset net_pump edge-detector state so a Stop()/Start() cycle on the same process
    // doesn't carry stale "was connected" / "was holding prop" entries into the new
    // session (phantom disconnect edge / suppressed connect replay / stale prop key).
    coop::net_pump::OnSessionStart();
    coop::prop_lifecycle::SetSession(&g_session);
    coop::npc_sync::SetSession(&g_session);
    coop::prop_snapshot::SetSession(&g_session);
    coop::dev::restore_vitals::SetSession(&g_session);
    coop::teleport_client::SetSession(&g_session);
    coop::dev::force_weather::SetSession(&g_session);
    coop::dev_gate::SetSession(&g_session);  // the strict CLIENT lockout for every dev feature
    coop::moderation::SetSession(&g_session);
    // v73 Inc4: the per-player inventory subsystem MUST install PRE-WORLD (here, with the other
    // SetSession caches) -- NOT via the world-gated subsystems::Install. Both halves run before the
    // join's world exists: OnReliable buffers the host's pushed apply blob during the menu-mode
    // save-transfer wait, and the SaveObjectReadyHook must be ARMED before BootStorySaveBlocking so
    // it fires on the join's OWN pre-materialize load. The old world-up call site left g_session
    // null during the wait (every apply chunk silently dropped at OnReliable's `if (!s) return`) AND
    // armed the hook AFTER the load -> the apply never ran. This was the "inventory never worked"
    // root cause (2026-06-16 timing hunt: the blob arrived at the net thread during the join but was
    // never assembled until world-up, far too late). Idempotent across Stop()/Start().
    coop::player_inventory_sync::Install(&g_session);
    if (netCfg.role == coop::net::Role::Host) {
        // Snapshot the canonical save BEFORE coop injects state (host-only; clients are
        // save-blocked). Synchronous on this bringup thread -> completes before Start.
        coop::save_guard::BackupSaveOnSessionStart();
        // Seen-players registry (F1 Administration): any-topology host bookkeeping
        // (on P2P the IP field may stay empty -- the offline-ban path surfaces that).
        coop::seen_players::Load();
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

namespace {

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
            const bool inGame = ue_wrap::engine::LoadStorySave(b->slot.c_str());
            // v56: the picked slot is what this host will SERVE to joiners.
            if (inGame) coop::save_transfer::SetHostSlot(b->slot);
            b->st.store(inGame ? 2 : 1);
        });
        while (b->st.load() == 0 && !coop::shutdown::IsShuttingDown()) ::Sleep(5);
        const int s = b->st.load();
        if (s == 2) {
            UE_LOGI("harness: host-with-save world loaded ('%ls') -- starting host session", b->slot.c_str());
            StartCoopSession(b->ph.cfg);
            coop::join_progress::Reset();  // world up + session started -> drop the host cover -> gameplay
            return;
        }
        if (s == 3) {
            UE_LOGW("harness: host-with-save create/load FAILED -- aborting host");
            // The lobby was already announced (HostWithSave announces before the load) --
            // cancel it so no phantom lobby lingers on the master (audit HIGH-1). Skip the
            // /leave HTTP during teardown.
            if (!coop::shutdown::IsShuttingDown()) coop::session_manager::EndHostedLobby();
            // Don't strand the user on a blank menu (the picker closed on click): drop the
            // host cover, surface the failure + reopen the browser so they can retry.
            coop::join_progress::Reset();
            coop::session_manager::SetHostStatus(
                b->ph.save.newGame ? "Host failed: could not create the new save"
                                   : "Host failed: could not load that save");
            if (!coop::shutdown::IsShuttingDown()) ui::server_browser::Open();
            return;
        }
        ::Sleep(1500);  // throttle LoadStorySave's `open` re-issue (matches BootStorySaveBlocking)
    }
    if (!coop::shutdown::IsShuttingDown()) {
        UE_LOGW("harness: host-with-save did not reach gameplay in time -- aborting host");
        coop::session_manager::EndHostedLobby();  // HIGH-1: don't leave a phantom lobby on timeout
        coop::join_progress::Reset();        // drop the host cover
        coop::session_manager::SetHostStatus("Host failed: the world did not load in time");
        ui::server_browser::Open();
    }
}

}  // namespace

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
    bool wasRunning = false;     // FIX 4: detect the coop session running->stopped edge
    bool wasHostSession = false; // ...and whether the session that was running was the HOST's
    while (!coop::shutdown::IsShuttingDown()) {
        // Client join ABORT (Cancel button OR a connect failure). Stop the session here
        // (the timeline thread -- where Stop, which joins the net thread, belongs), drop the
        // loading state, and reopen the browser so the user can pick again. multiplayer_menu
        // restores the hidden menu once join_progress goes inactive. ONE drain for both the
        // user-cancel and the auto-fail paths (regression C, 2026-06-06).
        if (coop::join_progress::TakeAbortRequest()) {
            // Only a CLIENT join is abortable here. A host boot never drives this path
            // (BeginHostBoot has no Cancel; host-mode MaybeTimeout just Resets) -- but a
            // STALE client-cancel must NEVER Stop a host session that has since started.
            // (2026-06-08 repro: a self-join Cancel killed the host the instant it started.)
            const bool isClientSession =
                g_session.running() && g_session.role() == coop::net::Role::Client;
            if (isClientSession) {
                UE_LOGI("harness: join aborted -- stopping the client session + reopening the browser");
                g_session.Stop();            // -> running->stopped edge -> FleeToMainMenu (-> main menu)
                coop::join_progress::Reset();
                ui::server_browser::Open();
            } else {
                // Host session running, or nothing running: a stale client-abort. Clear the
                // cover only -- do NOT Stop the host and do NOT pop the browser over gameplay.
                UE_LOGI("harness: stale join-abort with no client session -- clearing the cover only");
                coop::join_progress::Reset();
            }
            // A failed/cancelled join is a milestone a real user's bug report must capture --
            // land the abort sequence on disk now rather than leaving it in the buffered INFO
            // stream (a force-kill / quiet menu would lose it). Mirrors the boot-ready Flush.
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
                        // Menu-mode CLIENT join -- v56 SAVE-TRANSFER BOOTSTRAP (user mandate
                        // 2026-06-10 "pull all objects data at connecting time"): connect AT
                        // THE MENU first, download the HOST's save, load THAT world (the
                        // engine places every prop naturally, host keys, at rest), then
                        // net_pump announces world-ready and the host replays. The player
                        // never sees a divergent fresh world. Fallbacks (host has no save /
                        // transfer failed / timeout) fresh-boot the pre-v56 ephemeral
                        // baseline -- the all-key true-up degrades to the heavy reconcile,
                        // which run-5 proved survivable. Blocks on the TimelineThread like
                        // the old pre-connect fresh boot (abort drained inside).
                        // An already-in-gameplay browser join (idleInGameplay: the env/
                        // autotest play path, or a future in-game join) connects directly --
                        // it has a world; WorldReady fires within a tick.
                        if (pending.role == coop::net::Role::Client && !idleInGameplay) {
                            UE_LOGI("harness: menu-mode client join -- save-transfer bootstrap");
                            coop::save_transfer::ClientArm();
                            // A synchronous Start failure means no async connect edge will
                            // ever arrive to clear the cover -- Fail (drops it + reopens the
                            // browser). (regression C, 2026-06-06.)
                            if (!StartCoopSession(pending))
                                coop::join_progress::Fail("could not start the connection");
                            else
                                DriveMenuModeJoinWorldBoot();
                        } else if (!StartCoopSession(pending)) {
                            coop::join_progress::Fail("could not start the connection");
                        }
                    }
                }
            }
        }
        const bool running = g_session.running();
        // FIX 4 (2026-06-08) -- a HOST session death returns the user to the MAIN MENU so
        // they always KNOW it ended (the repro left the host stranded in a dead-session
        // world). HOST-ONLY on purpose: a CLIENT disconnect is already fled to the menu by
        // net_pump (its own disconnect/death edge), and a CLIENT join-cancel is handled by
        // the abort branch above -- fleeing those again here would arm the 30-min death
        // bypass on a normal cancel and break a same-process retry (audit). Idempotent via
        // net_pump's g_fleeing latch. Skipped during shutdown.
        if (wasRunning && wasHostSession && !running && !coop::shutdown::IsShuttingDown()) {
            UE_LOGI("harness: host session ended -- returning to the main menu");
            Post([] { coop::net_pump::FleeToMainMenuOnDeath(g_session, "host session ended -> main menu"); });
            // The lobby's lifetime IS the host session's lifetime: delist NOW (/leave +
            // stop the heartbeat) or the heartbeat keeps the DEAD lobby listed on the
            // master forever (2026-07-04: host died to the killerwisp -> everyone fled
            // to the menu -> the server was still in the browser). Blocking HTTP is fine
            // here: this is the TimelineThread, same as the boot-failure delist above.
            coop::session_manager::EndHostedLobby();
        }
        wasRunning = running;
        if (running) wasHostSession = (g_session.role() == coop::net::Role::Host);
        PostPumpComposite([running, idleInGameplay] {
            if (running) {
                coop::net_pump::Tick(g_session);
            } else if (idleInGameplay) {
                // Solo gameplay, no session yet: keep the local observers live.
                coop::subsystems::Install(g_session);
            }
            // roster needs a live world+player; skip it at the menu.
            if (running || idleInGameplay) {
                coop::roster::Refresh();
            }
            // ALWAYS (self-clearing): re-project the ImGui nameplates -- an empty
            // snapshot when there are no puppets / no local player, so the HUD
            // auto-hides at the menu -- and age out the chat feed. Both are cheap
            // no-ops when idle. Same for the dev object overlay (one atomic load
            // while its menu toggle is off).
            coop::nameplate::Update();
            coop::dev::object_overlay::Update(); coop::dev::ragdoll_bone_overlay::Update();
            coop::chat_feed::Tick();
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
                        coop::puppet_drive::Puppet(1).valid() ? 1 : 0);
                // Memory heartbeat (2026-07-04, the 17:10 "host ate RAM then died" report):
                // the log had ZERO memory observability, so "was it climbing for minutes or
                // spiking at death?" is unanswerable. 30 s cadence names the shape next time.
                static int memTick = 0;
                if (++memTick % 15 == 0) {  // every 15th 2s-stats = ~30 s
                    PROCESS_MEMORY_COUNTERS_EX pmc{};
                    if (::GetProcessMemoryInfo(::GetCurrentProcess(),
                            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
                        UE_LOGI("mem: ws=%lluMB private=%lluMB peak-ws=%lluMB",
                                static_cast<unsigned long long>(pmc.WorkingSetSize >> 20),
                                static_cast<unsigned long long>(pmc.PrivateUsage >> 20),
                                static_cast<unsigned long long>(pmc.PeakWorkingSetSize >> 20));
                    }
                }
                if (void* lp = coop::players::Registry::Get().Local()) {
                    const auto loc = ue_wrap::engine::GetActorLocation(lp);
                    const auto rot = ue_wrap::engine::GetActorRotation(lp);
                    ue_wrap::FRotator cRot{};
                    if (void* c = ue_wrap::engine::GetController(lp)) cRot = ue_wrap::engine::GetControlRotation(c);
                    UE_LOGI("pos diag: local actor=(%.0f,%.0f,%.0f) actorYaw=%.1f ctrl(P=%.1f Y=%.1f)",
                            loc.X, loc.Y, loc.Z, rot.Yaw, cRot.Pitch, cRot.Yaw);
                }
                if (coop::puppet_drive::Puppet(1).valid()) {
                    const auto p = coop::puppet_drive::Puppet(1).GetLocation();
                    UE_LOGI("pos diag: puppet world=(%.0f,%.0f,%.0f)", p.X, p.Y, p.Z);
                }
            });
        }
        ::Sleep(running ? 16 : 50);  // 60 Hz pump when active; 20 Hz idle poll otherwise
    }
}

coop::net::Session& Session() {
    return g_session;
}

}  // namespace harness::session_runtime
