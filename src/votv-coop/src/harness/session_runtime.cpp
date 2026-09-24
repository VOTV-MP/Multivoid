// harness/session_runtime.cpp -- the coop session's lifecycle driver on the TimelineThread: it
// owns g_session, brings a session up, and runs the one play loop for the env-configured and the
// menu paths (harness/session_runtime.h states the boundary).

#include "harness/session_runtime.h"

#include "harness/pump.h"
#include "harness/world_boot.h"

#include "coop/config/config.h"
#include "coop/comms/chat_feed.h"
#include "coop/creatures/npc_sync.h"
#include "coop/dev/dev_gate.h"
#include "coop/dev/force_weather.h"
#include "coop/dev/hotbar_icon_probe.h"
#include "coop/dev/load_reroll_watch.h"
#include "coop/dev/object_overlay.h"
#include "coop/dev/ragdoll_bone_overlay.h"
#include "coop/dev/rehost_rejoin.h"
#include "coop/dev/restore_vitals.h"
#include "coop/dispatch/event_feed.h"
#include "coop/items/hotbar_icon_edge.h"
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
#include "coop/props/container_park.h"
#include "coop/props/container_write_policy.h"
#include "coop/props/prop_lifecycle.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/save_guard.h"
#include "coop/save/save_transfer.h"
#include "coop/session/join_beacon.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/player_handshake.h"
#include "coop/session/rig_ready.h"
#include "coop/text/utf8_codec.h"
#include "coop/session/session_manager.h"
#include "coop/session/shutdown.h"
#include "coop/session/subsystems.h"
#include "coop/session/teleport_client.h"
#include "ui/server_browser.h"
#include "coop/net/end_reason.h"  // the named reason a join that reached the world load ends on
#include "ui/server_browser_surface.h"  // WHICH browser this session uses
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
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


void Post(GT::Task t) { GT::Post(std::move(t)); }

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
            ue_wrap::FVector p{};
            const bool pRead = ue_wrap::engine::TryGetActorLocation(local, p);   // unread proves no gameplay either
            if (!pRead || std::abs(p.X) + std::abs(p.Y) + std::abs(p.Z) < 100.f) {
                if (diag) UE_LOGI("play[wait %d]: mainPlayer_C @ORIGIN (%.0f,%.0f,%.0f)%s -- waiting for real gameplay",
                                  i, p.X, p.Y, p.Z, pRead ? "" : " (unread)");
                state->store(1);  // still the origin menu player; wait for gameplay
                return;
            }
            UE_LOGI("play: mainPlayer_C ready @ (%.0f,%.0f,%.0f) -- spawning puppet", p.X, p.Y, p.Z);
            state->store(coop::puppet_drive::Puppet(1).Spawn() ? 2 : 3);
        });
        // Shutdown-aware like its two siblings: a posted task the game thread never drains holds
        // this thread for as long as that lasts.
        while (state->load() == 0 && !coop::shutdown::IsShuttingDown()) ::Sleep(5);
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

// Bring up a session on g_session: reset the per-session edge state, wire every subsystem,
// (host) back the save up and install the LanDirect ban filter, then Start. The one path for
// "start a coop session", from the env boot and from a browser action; on the TimelineThread,
// since Start spawns the net thread and the save backup is a blocking copy. Returns Start()'s
// success, which the browser-join path uses to Fail the join when no connect edge will arrive.
bool StartCoopSession(const coop::net::Config& netCfg, coop::net::Refusal* why) {
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
    // And the container arbitration's own arithmetic: the base that is refused, the host change in
    // flight, and the sequence a third peer walks into -- which no two-peer run can reach, because
    // the peer that is judged against a stale baseline is the one that learned the world from a
    // relay rather than from its own join seed.
    coop::props::container_write_policy::RunSelftest();
    // And the pen beside it, whose cap has never fired in a run: every park a measured join
    // produced was host-authored, and those are deliberately not capped.
    coop::props::container_park::RunSelftest();
    // Reset net_pump's edge detectors, so a Stop/Start on one process carries no stale "was
    // connected" or "was holding" entries into the new session.
    coop::net_pump::OnSessionStart();
    coop::prop_lifecycle::SetSession(&g_session);
    coop::npc_sync::SetSession(&g_session);
    coop::prop_snapshot::SetSession(&g_session);
    coop::join_beacon::SetSession(&g_session);
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
    const bool ok = g_session.Start(netCfg, why);
    UE_LOGI("harness: ==== COOP SESSION START (%s / %s)%s ====",
            netCfg.role == coop::net::Role::Host ? "host" : "client",
            netCfg.topology == coop::net::Topology::P2P ? "p2p" : "lan-direct",
            ok ? "" : " -- START FAILED");
    // Every host start is made in a loaded world (the boot load or the picker's load comes first).
    if (ok && netCfg.role == coop::net::Role::Host) coop::rig_ready::Say("hosting");
    return ok;
}

