#include "harness/harness.h"

#include "harness/autotest.h"
#include "harness/autotest_dispatch.h"
#include "harness/config.h"
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
#include "ue_wrap/call.h"
#include "ue_wrap/hud_feed.h"
#include "ue_wrap/engine.h"
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

// Runs on the game thread (posted): dump a UFunction's parameter frame so we can
// verify the FProperty offsets (names/offsets/sizes) against the known UE4.27
// signature before we rely on them for marshaling. Temporary validation aid.
void DumpParams(const wchar_t* className, const wchar_t* funcName) {
    void* cls = R::FindClass(className);
    void* fn = cls ? R::FindFunction(cls, funcName) : nullptr;
    if (!fn) {
        UE_LOGW("paramdump: %ls::%ls not found (cls=%p)", className, funcName, cls);
        return;
    }
    const int32_t frame = R::FunctionFrameSize(fn);
    auto params = R::FunctionParams(fn);
    UE_LOGI("paramdump: %ls::%ls  frameSize=%d  params=%zu", className, funcName,
            frame, params.size());
    for (const auto& p : params) {
        UE_LOGI("    %-28ls off=0x%02x size=%-3d flags=0x%llx%s%s", p.name.c_str(),
                p.offset, p.size, static_cast<unsigned long long>(p.flags),
                (p.flags & ue_wrap::profile::cpf::OutParm) ? " OUT" : "",
                (p.flags & ue_wrap::profile::cpf::ReturnParm) ? " RET" : "");
    }
}

// Runs on the game thread (posted): log enough to confirm where we are.
void Report(const char* label) {
    // NumObjects is O(1). Avoid CountObjectsByClass here (a full GUObjectArray
    // walk) -- Report runs in the play path too, and per the post-ship audit we
    // don't pay a 100k-object scan just for a log line.
    const int32_t n = R::NumObjects();
    void* world = R::FindObjectByClass(P::name::WorldClass);
    std::wstring worldName = world ? R::ToString(R::NameOf(world)) : L"(none)";
    UE_LOGI("harness report [%s]: NumObjects=%d, world=%ls", label, n, worldName.c_str());
}

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

// Runs on the game thread: log an actor's default subobjects (its components),
// so we can find the mesh component(s) that carry the player's visible body.
void DumpComponents(const char* label, void* actor) {
    if (!actor) { UE_LOGW("components [%s]: null actor", label); return; }
    auto kids = R::ChildObjectsOf(actor);
    UE_LOGI("components [%s] of %p: %zu", label, actor, kids.size());
    for (const auto& k : kids) {
        UE_LOGI("    %-34ls : %ls", k.className.c_str(), k.name.c_str());
    }
}

void Post(GT::Task t) { GT::Post(std::move(t)); }

// Case-insensitive substring test (ASCII keywords against a wide string).
static bool ContainsCI(const std::wstring& hay, const wchar_t* needle) {
    std::wstring h = hay, n = needle;
    auto lower = [](std::wstring& s) { for (auto& c : s) c = static_cast<wchar_t>(::towlower(c)); };
    lower(h); lower(n);
    return h.find(n) != std::wstring::npos;
}

// List the callable UFunctions a class (and its parents) expose, so we can see
// exactly which one to call (e.g. a Proceed/Skip on the OMEGA widget). Functions
// are UObjects of class "Function" whose Outer is one of the classes in the chain.
static void DumpClassFunctions(void* cls, const wchar_t* tag) {
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || R::ClassNameOf(obj) != L"Function") continue;
        void* outer = R::OuterOf(obj);
        void* c = cls;
        for (int d = 0; d < 16 && c; ++d) {
            if (outer == c) {
                UE_LOGI("  %ls fn: %ls", tag, R::ToString(R::NameOf(obj)).c_str());
                break;
            }
            c = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(c) + P::off::UStruct_SuperStruct);
        }
    }
}

