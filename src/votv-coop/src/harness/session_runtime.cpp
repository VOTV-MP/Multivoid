// harness/session_runtime.cpp -- the coop session's lifecycle driver on the TimelineThread: it
// owns g_session, boots the story world, brings a session up, and runs the one play loop for
// the env-configured and the menu paths (harness/session_runtime.h states the boundary).

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
#include "coop/element/intent_authority.h"
#include "coop/element/portable_identity.h"
#include "coop/net/connect_history.h"
#include "coop/net/lobby_password.h"
#include "coop/net/peer_admission.h"
#include "coop/net/peer_identity.h"
#include "coop/player/movement_ledger.h"
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
#include "coop/player/death_revive.h"
#include "coop/session/net_pump.h"
#include "coop/session/player_handshake.h"
#include "coop/text/utf8_codec.h"
#include "coop/session/session_manager.h"
#include "coop/session/shutdown.h"
#include "coop/session/subsystems.h"
#include "coop/session/teleport_client.h"
#include "coop/session/world_load_episode.h"
#include "ui/host_session_settings.h"   // it paints a failed host's reason on itself
#include "ui/host_window_native.h"      // ...and so does step one
#include "ui/server_browser.h"
#include "ui/server_browser_surface.h"  // WHICH browser this session uses
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/save_browser.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/object_index.h"
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

// The single networking session; off until a scenario or a menu action starts it.
coop::net::Session g_session;

// The host's accept predicate (Session::SetAcceptFilter): a plain function, so it converts to the
// function pointer; read on the net thread, touching only ban_list's own mutexed state.
bool BanAcceptFilter(const char* remoteIp, char* whyOut, int whyLen) {
    return !coop::ban_list::IsBanned(remoteIp, whyOut, whyLen);
}


// The shutdown hooks, run every tick regardless of possession and idempotent: the HWND subclass
// and the window title must work before the local player exists (a close on the splash).
void TickShutdownHooks() {
    coop::shutdown::Install(&g_session);
    coop::shutdown::UpdateWindowTitle();
    // The level-travel seam is armed here, unconditionally, not lazily from the pump: armed only
    // while a session runs, the single-player guarantee would rest on the hook's absence rather
    // than on the veto's own session test, and a negative-control run would grade a hook that was
    // never there. The detour is a pass-through until a death arms it, one atomic load on a rare
    // function. Idempotent; safe off the game thread.
    coop::death_revive::Install(&g_session);
    // And the watchdog that covers a failure of the pump itself.
    coop::death_revive::Watchdog();
}

void Post(GT::Task t) { GT::Post(std::move(t)); }

// Pump-composite coalescing: the TimelineThread posts the composite at 60 Hz, and when the game
// thread drains slower (a blocking world load) the queue grew without bound (a measured 35 s
// connect-to-request lag). Posting is skipped while the previous composite has not run; a
// timestamp rather than a latch, so a composite dropped by a stalled pump self-heals after
// kPumpRepostMs. Composites are idempotent per-tick logic, so a skipped post is not lost work.
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

// Spawn the second player the moment the local mainPlayer_C exists: a ~100 ms poll on the game
// thread (engine state is read only there). The flag is a shared_ptr so it outlives the loop if
// a posted check is still queued.
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
            // A mainPlayer_C also exists at the menu (the preLoad world), at the origin; a puppet
            // spawned against it lands in the menu world, which the level load then destroys. Gate
            // on a non-origin location.
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
        // No sandbox fallback: coop targets story mode through the save system.
        ::Sleep(100);  // local player not in world yet -> poll again
    }
    UE_LOGW("play: gave up waiting for local mainPlayer_C");
}

