// harness/harness.cpp -- the process boot (Start) and the scenario timeline: the boot-time config
// seeds, the identity load, the dev and test probes, and the per-scenario branch that either
// boots a story save into a session (play), stays at the native menu (menu, the shipped launch)
// or runs an autonomous derisk. The session lifecycle lives in harness/session_runtime.cpp.

#include "harness/harness.h"

#include "harness/session_runtime.h"

#include "harness/autotest.h"
#include "harness/autotest_dispatch.h"
#include "coop/config/config.h"
#include "harness/harness_diag.h"
#include "harness/screenshot.h"
#include "harness/mod_environment.h"
#include "harness/sdk_check.h"
#include "coop/dev/freecam.h"
#include "coop/dev/object_overlay.h"
#include "coop/dev/ragdoll_bone_overlay.h"
#include "coop/save/save_transfer.h"
#include "coop/dev/spawn_menu_unlock.h"
#include "coop/dev/spawn_npc.h"
#include "coop/dev/gnatives_probe.h"
#include "coop/dev/kerfur_toggle.h"
#include "coop/session/teleport_client.h"
#include "coop/player/players_registry.h"
#include "coop/config/config.h"
#include "coop/config/config_review.h"      // RunBootSweep, the settings check
#include "coop/net/peer_identity.h"        // the durable Ed25519 identity
#include "coop/player/local_body.h"         // SetInitialSkin
#include "coop/text/utf8_codec.h"
#include "coop/session/session_manager.h"
#include "coop/player/nameplate.h"
#include "coop/player/nick_color.h"
#include "coop/player/roster.h"
#include "coop/net/session.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/session/shutdown.h"
#include "ui/dev_menu.h"
#include "ui/imgui_overlay.h"
#include "ui/console.h"
#include "ui/server_browser.h"
#include "ui/multiplayer_menu.h"
#include "coop/dev/menu_proceed.h"
#include "coop/dev/save_probe.h"
#include "coop/dev/native_ui_probe.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

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

namespace cfg = coop::config;

// The diagnostic dumps (Report, DumpComponents, DumpLiveWidgets, DumpParams) live in
// harness/harness_diag.cpp, in scope here unqualified.
using namespace harness::diag;

// Posts a task to the game thread.
void Post(GT::Task t) { GT::Post(std::move(t)); }

