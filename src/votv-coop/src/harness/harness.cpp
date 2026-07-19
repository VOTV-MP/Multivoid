#include "harness/harness.h"

#include "harness/session_runtime.h"

#include "harness/autotest.h"
#include "harness/autotest_dispatch.h"
#include "coop/config/config.h"
#include "harness/harness_diag.h"
#include "harness/screenshot.h"
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
#include "coop/session/player_handshake.h"  // SetLocalGuid (v73 per-player inventory identity)
#include "coop/player/local_body.h"         // SetInitialSkin (v93 skins: ini player_skin=)
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

// Diagnostic dumps (Report / DumpComponents / DumpLiveWidgets / DumpParams) live in
// harness/harness_diag.cpp; bring them into scope so the scenario call sites stay unqualified.
using namespace harness::diag;

// ReadLocalPose extracted to coop/net_pump.cpp (PR-4.13).

// The session-lifecycle driver (g_session + StartCoopSession + world boot +
// RunPlayLoop + the pump composite) lives in harness/session_runtime.cpp
// (s27 cut); this file keeps the process boot (Start) + the scenario timeline.

// GT-post 1-liner (a second anon-ns copy lives in session_runtime.cpp).
void Post(GT::Task t) { GT::Post(std::move(t)); }

// Diagnostic dumps (DumpParams / Report / DumpComponents / DumpLiveWidgets, + their
// ContainsCI / DumpClassFunctions helpers) extracted to harness/harness_diag.cpp
// (2026-06-06 modular file-size audit). They are reached unqualified here via
// `using namespace harness::diag` below.
// Autonomous grab test moved to harness/autotest/autotest_grab.cpp.