// Diagnostic: log every live UUserWidget instance (name + class). Used to find
// the OMEGA WARNING startup widget's class so we can auto-Proceed past it. Fast:
// resolves the UserWidget UClass once and walks each object's super chain by
// POINTER compare (no per-object string work). Any widget whose name/class looks
// like an intro/warning gate is flagged and its UFunctions dumped (to find the
// Proceed/Skip to call next).
void DumpLiveWidgets() {
    void* userWidgetCls = R::FindClass(L"UserWidget");
    if (!userWidgetCls) { UE_LOGW("widgets: UserWidget class not found"); return; }
    static const wchar_t* kGateWords[] = {L"omega", L"warning", L"intro", L"disclaimer",
                                          L"epilepsy", L"splash", L"boot", L"startup",
                                          L"proceed", L"continue", L"title", L"legal"};
    const int32_t n = R::NumObjects();
    int found = 0, flagged = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* c = R::ClassOf(obj);
        bool isWidget = false;
        for (int d = 0; d < 16 && c; ++d) {
            if (c == userWidgetCls) { isWidget = true; break; }
            c = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(c) + P::off::UStruct_SuperStruct);
        }
        if (!isWidget) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;  // skip CDOs
        const std::wstring cn = R::ClassNameOf(obj);
        ++found;
        bool gate = false;
        for (const wchar_t* w : kGateWords)
            if (ContainsCI(nm, w) || ContainsCI(cn, w)) { gate = true; break; }
        if (gate) {
            UE_LOGI("widget[GATE?]: %ls : %ls -- dumping its UFunctions:", cn.c_str(), nm.c_str());
            DumpClassFunctions(R::ClassOf(obj), cn.c_str());
            ++flagged;
        } else {
            UE_LOGI("widget: %ls : %ls", cn.c_str(), nm.c_str());
        }
    }
    UE_LOGI("widgets: %d live UserWidget instances (%d flagged as intro/gate candidates)", found, flagged);
}

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
bool BootStorySaveBlocking() {
    // STORY save slot, from votv-coop.ini "save=<slot>" (defaults s_may2026). Coop
    // targets story mode, so we never boot the sandbox map fresh.
    const std::string slotA = cfg::ReadIniValue("save", "s_may2026");
    const std::wstring slot(slotA.begin(), slotA.end());  // ASCII slot name
    UE_LOGI("harness: target STORY save '%ls'", slot.c_str());
    for (int i = 0; i < 80; ++i) {  // ~120 s cap (boot + omega + level load)
        if (coop::shutdown::IsShuttingDown()) {
            UE_LOGI("harness: BootStorySaveBlocking aborting -- shutdown signaled");
            return false;
        }
        auto st = std::make_shared<std::atomic<int>>(0);  // 0 pending,1 retry,2 ok
        Post([slot, st] { st->store(ue_wrap::engine::LoadStorySave(slot.c_str()) ? 2 : 1); });
        while (st->load() == 0) ::Sleep(5);
        if (st->load() == 2) return true;
        ::Sleep(1500);
    }
    UE_LOGW("harness: did not reach story gameplay in time for '%ls'", slot.c_str());
    return false;
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
    if (storyBoot) {
        for (int k = 0; k < 7; ++k) {  // ~2.8 s of coverage
            Post([k] { UE_LOGI("widgets: == intro dump pass %d ==", k); DumpLiveWidgets(); });
            ::Sleep(400);
        }
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
            } else if (netCfg.role == coop::net::Role::Client) {
                Post([] {
                    void* local = coop::players::Registry::Get().Local();
                    if (!local) {
                        UE_LOGW("client KPP teleport: local mainPlayer not in world yet");
                        return;
                    }
                    const ue_wrap::FVector kpp{
                        P::name::kKPPSpawnX,
                        P::name::kKPPSpawnY,
                        P::name::kKPPSpawnZ,
                    };
                    ue_wrap::engine::SetActorLocation(local, kpp);
                    UE_LOGI("client KPP teleport: mainPlayer -> (%.0f,%.0f,%.0f)",
                            kpp.X, kpp.Y, kpp.Z);
                });
                ::Sleep(100);  // let the posted task run before session.Start
            }
            coop::event_feed::SetLocalNickname(cfg::ReadNickname());
            coop::event_feed::OnSessionStart();
            // Reset net_pump edge-detector state (g_wasConnected[BySlot] +
            // held-prop pointer/key/count). Without this a session
            // Stop()/Start() cycle on the same process would carry stale
            // entries into the new session -- phantom disconnect edge,
            // suppressed connect-edge replay, or a SendPropRelease for the
            // OLD session's key on the NEW session (audit fix 2026-05-24).
            coop::net_pump::OnSessionStart();
            coop::prop_lifecycle::SetSession(&g_session);
            coop::npc_sync::SetSession(&g_session);
            coop::prop_snapshot::SetSession(&g_session);
            coop::dev::restore_vitals::SetSession(&g_session);
            coop::dev::teleport_client::SetSession(&g_session);
            coop::dev::force_weather::SetSession(&g_session);
            coop::moderation::SetSession(&g_session);
            // PR-FOUNDATION-2 (A): snapshot the canonical save BEFORE coop starts
            // injecting state. Host-only -- the host's save is the one written
            // during coop (clients are blocked from saving). Runs on this bringup
            // thread (not the game thread), synchronously, so it completes before
            // Start. Idempotent per process.
            if (netCfg.role == coop::net::Role::Host) {
                coop::save_guard::BackupSaveOnSessionStart();
                // Phase 2: load the persistent banlist + install the accept
                // filter BEFORE Start spawns the net thread (the filter pointer
                // is read there). Host-only -- a client never accepts incoming
                // connections, so it has no banlist.
                coop::ban_list::Load();
                g_session.SetAcceptFilter(&BanAcceptFilter);
            }
            g_session.Start(netCfg);
            UE_LOGI("harness: ==== PLAY READY (coop net %s) ====",
                    netCfg.role == coop::net::Role::Host ? "host" : "client");

            // Autonomous autotest dispatch: spawn each VOTVCOOP_RUN_*_TEST worker
            // thread whose env flag is set (every routine self-gates on role
            // internally). Extracted to harness/autotest_dispatch.cpp -- the
            // cluster had grown to five near-identical blocks.
            harness::autotest::SpawnEnvGatedTests(netCfg.role);

            // Audit H8 (2026-05-27): the 30-second autotest correction loop
            // was deleted. It existed to fight K2_TeleportTo getting reverted
            // by VOTV's player constraints during late-init. The initial-
            // teleport block above now uses coop::dev::teleport_client::ApplyLocally,
            // which routes through teleportWObackrooms -- VOTV's own
            // non-revert path. Once the first teleport sticks, no correction
            // is needed.

            int tick = 0;
            while (!coop::shutdown::IsShuttingDown()) {
                Post([] { coop::net_pump::Tick(g_session, 0.f); coop::nameplate::Update(); coop::roster::Refresh(); TickShutdownHooks(); });
                // Z-trace tick: every ~500 ms log the local actor.Z AND the
                // local mesh_playerVisible.world.Z (the value the source now
                // streams). This catches the "puppet jumps afloat after init"
                // signature: if the local's mesh.world.Z changes over the
                // first few seconds, the streamed Z changes, the puppet's
                // rendered world Z changes -- the jump moment lives in this
                // trace. Tight cadence (500 ms) for the first 30 s, then drop
                // to the 2 s stats interval.
                using namespace std::chrono;
                static const auto sTickEpoch = steady_clock::now();
                const bool tightTrace = (steady_clock::now() - sTickEpoch) < seconds(30);
                if (tightTrace && (tick % 60 == 0)) {  // ~500 ms at 125 Hz
                    Post([] {
                        void* lp = coop::players::Registry::Get().Local();
                        if (!lp) return;
                        const auto actorLoc = ue_wrap::engine::GetActorLocation(lp);
                        float meshWorldZ = NAN, meshRelZ = NAN;
                        if (void* mc = ue_wrap::puppet::GetMeshPlayerVisibleComponent(lp)) {
                            meshWorldZ = ue_wrap::engine::GetComponentLocation(mc).Z;
                            meshRelZ = ue_wrap::engine::GetComponentRelativeLocation(mc).Z;
                        }
                        // Option G: the wire carries actor.Z; the receiver applies
                        // mesh.RelLoc.Z (raw +0x11C, BP-authored static value). The
                        // raw RelLoc.Z column should STABILIZE within 1-2 sec of
                        // load. If it keeps drifting beyond that, a BP timeline
                        // (wakingup, fixCrouch) is still touching it -- which the
                        // puppet WON'T see, because we don't stream it anymore.
                        UE_LOGI("Z-trace: local actor.Z=%.2f mesh.RelLoc.Z=%.2f mesh.world.Z=%.2f (delta=%.2f)%s",
                                actorLoc.Z, meshRelZ, meshWorldZ, meshWorldZ - actorLoc.Z,
                                coop::net_pump::Puppet(1).valid() ? "" : " (puppet not spawned)");
                        if (coop::net_pump::Puppet(1).valid()) {
                            const auto pp = coop::net_pump::Puppet(1).GetLocation();
                            UE_LOGI("Z-trace: puppet world.Z=%.2f (= wire.actor.Z + cached local mesh.RelLoc.Z)",
                                    pp.Z);
                        }
                    });
                }
                if (++tick % 250 == 0) {  // ~every 2 s at 125 Hz: stats for the LAN test framework
                    Post([] {
                        UE_LOGI("net stats: state=%d sent=%llu recv=%llu puppet=%d",
                                static_cast<int>(g_session.state()),
                                static_cast<unsigned long long>(g_session.packetsSent()),
                                static_cast<unsigned long long>(g_session.packetsRecv()),
                                coop::net_pump::Puppet(1).valid() ? 1 : 0);
                        // Position diagnostic: log the local actor AND the puppet
                        // (if alive) world positions. Lets us see whether the
                        // autotest teleport stuck + whether the pose stream is
                        // updating the puppet position vs leaving it at spawn.
                        if (void* lp = coop::players::Registry::Get().Local()) {
                            const auto loc = ue_wrap::engine::GetActorLocation(lp);
                            const auto rot = ue_wrap::engine::GetActorRotation(lp);
                            ue_wrap::FRotator cRot{};
                            if (void* c = ue_wrap::engine::GetController(lp)) {
                                cRot = ue_wrap::engine::GetControlRotation(c);
                            }
                            UE_LOGI("pos diag: local actor=(%.0f,%.0f,%.0f) actorYaw=%.1f ctrl(P=%.1f Y=%.1f)",
                                    loc.X, loc.Y, loc.Z, rot.Yaw, cRot.Pitch, cRot.Yaw);
                        }
                        if (coop::net_pump::Puppet(1).valid()) {
                            const auto p = coop::net_pump::Puppet(1).GetLocation();
                            UE_LOGI("pos diag: puppet world=(%.0f,%.0f,%.0f)", p.X, p.Y, p.Z);
                        }
                    });
                }
                // 125 Hz pump for MAX SMOOTHNESS: RemotePlayer::Tick advances the
                // interp at game-frame rate (up to ~120 fps), so the 50 ms LERP
                // window resolves to ~6 micro-steps -- no visible stepping. The
                // earlier 60 Hz cap was a workaround for 2-instance same-machine
                // LAN-test CPU starvation; HANDS-ON play is a single host instance
                // (the client runs in a separate game folder = separate process =
                // its own CPU/GPU budget), so the 2-process contention scenario
                // doesn't apply here. Tick is cheap (~3 UFunction writes per call,
                // dirty-gated; SetLocalPose lock + 20-byte copy). If a future
                // multi-instance test surfaces starvation again, the proper fix
                // is a per-frame engine hook -- not dropping the rate.
                ::Sleep(8);
            }
        } else {
            SpawnSecondPlayerWhenReady();
            UE_LOGI("harness: ==== PLAY READY (you have control) ====");
            // (No auto-screenshot here: GDI can't read the 3D swapchain in-process ->
            // black. Use the external tools/capture-window.ps1 for gameplay frames.)
            // Keep the 3D nameplate(s) glued above the head + facing the local player.
            // Also install the Stage-1 grab observers (idempotent) so single-
            // instance hands-on play sees the hook lines fire on E-press --
            // the net branch installs them via NetPumpTick, but this branch
            // doesn't run NetPumpTick. The observers are purely local
            // (observe THIS instance's own UFunctions); zero wire dependency.
            while (!coop::shutdown::IsShuttingDown()) {
                Post([] {
                    coop::net_pump::InstallObservers(g_session);  // idempotent; net branch calls the same fn from net_pump::Tick
                    coop::nameplate::Update(); coop::roster::Refresh();
                    TickShutdownHooks();
                });
                ::Sleep(50);
            }
        }
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
        coop::event_feed::SetLocalNickname(cfg::ReadNickname());
        coop::event_feed::OnSessionStart();
        coop::net_pump::OnSessionStart();
        coop::prop_lifecycle::SetSession(&g_session);
        coop::npc_sync::SetSession(&g_session);
        coop::prop_snapshot::SetSession(&g_session);
        coop::dev::restore_vitals::SetSession(&g_session);
        coop::dev::teleport_client::SetSession(&g_session);
        coop::dev::force_weather::SetSession(&g_session);
        coop::moderation::SetSession(&g_session);
        // netloopback is a host self-test (it dials 127.0.0.1). Load the banlist
        // + install the accept filter for parity with the real host path; a
        // banned localhost would be unusual but the wiring is identical.
        coop::ban_list::Load();
        g_session.SetAcceptFilter(&BanAcceptFilter);
        g_session.Start(cfg);
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
    // TAB player-list scoreboard (a second overlay surface, shown to everyone). The
    // roster snapshot reads this session; Refresh() runs in the game-thread ticks.
    coop::roster::SetSession(&g_session);
    if (!ui::imgui_overlay::Init()) {
        UE_LOGW("harness: imgui_overlay::Init failed -- F1 menu unavailable this run");
    }

    // The VOTVCOOP_SPAWN_TRIGGER file watcher (autonomous NPC-spawn path that
    // exercises host AllocAndInstall + broadcast + client mirror Install). Hands-on
    // spawning is the F1 menu (Game > Entities). No-op unless the trigger env is set.
    coop::dev::spawn_npc::Init();

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