// The main loop on the TimelineThread. Each tick: with no session running, poll session_manager
// for a browser-initiated start and boot it here (Start and the save backup must not run on the
// game thread); post the per-tick pump; ~2 s stats while running. One loop for the env "play"
// path and the native menu path.
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

// Is a gameplay world up RIGHT NOW? Asked of the module that owns world identity rather than of a
// boot parameter, because the two are not the same question and a launch that reaches gameplay
// later answers them differently. Free to ask: the memo behind it refreshes at 10 Hz on the game
// thread and every other call is an atomic load. Unknown is not Gameplay on purpose -- a gate that
// STARTS something wants a positive answer, and the kind is Unknown across every travel.
//
// DEGRADED fails OPEN, the direction world_identity's header prescribes: a recook that renames one
// of the three properties the chain needs leaves the kind Unknown for the life of the process, and
// reading that as "no world" would take the observers below off every path again -- this split's
// own defect, reintroduced by its instrument. Nothing here fears a false positive: the branch is
// reached only with no session, and everything it starts re-asks for itself.
bool InGameplayWorld() {
    if (ue_wrap::world_identity::Degraded()) return true;
    return ue_wrap::world_identity::CurrentWorldKind() ==
           ue_wrap::world_identity::WorldKind::Gameplay;
}

// A menu-mode join whose session refused to start leaves the player where its sentence tells them
// to act: the notice for the dialog, the cover dropped (which drains the abort as well), the save
// transfer armed for this join disarmed, since no session will ever disconnect to do it, and the
// browser back. The shape is world_boot's failed-world exit, less the session stop.
void FailRefusedMenuJoin_(const coop::net::Refusal& why) {
    coop::join_progress::Fail(why.code, why.detail);
    coop::join_progress::Reset();
    coop::save_transfer::OnDisconnect();
    // Opening the browser starts its HTTP workers, which a teardown must not.
    if (!coop::shutdown::IsShuttingDown()) ui::server_browser_surface::Open();
    ue_wrap::log::Flush();
}

}  // namespace

void InstallLobbyPlayerCountSource() {
    coop::session_manager::SetPlayerCountSource(&LobbyPlayerCount);
}