// The background timeline: sleeps for pacing, and every engine touch is posted to the game
// thread.
DWORD WINAPI TimelineThread(LPVOID param) {
    const std::string scenario = *static_cast<std::string*>(param);
    delete static_cast<std::string*>(param);

    UE_LOGI("harness: timeline start, scenario='%s'", scenario.c_str());

    // The master URL and the host fallback Config go into session_manager before any browser action
    // can fire; a native launch has no env, so this is where the ini's net.master takes effect.
    coop::session_manager::Configure(cfg::ReadMasterUrl(), cfg::ReadP2PHostFallback());
    // A fresh install seeds the multivoid.ini skeleton before anything reads or writes the file
    // this launch: absent-only, atomic.
    cfg::EnsureIniSkeleton();
    // multivoid.ini.example regenerates beside the DLL, every key with its description, default and
    // env twin; deterministic, compare-first, fail-soft, and never read back.
    cfg::GenerateExampleCatalog();
    // Stored values a shipped bug wrote are retired (config.h): exact-match, one-shot, and before
    // anything reads the rows it touches.
    cfg::MigrateRetiredIniValues();
    // The local nickname from config (the env twin, the ini, the registry default), so the browser
    // shows the current name; the browser value wins at session start.
    {
        // session_manager holds UTF-8 (the browser's InputText writes it), so the seed encodes
        // rather than narrows: a narrowing loop here once turned a Cyrillic name into question
        // marks before the sanitiser or the wire saw it, and every downstream fix looked as if it
        // had not worked.
        coop::session_manager::SetNickname(coop::text::ToUtf8(cfg::ReadNickname()));
    }
    // The durable player identity: an Ed25519 keypair whose public key is the network identity and
    // whose 32-character guid (the key of the host-side inventory) is derived from it, replacing an
    // ini value the peer chose for itself and every host believed (coop/net/peer_identity.h).
    // Loaded at boot, so a failure is visible before any session.
    if (!coop::net::peer_identity::Load()) {
        UE_LOGE("harness: no durable identity could be established -- coop sessions "
                "will refuse to start (see the peer_identity log line above)");
    }
    // The persisted body skin; local_body owns it and the Join reads it from there.
    coop::local_body::SetInitialSkin(cfg::ReadPlayerSkin());
    // The persisted nameplate pref (absent = visible), read by the Join's prefs byte.
    coop::nameplate::SetInitialLocalVisible(cfg::ResolveFlag(coop::config_registry::rows::nameplate));
    // The persisted nick colour (ini nick_color=RRGGBB); nick_color owns the parse.
    coop::nick_color::SetInitialLocalFromIniHex(cfg::ResolveString(coop::config_registry::rows::nick_color));
    // The boot file-versus-schema sweep, after the mints above so the post-mint file is what is
    // reviewed; it arms the settings-check panel at the main menu and never rewrites.
    coop::config_review::RunBootSweep();

    // The census of the other mods in this process, logged and never raised to the player:
    // file-system reads, posted so a slow disk cannot stall the boot thread. Above the scenario
    // branch, since every scenario reaches this line; inside the autotest branch it never ran for a
    // real player, and its own verification could not see that.
    Post([] { harness::mod_environment::Run(); });

    // The lobby heartbeat's player-count source, here for the same reason: the scenarios that
    // announce a lobby do not agree on where, and an install in any one of them is missed by the
    // others. Ordered before every announce site, so the heartbeat worker never reads the pointer
    // before it is written.
    session_runtime::InstallLobbyPlayerCountSource();

    const bool storyBoot = (scenario == "play");
    const bool menuMode  = (scenario == "menu");
    if (storyBoot) {
        // Widgets sampled across the first seconds, catching the OMEGA warning gate before gameplay
        // opens: an autotest diagnostic that logs thousands of lines and saturates the task pump,
        // so never on the native menu path (it delayed the menu by 20 s once).
        for (int k = 0; k < 7; ++k) {  // about 2.8 s of coverage
            Post([k] { UE_LOGI("widgets: == intro dump pass %d ==", k); DumpLiveWidgets(); });
            ::Sleep(400);
        }
    } else if (menuMode) {
        ::Sleep(1500);  // the native menu boot: a brief settle, then RunPlayLoop
    } else {
        ::Sleep(8000);  // the other scenarios: let the engine init
    }
    Post([] { Report("menu"); });
    // The param-offset validator (scenario paramdump): a UFunction's property layout, to check
    // against the known signature on a new function or game build.
    if (scenario == "paramdump") {
        Post([] {
            DumpParams(L"Actor", L"K2_SetActorLocation");
            DumpParams(L"GameplayStatics", L"BeginDeferredActorSpawnFromClass");
            DumpParams(L"GameplayStatics", L"FinishSpawningActor");
        });
    }

    const bool wantGameplay = (scenario == "newgame" || scenario == "orphan" ||
                               scenario == "skin" || scenario == "show" ||
                               scenario == "play");
    // The autonomous scenarios boot to the menu, then `open` gameplay and wait; the story-boot
    // scenario loads through LoadStorySave in its own branch.
    if (wantGameplay && !storyBoot) {
        ::Sleep(4000);
        Post([] {
            UE_LOGI("harness: skip-to-gameplay (open %ls)", P::name::GameplayLevel);
            std::wstring cmd = L"open ";
            cmd += P::name::GameplayLevel;
            ue_wrap::engine::ExecuteConsoleCommand(cmd.c_str());
        });
        ::Sleep(25000);  // the level load and BeginPlay
        Post([] { Report("post-load"); });
    }
    // No in-game HighResShot: its toast distracts a hands-on tester; autonomous captures use the
    // external window capture.

    if (scenario == "orphan") {
        // The orphan derisk: spawn a second mainPlayer_C through our own call path, confirm the
        // count goes from one to two, drive it by absolute teleport, then soak.
        ::Sleep(2000);
        Post([] { Report("pre-spawn"); });
        Post([] {
            UE_LOGI("harness: === spawn coop::RemotePlayer (2nd mainPlayer_C) ===");
            coop::puppet_drive::Puppet(1).Spawn();
        });
        ::Sleep(2000);
        Post([] { Report("post-spawn"); });
        Post([] {
            if (coop::puppet_drive::Puppet(1).valid()) {
                ue_wrap::FVector p = coop::puppet_drive::Puppet(1).GetLocation();
                UE_LOGI("harness: orphan post-spawn pos=(%.0f,%.0f,%.0f)", p.X, p.Y, p.Z);
            }
        });

        // The pose drive: the orphan teleported in +X steps, read back each time.
        for (int i = 1; i <= 5; ++i) {
            ::Sleep(3000);
            Post([i] {
                if (!coop::puppet_drive::Puppet(1).valid()) { UE_LOGW("harness: drive %d -- no orphan", i); return; }
                ue_wrap::FVector p = coop::puppet_drive::Puppet(1).GetLocation();
                p.X += 150.f;
                const bool ok = coop::puppet_drive::Puppet(1).SetLocation(p);
                ue_wrap::FVector got = coop::puppet_drive::Puppet(1).GetLocation();
                UE_LOGI("harness: drive step %d set X=%.0f ok=%d -> read (%.0f,%.0f,%.0f)",
                        i, p.X, ok, got.X, got.Y, got.Z);
            });
        }

        ::Sleep(5000);
        Post([] { Report("post-drive soak"); });
        UE_LOGI("harness: ==== AUTONOMOUS ORPHAN TIMELINE DONE ====");
    } else if (scenario == "play") {
        // The hands-on test, in story mode. The net role first: an env client no longer boots its
        // own world but takes the same save-transfer join as the browser (connect at the menu,
        // download the host's save, load that world); the host and a solo run auto-load their story
        // save here.
        bool netEnabled = false;
        const coop::net::Config netCfg = cfg::ReadNetConfig(netEnabled);
        const bool saveTransferClient =
            netEnabled && netCfg.role == coop::net::Role::Client;
        if (!saveTransferClient) session_runtime::BootStorySaveBlocking();
        // The SDK profile is checked against the running build (after the world boot on a host; on
        // a save-transfer client the classes load with the menu world, and every consumer
        // self-retries).
        Post([] { harness::sdk_check::Run(); });
        // With a configured net role the puppet is network-driven and our pose is sent; otherwise
        // the puppet is a static local one.
        if (netEnabled) {
            // The autotest positioning, shared by both arms: a host runs it before Start (the first
            // pose packet already carries the pose); a save-transfer client would run it after its
            // world exists.
            auto runAutotestTeleport = [&] {
            // With VOTVCOOP_AUTOTEST_X/Y/Z (and YAW, PITCH) set, the local pawn is placed and aimed
            // at the role-specific pose so each test instance's screenshot sees the other's puppet;
            // once, post-load, before the session starts.
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
                // Through teleport_client::ApplyLocally, VOTV's own teleportWObackrooms, which the
                // movement component's constraints do not undo the way they undo K2_TeleportTo. The
                // retry loop covers the first ticks before the local player exists; once it does,
                // the teleport sticks.
                const ue_wrap::FVector target{ax, ay, az};
                bool teleported = false;
                for (int attempt = 0; attempt < 50 && !teleported; ++attempt) {
                    auto okFlag = std::make_shared<std::atomic<int>>(0);  // 0=pending,1=ok,2=nope
                    Post([ax, ay, az, ayaw, apitch, target, okFlag] {
                        void* local = coop::players::Registry::Get().Local();
                        if (!local) { okFlag->store(2); return; }
                        coop::teleport_client::ApplyLocally({ax, ay, az, apitch, ayaw, 0.f});
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
            };  // runAutotestTeleport
            // A client never teleports to a fixed checkpoint: it appears at the host's position,
            // which the world-ready connect replay sends (net_pump,
            // teleport_client::TeleportSlotToHost).
            if (!saveTransferClient) {
                runAutotestTeleport();
                session_runtime::StartCoopSession(netCfg);
                if (netCfg.role == coop::net::Role::Host) {
                    // The env host is a real master-announced game, hidden from the public list
                    // (the heartbeat live, joiners direct-connect by address, the test lobby never
                    // in the browser); best effort.
                    std::string w = cfg::ResolveString(coop::config_registry::rows::save);
                    coop::session_manager::AnnounceEnvHostHidden(
                        coop::session_manager::Nickname() + "'s game", w);
                }
            } else {
                // The env client goes through the same session_manager door as the browser: the
                // connect raises the join cover and queues the start, and RunPlayLoop (entered with
                // idleInGameplay false) drains it through the one menu-mode join branch. It dials
                // the way its topology says: unconditionally direct, a P2P client silently
                // connected over IP, and the P2P smoke proved only the host half.
                if (netCfg.topology == coop::net::Topology::P2P) {
                    if (!coop::session_manager::ConnectP2PDirect(netCfg.hostIdentity,
                                                                 netCfg)) {
                        UE_LOGW("harness: env P2P connect to '%s' rejected",
                                netCfg.hostIdentity.c_str());
                    }
                } else {
                    char hostPort[64];
                    std::snprintf(hostPort, sizeof(hostPort), "%s:%u",
                                  netCfg.peerIp.empty() ? "127.0.0.1" : netCfg.peerIp.c_str(),
                                  static_cast<unsigned>(netCfg.port));
                    if (!coop::session_manager::ConnectDirect(hostPort)) {
                        UE_LOGW("harness: env ConnectDirect('%s') rejected", hostPort);
                    }
                }
            }

        } else if (coop::config::ResolveFlag(::coop::config_registry::rows::static_2nd_player)) {
            // An opt-in dev aid ([dev] static_2nd_player=1): a static slot-1 puppet for solo visual
            // tests. Off by default: it would collide with a browser-hosted session's slot-1
            // network puppet (pose-driven but never in the roster), and a solo game should show no
            // phantom player.
            session_runtime::SpawnSecondPlayerWhenReady();
        }

        // The autotest dispatch: each VOTVCOOP_RUN_*_TEST worker whose env flag is set (each
        // self-gates on role). Outside the net-role branch: inside it, an env-gated test could run
        // only in a launch with a session, and the routines documented as solo (the menu-travel
        // probe, the ragdoll spawn probe, the death instrument that must run sessionless) were
        // silently unreachable, reporting INCONCLUSIVE with no line saying why.
        harness::autotest::SpawnEnvGatedTests(netCfg.role);

        UE_LOGI("harness: ==== PLAY READY ====");
        ue_wrap::log::Flush();  // the boot sequence lands on disk
        // The one play loop, env- or browser-driven. idleInGameplay: a host or solo run booted
        // straight into gameplay; a save-transfer client is at the menu, and its queued connect
        // must hit the menu-mode branch.
        session_runtime::RunPlayLoop(/*idleInGameplay=*/!saveTransferClient);
    } else if (scenario == "show") {
        // The autonomous visual confirm: spawn the puppet in front, hold idle, then drive a walk
        // speed to confirm the AnimBP animates from the variable writes. It does not exercise the
        // receiver's interpolation (each pose here snaps or has no positional delta); the
        // two-process LAN test does.
        ::Sleep(2000);
        Post([] {
            UE_LOGI("show: === spawn skin-puppet ===");
            coop::puppet_drive::Puppet(1).Spawn();
        });
        ::Sleep(3000);
        Post([] {
            if (!coop::puppet_drive::Puppet(1).valid()) { UE_LOGW("show: no puppet"); return; }
            const ue_wrap::FVector at = coop::puppet_drive::Puppet(1).GetLocation();
            UE_LOGI("show: drive WALK in place (speed=200) to test AnimBP locomotion");
            // The same location and yaw with the speed bumped: the first SetTargetPose since spawn
            // snaps, then Tick applies.
            coop::net::PoseSnapshot s{at.X, at.Y, at.Z, /*yaw*/0.f, /*pitch*/0.f, /*speed*/200.f};
            coop::puppet_drive::Puppet(1).SetTargetPose(s);
            coop::puppet_drive::Puppet(1).Tick();
        });
        ::Sleep(4000);
        Post([] {
            if (!coop::puppet_drive::Puppet(1).valid()) return;
            const ue_wrap::FVector at = coop::puppet_drive::Puppet(1).GetLocation();
            coop::net::PoseSnapshot s{at.X, at.Y, at.Z, /*yaw*/0.f, /*pitch*/0.f, /*speed*/0.f};
            coop::puppet_drive::Puppet(1).SetTargetPose(s);
            coop::puppet_drive::Puppet(1).Tick();
            UE_LOGI("show: back to idle (speed=0)");
        });
        UE_LOGI("harness: ==== SHOW DONE ====");
    } else if (scenario == "skin") {
        // The visible-body inspection: the components of the local pawn and of a spawned orphan,
        // and the SuperStruct offset probe.
        ::Sleep(2000);
        Post([] {
            R::DebugProbeSuperStructOffset();
            void* local = coop::players::Registry::Get().Local();
            DumpComponents("local mainPlayer_C", local);
            coop::puppet_drive::Puppet(1).Spawn();
        });
        ::Sleep(2000);
        Post([] { DumpComponents("orphan mainPlayer_C", coop::puppet_drive::Puppet(1).actor()); });
        UE_LOGI("harness: ==== SKIN INSPECT DONE ====");
    } else if (scenario == "newgame") {
        ::Sleep(5000);
        Post([] { Report("post-shot"); });
        UE_LOGI("harness: ==== AUTONOMOUS NEWGAME TIMELINE DONE ====");
    } else if (scenario == "menu") {
        // The native launch (no test env, so the scenario defaults to menu): VOTV's own main menu,
        // where the MULTIPLAYER button drives coop, and no auto-load into gameplay (a test-only
        // behaviour). RunPlayLoop drains browser-initiated sessions and keeps the shutdown hooks
        // live at the menu; the gameplay observers install when a session starts.
        UE_LOGI("harness: ==== MENU mode (native launch) -- MULTIPLAYER button drives coop ====");
        ue_wrap::log::Flush();  // the boot sequence lands on disk
        session_runtime::RunPlayLoop(/*idleInGameplay=*/false);
    } else {
        UE_LOGI("harness: scenario '%s' -- no automatic actions", scenario.c_str());
    }
    return 0;
}

}  // namespace

void Start() {
    // F12 takes a toast-free screenshot into coop-screenshots/; always on, for hands-on testing.
    screenshot::StartHotkeyWatcher();

    // The dev free camera: HOME toggles it under [dev] freecam=1, and the F1 menu toggles it under
    // [dev] devkeys; a no-op at boot otherwise.
    coop::dev::freecam::Init();

    // The sandbox prop-spawn menu (Q) in story mode, under [dev] spawn_menu_unlock=1 or the F1
    // menu; host and local only.
    coop::dev::spawn_menu_unlock::Init();

    // The other dev features (snow, restore vitals, teleport clients, the position and camera
    // overlay, spawn NPC) are driven from the F1 menu; their SetSession calls ran above.

    // The ImGui overlay, the F1 menu's host: dev_menu::Init reads the dev switch off the render
    // thread, and the overlay installs the DXGI present hook. Visible to all players; the dev
    // categories gate on [dev] devkeys inside the menu.
    ui::dev_menu::Init();
    // The object-overlay labels: menu-toggled normally, force-enabled at boot by [dev]
    // object_overlay=1 so the smoke exercises the draw path.
    coop::dev::object_overlay::InitFromIni();
    coop::dev::ragdoll_bone_overlay::InitFromIni();
    // The save transfer's sinks on the session, and a sweep of stale crash-leftover zcoop_ slots
    // (age-gated, never a live sibling's).
    coop::save_transfer::Install(&session_runtime::Session());
    coop::save_transfer::CleanupStaleSlotsAtBoot();
    // The player-list scoreboard (a second overlay surface, on tilde); the roster reads this
    // session.
    coop::roster::SetSession(&session_runtime::Session());
    if (!ui::imgui_overlay::Init()) {
        UE_LOGW("harness: imgui_overlay::Init failed -- F1 menu unavailable this run");
    }
    // The in-game console's logger sink, registered now so it captures the mod log from here on;
    // it auto-shows during a client join.
    ui::console::Init();

    // The MULTIPLAYER entry point: the native button injected above NEW GAME in VOTV's main menu,
    // which opens the server browser. ui_menu_C resolves lazily with a bounded retry. On by
    // default; [coop] multiplayer_menu_off=1 disables it.
    coop::multiplayer_menu::Init();

    // Test only (VOTVCOOP_MENU_PROCEED=1): auto-advance past the content-warning screen so an
    // autonomous run reaches the main menu. Never on by default.
    coop::dev::menu_proceed::Init();

    // The VOTVCOOP_SPAWN_TRIGGER file watcher, the autonomous NPC-spawn path (host install and
    // broadcast, client mirror); hands-on spawning is the F1 menu. A no-op without the env.
    coop::dev::spawn_npc::Init();

    // Test only (VOTVCOOP_KERFUR_TOGGLE_TRIGGER): a programmatic kerfur turn-off and turn-on, so
    // the client's conversion-adopt path has autonomous coverage (the radial verb is a local
    // virtual call and needs a player at the menu).
    coop::dev::kerfur_toggle::Init();

    // The GNatives probe (ini gnatives_probe=1): swaps two opcode handlers with the substrate's
    // wrapper shape and counts the local-dispatch rate and cost; installed at boot so the boot and
    // solo windows are covered.
    coop::dev::gnatives_probe::Init();

    // Test only (VOTVCOOP_TEST_SAVE_ENUM=1): the native save browser (VOTV's loadSlots) verified at
    // the menu.
    coop::dev::save_probe::Init();

    // The native-UI probe (ini native_ui_probe=1): the read-only UMG resolve census, the donor
    // residency, the menu switcher's child map, and the count of frames presented while no world
    // exists; native_ui_probe_write=1 adds the one write. It rides the menu tick observer, since at
    // boot there is no menu and a null donor is indistinguishable from an absence.
    coop::dev::native_ui_probe::Init();

    // The WM_CLOSE subclass on the game window, so an X-close runs our cleanup before the engine's
    // teardown dispatches: with the detour live through shutdown it faults on half-destroyed
    // objects and the process hangs. The window may not exist yet; Install retries from the
    // timeline.
    coop::shutdown::Install(&session_runtime::Session());

    auto* scenario = new std::string(cfg::ReadScenario());
    if (HANDLE t = ::CreateThread(nullptr, 0, TimelineThread, scenario, 0, nullptr)) {
        ::CloseHandle(t);
    } else {
        delete scenario;
        UE_LOGE("harness: failed to start timeline thread");
    }
}

}  // namespace harness