// Boot story gameplay: LoadStorySave re-issues the open each tick while still at the splash or
// the menu (a single early open is dropped) and returns true once gameplay is reached; ~1.5 s
// between opens, blocking this worker until loaded or the ~120 s cap. `forceFresh` forces the
// blank New Game path (the menu-mode join's fallback); `slotOverride` loads that slot (the
// downloaded coop slot), with `forceGameMode` carrying the host's mode, since the zcoop_ prefix
// matches none.
bool BootStorySaveBlocking(bool forceFresh, const wchar_t* slotOverride,
                           int forceGameMode) {
    // A blank New Game is the deterministic baseline the host's snapshot mirrors onto; chosen by
    // `forceFresh`, by VOTVCOOP_FRESH=1 (the test launcher sets it for the client) or by the
    // fresh_boot ini row.
    const bool freshBoot =
        !slotOverride && (forceFresh || cfg::ResolveFlag(coop::config_registry::rows::fresh_boot));
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
            // A non-fresh, non-override load is the slot this process would serve if it hosts; the
            // picker sets its own, and the coop override is a client load that never serves.
            if (!freshBoot && !slotOverride) coop::save_transfer::SetHostSlot(slot);
            return true;
        }
        ::Sleep(1500);
    }
    UE_LOGW("harness: did not reach gameplay in time (fresh_boot=%d, '%ls')", freshBoot ? 1 : 0, slot.c_str());
    return false;
}