void RunPlayLoop(bool bootedIntoGameplay) {
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
            harness::world_boot::DriveHostBootIfPending();
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
                        coop::net::Refusal why{coop::net::EndReason::CouldNotStart, {}};
                        // The menu-mode client join: connect at the menu, download the host's save,
                        // load that world, then net_pump announces world-ready and the host
                        // replays; the player never sees a divergent fresh world. ONE fallback
                        // survives, a host that genuinely has no save, which is an answer and not
                        // a timeout; a failed transfer or a world that will not load ends the join
                        // by name. Blocks the TimelineThread, the abort drained inside.
                        //
                        // BOTH halves below are required. The direct arm exists for a process that
                        // auto-loaded its OWN world at boot -- the LAN rigs, where both peers were
                        // given the same save -- AND that is still standing in it. The boot fact
                        // alone is this split's own defect one consumer later: a rig that quit to
                        // the menu, or whose host session ended and fled there, still answers yes
                        // and would connect with no world to connect in. A player who reaches a
                        // solo world through the game's own menu answers no to the boot half and
                        // downloads the host's, which is right -- their world is not the join's.
                        if (pending.role == coop::net::Role::Client &&
                            !(bootedIntoGameplay && InGameplayWorld())) {
                            UE_LOGI("harness: menu-mode client join -- save-transfer bootstrap");
                            coop::save_transfer::ClientArm();
                            // A synchronous Start failure means no connect edge will ever clear the
                            // cover, so the refusal is settled here.
                            if (!StartCoopSession(pending, &why))
                                FailRefusedMenuJoin_(why);
                            else
                                harness::world_boot::DriveMenuModeJoinWorldBoot();
                        } else if (!StartCoopSession(pending, &why)) {
                            // A client standing in its own world (the rigs) gets the notice,
                            // and the abort drain clears the cover: never a browser over
                            // gameplay. A host start here is only logged, by Start.
                            coop::join_progress::Fail(why.code, why.detail);
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
        harness::pump::PostComposite([running, bootedIntoGameplay] {
            // One live answer per tick, on the game thread, which is where the world memo
            // refreshes; every consumer below that names a STATE reads this rather than the boot
            // parameter.
            const bool inGameplayWorld = InGameplayWorld();
            if (running) {
                coop::net_pump::Tick(g_session);   // drains the object index first
            } else {
                // No session: the object index still follows the engine, so a later session starts
                // from a current one rather than a backlog.
                ue_wrap::object_index::Drain();
                if (bootedIntoGameplay) {
                    // A run that auto-loaded its own world and may yet be joined: keep the coop
                    // observers armed so a session starting on this world does not begin behind.
                    // The boot fact, deliberately -- a player in a single-player world has no
                    // session coming until they click Multiplayer, and the fan-out installs with
                    // it, so arming it under them would change a game nobody asked us to change.
                    coop::subsystems::Install(g_session);
                }
                if (inGameplayWorld) {
                    // The quick-slot bar's icon edge is not a session's business -- the game
                    // publishes its icon tables after its own post-load refresh whether or not
                    // anyone is connected, so a solo world loses the race exactly as a hosted one
                    // does. It rides the session tick when there is a session and this one when
                    // there is not; it latches per world either way. A live world gate, not the
                    // boot one: the ordinary player starts at the menu and loads from there, and
                    // that is the launch the bar is broken in.
                    coop::hotbar_icon_edge::Tick();
                }
                // The readout, OUTSIDE the world gate: it catches a dispatch that happens during
                // the load, so its watch has to be armed while the menu is still up. A no-op
                // unless its own row is set. The load-reroll watch has the same reason: a world
                // loaded from the menu before any session is seen from this tick only.
                coop::dev::hotbar_icon_probe::Tick();
                coop::dev::load_reroll_watch::Tick(g_session);
                // A client left at the menu by its host joins again when the rig says the host is back.
                coop::dev::rehost_rejoin::Tick(g_session);
            }
            // The roster shows a board in a world and none at the menu, which is a live question:
            // out of session it synthesises the local row, and at the menu there is nobody to show.
            if (running || inGameplayWorld) {
                coop::roster::Refresh();
            }
            // Always, self-clearing: re-project the nameplates (an empty snapshot with no puppets,
            // so the HUD auto-hides at the menu), age the chat feed, tick the dev overlays; all
            // cheap no-ops when idle.
            coop::nameplate::Update();
            coop::dev::object_overlay::Update(); coop::dev::ragdoll_bone_overlay::Update();
            coop::chat_feed::Tick();
            // Always: the close subclass and the window title must work at the menu too.
            harness::pump::TickShutdownHooks();
        });
        harness::pump::TickWatchdogs();
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
                    ue_wrap::FVector loc{};
                    const bool locRead = ue_wrap::engine::TryGetActorLocation(lp, loc);
                    ue_wrap::FRotator rot{};
                    const bool rotRead = ue_wrap::engine::TryGetActorRotation(lp, rot);
                    ue_wrap::FRotator cRot{};
                    if (void* c = ue_wrap::engine::GetController(lp)) cRot = ue_wrap::engine::GetControlRotation(c);
                    UE_LOGI("pos diag: local actor=(%.0f,%.0f,%.0f)%s actorYaw=%.1f%s ctrl(P=%.1f Y=%.1f)",
                            loc.X, loc.Y, loc.Z, locRead ? "" : " (unread)", rot.Yaw, rotRead ? "" : " (unread)",
                            cRot.Pitch, cRot.Yaw);
                }
                if (coop::puppet_drive::Puppet(1).valid()) {
                    ue_wrap::FVector p{};
                    const bool pRead = coop::puppet_drive::Puppet(1).TryGetLocation(p);
                    UE_LOGI("pos diag: puppet world=(%.0f,%.0f,%.0f)%s", p.X, p.Y, p.Z, pRead ? "" : " (unread)");
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