// Background timeline. Sleeps for pacing; every engine touch is posted to the
// game thread. Mirrors the Lua harness's newgame timeline.
DWORD WINAPI TimelineThread(LPVOID param) {
    const std::string scenario = *static_cast<std::string*>(param);
    delete static_cast<std::string*>(param);

    UE_LOGI("harness: timeline start, scenario='%s'", scenario.c_str());

    // Push the master URL + the host fallback Config into session_manager BEFORE any
    // browser action can fire (the menu server browser / Host-Game picker read them).
    // A native launch has no env, so this is where the deployed ini's net.master (->
    // the VPS) takes effect. Cheap env/ini reads; harmless for every scenario.
    coop::session_manager::Configure(cfg::ReadMasterUrl(), cfg::ReadP2PHostFallback());
    // Seed the local nickname from config (env VOTVCOOP_NET_NICK / ini net.nick / "Player")
    // so the server browser shows the current name; the user can overwrite it there, and the
    // browser value wins at StartCoopSession. ASCII narrow (explicit, non-ASCII -> '?').
    {
        const std::wstring wn = cfg::ReadNickname();
        std::string nn;
        nn.reserve(wn.size());
        for (wchar_t c : wn) nn.push_back(c < 128 ? static_cast<char>(c) : '?');
        coop::session_manager::SetNickname(nn);
    }
    // v73 per-player inventory: seed the durable identity GUID (ini player_guid=, generated
    // + persisted on first launch). Rides our Join so the host keys our inventory file.
    coop::player_handshake::SetLocalGuid(cfg::ReadPlayerGuid());
    // v93 skins: the persisted body-skin choice (same ini; a fresh identity is assigned
    // the current scientist). local_body owns it; the Join payload reads it from there.
    coop::local_body::SetInitialSkin(cfg::ReadPlayerSkin());
    // v94: the persisted nameplate pref (absent = visible). The Join prefs byte reads it.
    coop::nameplate::SetInitialLocalVisible(cfg::ReadIniValue("nameplate", "1") != "0");
    // v103 (12f): the persisted nick color (ini nick_color=RRGGBB hex). The Join
    // color field reads it; nick_color owns the parse + the absent/empty
    // semantics (s27 Tier-C move).
    coop::nick_color::SetInitialLocalFromIniHex(cfg::ReadIniValue("nick_color", "unset"));

    // The OMEGA WARNING is on screen during the FIRST few seconds (the intro/menu
    // world), BEFORE we `open` gameplay. Sample widgets across that window so the
    // dump catches the omega gate (a later single dump only sees gameplay widgets,
    // since VOTV preloads its UMG and the omega widget is gone by then). Each Post
    // runs on the game thread as soon as the pump is live (which is while the omega
    // screen ticks UMG), so these land during the intro.
    const bool storyBoot = (scenario == "play");
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
                               scenario == "play");
    // Autonomous scenarios boot to the menu then `open` + wait a fixed time. The
    // story-boot scenarios (play) load via LoadStorySave in their own
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

        // Pose-drive: teleport the orphan in +X steps, read back each time.
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
        // Hands-on test. Coop targets STORY mode. Read the net role FIRST: a
        // v56 CLIENT (.bat/env) no longer pre-boots its own world -- it takes
        // the SAME save-transfer join as the browser (user 2026-06-10: "make
        // the existing .bat files go the server-browser way automagically into
        // a host's game"): connect at the menu, download the host's save, load
        // THAT world. Host/solo still auto-load their story save here via
        // VOTV's own load path (LoadStorySave -> open untitled_1).
        bool netEnabled = false;
        const coop::net::Config netCfg = cfg::ReadNetConfig(netEnabled);
        const bool saveTransferClient =
            netEnabled && netCfg.role == coop::net::Role::Client;
        if (!saveTransferClient) session_runtime::BootStorySaveBlocking();
        // Verify the SDK profile resolves against the running VOTV build.
        // (After the world boot on host/solo; on a save-transfer client the BP
        // classes load with the menu/preLoad world -- the checker logs what it
        // can and the per-class consumers all self-retry anyway.)
        Post([] { harness::sdk_check::Run(); });
        // Coop networking: if multivoid.ini configures net.role, the puppet is
        // network-driven (auto-spawned on the first peer pose) and we send our pose;
        // otherwise the puppet is spawned locally + static (the pre-net behaviour).
        if (netEnabled) {
            // Autotest positioning, shared by both arms below: host/solo run it
            // BEFORE Start (the first pose packet already carries the pose); a
            // save-transfer client runs it AFTER its world exists.
            auto runAutotestTeleport = [&] {
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
                // coop::teleport_client::ApplyLocally. That path bypasses the
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
            // NOTE: a CLIENT never teleports to a fixed КПП checkpoint. A joining
            // client appears at the HOST's position: the world-ready connect replay
            // sends the host's own player pose (net_pump RunConnectReplayForSlot ->
            // teleport_client::TeleportSlotToHost) and the client applies it.
            if (!saveTransferClient) {
                runAutotestTeleport();
                session_runtime::StartCoopSession(netCfg);
                if (netCfg.role == coop::net::Role::Host) {
                    // v56 (user 2026-06-10): the env host is a REAL master-announced
                    // game, HIDDEN from the public list (heartbeat live; joiners
                    // direct-connect by IP; the .bat/test lobby never pollutes the
                    // browser). Best-effort -- master down changes nothing.
                    std::string w = cfg::ReadEnv("VOTVCOOP_SAVE");
                    if (w.empty()) w = cfg::ReadIniValue("save", "s_may2026");
                    coop::session_manager::AnnounceEnvHostHidden(
                        coop::session_manager::Nickname() + "'s game", w);
                }
            } else {
                // v56 (user 2026-06-10 "make the .bat go the server-browser way"):
                // the env CLIENT goes through the SAME session_manager path as the
                // browser -- ConnectDirect raises the join cover + queues the
                // start; RunPlayLoop (entered with idleInGameplay=false below)
                // drains it through the ONE menu-mode join branch (arm -> connect
                // at the menu -> save download -> load -> world-ready). RULE 2:
                // no harness-side parallel join path. (The autotest-positioning
                // teleport doesn't run on this path -- the puppetshot/ragdollshot
                // scenarios need a post-join hook when next used.)
                char hostPort[64];
                std::snprintf(hostPort, sizeof(hostPort), "%s:%u",
                              netCfg.peerIp.empty() ? "127.0.0.1" : netCfg.peerIp.c_str(),
                              static_cast<unsigned>(netCfg.port));
                if (!coop::session_manager::ConnectDirect(hostPort)) {
                    UE_LOGW("harness: env ConnectDirect('%s') rejected", hostPort);
                }
            }

            // Autonomous autotest dispatch: spawn each VOTVCOOP_RUN_*_TEST worker
            // thread whose env flag is set (each self-gates on role internally).
            harness::autotest::SpawnEnvGatedTests(netCfg.role);
        } else if (coop::config::IsIniKeyTrue("static_2nd_player")) {
            // Opt-in dev aid ([dev] static_2nd_player=1): a static slot-1 puppet for solo
            // visual tests. OFF by default (audit P1) -- it would collide with a browser-
            // booted HOST session's slot-1 NETWORK puppet (net_pump's auto-spawn is gated
            // on !Puppet(1).valid(), so the static one would be pose-driven but never
            // registered in the roster), and a shipping solo game shouldn't show a phantom
            // player. The real 2nd player arrives via the MULTIPLAYER browser; RunPlayLoop
            // installs the solo observers regardless.
            session_runtime::SpawnSecondPlayerWhenReady();
        }
        UE_LOGI("harness: ==== PLAY READY ====");
        ue_wrap::log::Flush();  // boot-ready milestone: land the boot sequence on disk now
        // Unified play loop (env- or browser-driven). Replaces the two prior per-branch
        // loops + the stale Z-trace debug block (RULE 2: one loop, one start path).
        // idleInGameplay: host/solo booted straight into gameplay (BootStorySaveBlocking);
        // a v56 save-transfer client is AT THE MENU (its queued ConnectDirect must hit
        // the menu-mode branch in the TakePendingStart drain).
        session_runtime::RunPlayLoop(/*idleInGameplay=*/!saveTransferClient);
    } else if (scenario == "show") {
        // Autonomous visual confirm: spawn the puppet in front, hold idle, then
        // drive a walk (speed) for a few seconds to confirm the AnimBP animates
        // from our direct variable writes. NOTE: this scenario does NOT exercise
        // the receiver-side INTERPOLATION (each SetTargetPose here either snaps
        // -- first call -- or has zero positional delta -- subsequent walk/idle
        // at same loc). The interp linear LERP path is exercised by the LAN
        // test (the two-process mp.py pose stream).
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
            // Same loc/yaw, just bump speed -- the first SetTargetPose since spawn
            // snaps (hasPose_ false), then Tick applies. AnimBP locomotion picks it up.
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
        // Investigate the player's visible-body setup: enumerate components of
        // the local pawn and a spawned orphan, and confirm SuperStruct offset.
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
        session_runtime::RunPlayLoop(/*idleInGameplay=*/false);
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

    // Dev: use the sandbox prop-spawn menu (Q) in STORY mode. No-op at boot
    // unless [dev] spawn_menu_unlock=1; the F1 menu (Game > Entities) toggles it
    // under [dev] devkeys. Host/local only (coop::dev_gate).
    coop::dev::spawn_menu_unlock::Init();

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
    // Dev object-overlay labels: menu-toggled normally; [dev] object_overlay=1
    // force-enables at boot so the autonomous smoke exercises the draw path.
    coop::dev::object_overlay::InitFromIni();
    coop::dev::ragdoll_bone_overlay::InitFromIni();
    // v56 save-transfer: register the bulk sink with the session + sweep stale
    // crash-leftover zcoop_* slots (age-gated -- never a live sibling's).
    coop::save_transfer::Install(&session_runtime::Session());
    coop::save_transfer::CleanupStaleSlotsAtBoot();
    // Player-list scoreboard (a second overlay surface, shown to everyone, on tilde). The
    // roster snapshot reads this session; Refresh() runs in the game-thread ticks.
    coop::roster::SetSession(&session_runtime::Session());
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

    // TEST-ONLY (VOTVCOOP_KERFUR_TOGGLE_TRIGGER): programmatic kerfur turn_off/turn_on so the
    // CLIENT conversion-adopt path has autonomous coverage (the radial verb is EX_LocalVirtual-
    // Function -- unhookable + needs a player at the menu). No-op unless the trigger env is set.
    coop::dev::kerfur_toggle::Init();

    // GATE 2.2 of docs/COOP_VM_DISPATCH_PLAN.md (THROWAWAY, ini `gnatives_probe=1`):
    // swap GNatives[0x45]/[0x46] with the substrate's wrapper shape and count EX_Local*
    // dispatch rate + cost, to decide the <=0.1 ms/frame perf gate before building the
    // real substrate. Installs at boot so the boot/solo-SP windows are covered too.
    coop::dev::gnatives_probe::Init();

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