namespace {

// The menu-mode join's world boot: wait for the save transfer (the session is already connecting
// at the menu), then load the downloaded slot; any failure falls back to the fresh-boot baseline,
// which the true-up handles more heavily. It blocks the TimelineThread, so RunPlayLoop's abort
// branch cannot run meanwhile and the Cancel, the cover timeout and a dead session are drained
// here.
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
        // Feed the loading screen the download's real progress, polled here (this loop runs at ~60
        // Hz on the timeline thread for the whole transfer) rather than pushed from the chunk sink,
        // so the net thread gains no per-chunk work. Without it the longest phase of a join, ~17 s
        // at 1 MB/s, showed a marquee.
        {
            uint32_t doneB = 0, totalB = 0;
            coop::save_transfer::GetProgress(doneB, totalB);
            coop::join_progress::NoteDownload(doneB, totalB);
        }
        // This loop blocks the thread that posts net_pump::Tick, and the transfer lives in that
        // tick (the connect edge sends the Request, event_feed delivers Begin, the host pumps
        // chunks): pump it here or the join deadlocks at "Connecting". Coalesced, never stacked
        // behind a stalled game thread.
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
        // The cover first: a tick posted before this drain must not find the join active over
        // a session the stop below has already driven to Disconnected, and fail it.
        coop::join_progress::Reset();
        if (g_session.running() && g_session.role() == coop::net::Role::Client) g_session.Stop();
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
    auto waitForApplyBlob = [] {
        namespace PIS = coop::player_inventory_sync;
        if (PIS::HasPendingApply()) return;
        const ULONGLONG w0 = ::GetTickCount64();
        while (!PIS::HasPendingApply()) {
            if (coop::shutdown::IsShuttingDown() || !g_session.running()) return;
            // A 20 s cap for the degenerate case (the host has no inventory for us, or never
            // sends); on a timeout the load proceeds and the apply hook skips, keeping the loaded
            // inventory.
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

// Bring up a session on g_session: reset the per-session edge state, wire every subsystem,
// (host) back the save up and install the LanDirect ban filter, then Start. The one path for
// "start a coop session", from the env boot and from a browser action; on the TimelineThread,
// since Start spawns the net thread and the save backup is a blocking copy. Returns Start()'s
// success, which the browser-join path uses to Fail the join when no connect edge will arrive.
bool StartCoopSession(const coop::net::Config& netCfg) {
    // Never start once teardown has begun: the net thread would be spawned after the single Stop()
    // and joined at static destruction, the loader-lock hazard the project forbids.
    if (coop::shutdown::IsShuttingDown()) {
        UE_LOGW("harness: StartCoopSession ignored -- shutdown in progress");
        return false;
    }
    // The nickname's source of truth is session_manager (config at boot, the browser after).
    {
        // session_manager holds UTF-8 (the ini and the browser's input both emit it); a per-byte
        // widen here turned every non-ASCII name into mojibake before the sanitiser saw it.
        const std::string n = coop::session_manager::Nickname();
        coop::event_feed::SetLocalNickname(coop::text::FromUtf8Lossy(n.data(), n.size()));
    }
    coop::event_feed::OnSessionStart();
    // Rows are per session (a stale anchor across a restart would read as a teleport); the call
    // also runs the ledger's un-gated arithmetic self-test.
    coop::movement_ledger::OnSessionStart();
    // The intent authorizer's un-gated arithmetic self-test (coop/element/intent_authority.h): pure
    // math, microseconds, impossible to switch off, because a wrong reach verdict does not crash,
    // it refuses a real player or authorises the map.
    coop::element::RunSelftest();
    // The portable-identity hash's un-gated self-test (coop/element/portable_identity.h), on pinned
    // vectors: the lane rests on two machines computing the same 19 characters from one string, and
    // a hash that depended on wchar_t's width would make every peer agree with itself and nobody
    // else.
    coop::element::RunSelfTest();
    // The durable identity's crypto self-test: SHA-256 and Ed25519 against published vectors, each
    // with a tamper arm, plus the decision this module exports (a signature verifies against its
    // own identity and no other). Un-gated: a verifier that accepts everything reads as working.
    coop::net::peer_identity::RunSelftest();
    // And the admission decision built on it; its arms are the ones no LAN drill can stage: a proof
    // replayed in the wrong direction, aimed at a third party, or for a stale nonce.
    coop::net::peer_admission::RunSelftest();
    // And the end-reason table every close names a code from: ids unique and in their family,
    // and a code surviving the trip through the transport's end reason.
    coop::net::end_reason::RunSelftest();
    // And the per-source history behind the connection cap and the password-guess bound: a count
    // that refuses, a refusal that lifts, a window that slides, a full table that refuses nobody.
    coop::net::connect_history::RunSelftest();
    // And the lobby password inside it: if the salt were ignored, every locked lobby would open to
    // one table, and the only visible difference is that joins keep succeeding. The negatives are
    // the test: one password under two host keys must not collide, an empty password must refuse to
    // derive, a tag must not verify against another nonce.
    coop::net::lobby_password::RunSelftest();
    // Reset net_pump's edge detectors, so a Stop/Start on one process carries no stale "was
    // connected" or "was holding" entries into the new session.
    coop::net_pump::OnSessionStart();
    coop::prop_lifecycle::SetSession(&g_session);
    coop::npc_sync::SetSession(&g_session);
    coop::prop_snapshot::SetSession(&g_session);
    coop::dev::restore_vitals::SetSession(&g_session);
    coop::teleport_client::SetSession(&g_session);
    coop::dev::force_weather::SetSession(&g_session);
    coop::dev_gate::SetSession(&g_session);  // the strict CLIENT lockout for every dev feature
    coop::moderation::SetSession(&g_session);
    // The per-player inventory subsystem installs pre-world, here, not through the world-gated
    // subsystems::Install: its receiver buffers the host's pushed blob during the menu-mode
    // transfer wait, and the SaveObjectReadyHook must be armed before BootStorySaveBlocking so it
    // fires on the join's own load. Idempotent across Stop/Start.
    coop::player_inventory_sync::Install(&g_session);
    if (netCfg.role == coop::net::Role::Host) {
        // Snapshot the canonical save before coop injects state (host only; clients are
        // save-blocked); synchronous, so it completes before Start.
        coop::save_guard::BackupSaveOnSessionStart();
        // The seen-players registry, host bookkeeping on any topology (on P2P the IP may stay
        // empty).
        coop::seen_players::Load();
        // LanDirect only: the IP-keyed ban filter fails closed on P2P (the Connecting edge has no
        // remote address, and a public IP is the wrong key there); P2P bans are identity-based.
        if (netCfg.topology == coop::net::Topology::LanDirect) {
            coop::ban_list::Load();
            g_session.SetAcceptFilter(&BanAcceptFilter);
        }
    }
    // The client's connecting state is not raised here but by the browser connect actions, so the
    // loading screen is browser-join only; the env and autotest client boot reaches this function
    // directly and shows nothing.
    const bool ok = g_session.Start(netCfg);
    UE_LOGI("harness: ==== COOP SESSION START (%s / %s)%s ====",
            netCfg.role == coop::net::Role::Host ? "host" : "client",
            netCfg.topology == coop::net::Topology::P2P ? "p2p" : "lan-direct",
            ok ? "" : " -- START FAILED");
    return ok;
}

namespace {

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

// The host-with-save orchestration: if one was queued (the picker's "Host selected save" or
// "New Game & Host"), load the chosen world or create the new save first, polling like
// BootStorySaveBlocking, then StartCoopSession. A no-op when nothing is queued; on the
// TimelineThread, where the blocking load and the start belong. One place owns load-then-host.
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
            StartCoopSession(b->ph.cfg);
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

}  // namespace

// The main loop on the TimelineThread. Each tick: with no session running, poll session_manager
// for a browser-initiated start and boot it here (Start and the save backup must not run on the
// game thread); post the per-tick pump; ~2 s stats while running. One loop for the env "play"
// path and the native menu path. `idleInGameplay` names the idle state: solo gameplay (keep the
// local observers and the roster live) or the main menu (the gameplay classes are not loaded, so
// the observers install when a session starts). No per-tick world probe: a per-frame
// object-array scan is the FPS pattern the perf rule forbids, and the scenario flag is free.
namespace {

// The lobby's live player count for the heartbeat, on the announcer's worker thread, so atomics
// only: running() and connectedPeerCount() (the peerConns_ and ready arrays); g_session is
// process-lifetime. Not role(), a plain field, and only a host announces. +1 is the host itself;
// before the session starts (the lobby is announced while the world loads) 1 is the honest
// answer. A joiner is undercounted for its admission round trip (the pending band holds no seat),
// invisible at a 30 s cadence.
int LobbyPlayerCount() {
    if (!g_session.running()) return 1;
    return g_session.connectedPeerCount() + 1;
}

}  // namespace

void InstallLobbyPlayerCountSource() {
    coop::session_manager::SetPlayerCountSource(&LobbyPlayerCount);
}

void RunPlayLoop(bool idleInGameplay) {
    int tick = 0;
    bool wasRunning = false;     // the session's running-to-stopped edge
    bool wasHostSession = false;  // and whether the session that was running was the host's
    while (!coop::shutdown::IsShuttingDown()) {
        // A client join abort (Cancel, or a connect failure): Stop here (the thread Stop's join
        // belongs on), drop the loading state, reopen the browser; multiplayer_menu restores the
        // hidden menu once join_progress goes inactive. One drain for both paths.
        if (coop::join_progress::TakeAbortRequest()) {
            // Only a client join is abortable here; a stale client cancel must never Stop a host
            // session that has since started (a self-join Cancel once killed the host the instant
            // it started).
            const bool isClientSession =
                g_session.running() && g_session.role() == coop::net::Role::Client;
            if (isClientSession) {
                UE_LOGI("harness: join aborted -- stopping the client session + reopening the browser");
                coop::join_progress::Reset();  // the cover first, as in the transfer loop's drain
                g_session.Stop();
                ui::server_browser_surface::Open();
            } else {
                // A host running, or nothing: a stale client abort; clear the cover only, never
                // Stop the host or pop the browser over gameplay.
                UE_LOGI("harness: stale join-abort with no client session -- clearing the cover only");
                coop::join_progress::Reset();
            }
            // A failed or cancelled join is a milestone a bug report must capture: flush the log
            // now rather than leave it in the buffered stream.
            ue_wrap::log::Flush();
        }
        if (!g_session.running() && !coop::shutdown::IsShuttingDown()) {
            // The save picker: load the chosen world (or create the save), then host; blocks until
            // done, a no-op if nothing is queued.
            DriveHostBootIfPending();
            // Browser Join / Direct connect: start immediately on the current world.
            if (!g_session.running()) {
                coop::net::Config pending;
                if (coop::session_manager::TakePendingStart(pending)) {
                    // The stale-start guard: a browser client join raises join_progress at the
                    // click, and the master round trip can QueueStart after the player cancelled; a
                    // start whose join is no longer Active is discarded rather than ghost-started.
                    // A host start carries no join_progress and always proceeds.
                    if (pending.role == coop::net::Role::Client && !coop::join_progress::Active()) {
                        UE_LOGI("harness: discarding stale browser client start -- join no longer active (cancelled/failed)");
                    } else {
                        UE_LOGI("harness: browser-initiated coop session");
                        // The menu-mode client join: connect at the menu, download the host's save,
                        // load that world (the engine places every prop naturally, with the host's
                        // keys), then net_pump announces world-ready and the host replays; the
                        // player never sees a divergent fresh world. The fallbacks (no save, a
                        // failed transfer, a timeout) fresh-boot the baseline and the true-up
                        // degrades to the heavy reconcile. Blocks the TimelineThread, the abort
                        // drained inside. A join from inside gameplay connects directly, since it
                        // has a world.
                        if (pending.role == coop::net::Role::Client && !idleInGameplay) {
                            UE_LOGI("harness: menu-mode client join -- save-transfer bootstrap");
                            coop::save_transfer::ClientArm();
                            // A synchronous Start failure means no connect edge will ever clear the
                            // cover: Fail drops it and reopens the browser.
                            if (!StartCoopSession(pending))
                                coop::join_progress::Fail(coop::net::EndReason::CouldNotStart, "");
                            else
                                DriveMenuModeJoinWorldBoot();
                        } else if (!StartCoopSession(pending)) {
                            coop::join_progress::Fail(coop::net::EndReason::CouldNotStart, "");
                        }
                    }
                }
            }
        }
        const bool running = g_session.running();
        // A host session's death returns the player to the main menu, so they always know it ended.
        // Host only: a client disconnect is already fled by net_pump, and a client cancel by the
        // abort branch above; fleeing those again would arm the death bypass on a normal cancel and
        // break a same-process retry. Idempotent through net_pump's latch.
        if (wasRunning && wasHostSession && !running && !coop::shutdown::IsShuttingDown()) {
            UE_LOGI("harness: host session ended -- returning to the main menu");
            Post([] { coop::net_pump::FleeToMainMenuOnDeath(g_session, "host session ended -> main menu"); });
            // The lobby's lifetime is the host session's: delist now, or the heartbeat keeps a dead
            // lobby listed forever (a host killed in the world stayed in the browser). Blocking
            // HTTP is fine on the TimelineThread.
            coop::session_manager::EndHostedLobby();
        }
        wasRunning = running;
        if (running) wasHostSession = (g_session.role() == coop::net::Role::Host);
        PostPumpComposite([running, idleInGameplay] {
            if (running) {
                coop::net_pump::Tick(g_session);   // drains the object index first
            } else {
                // No session: the object index still follows the engine, so a later session starts
                // from a current one rather than a backlog.
                ue_wrap::object_index::Drain();
                if (idleInGameplay) {
                    // Solo gameplay, no session yet: keep the local observers live.
                    coop::subsystems::Install(g_session);
                }
            }
            // The roster needs a live world and player; skipped at the menu.
            if (running || idleInGameplay) {
                coop::roster::Refresh();
            }
            // Always, self-clearing: re-project the nameplates (an empty snapshot with no puppets,
            // so the HUD auto-hides at the menu), age the chat feed, tick the dev overlays; all
            // cheap no-ops when idle.
            coop::nameplate::Update();
            coop::dev::object_overlay::Update(); coop::dev::ragdoll_bone_overlay::Update();
            coop::chat_feed::Tick();
            // Always: the close subclass and the window title must work at the menu too.
            TickShutdownHooks();
        });
        if (running && ++tick % 120 == 0) {  // ~every 2 s at 60 Hz: stats for the LAN tests
            Post([] {
                UE_LOGI("net stats: state=%d sent=%llu recv=%llu puppet=%d",
                        static_cast<int>(g_session.state()),
                        static_cast<unsigned long long>(g_session.packetsSent()),
                        static_cast<unsigned long long>(g_session.packetsRecv()),
                        coop::puppet_drive::Puppet(1).valid() ? 1 : 0);
                // A memory heartbeat every ~30 s, so a log can tell a slow climb from a spike at
                // death.
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
