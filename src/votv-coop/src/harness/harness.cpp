#include "harness/harness.h"

#include "harness/screenshot.h"
#include "dev/freecam.h"
#include "coop/event_feed.h"
#include "coop/nameplate.h"
#include "coop/net/session.h"
#include "coop/remote_player.h"
#include "ue_wrap/hud_feed.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <memory>
#include <string>

namespace harness {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

// Directory of this mod DLL (scenario.txt lives next to it, like the Lua mod's).
std::wstring ModuleDir() {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p(path);
    const size_t sep = p.find_last_of(L"\\/");
    return sep == std::wstring::npos ? L"." : p.substr(0, sep);
}

// Read an environment variable (ASCII). Empty string if unset. Used by the
// two-instance LAN test framework: both instances load the SAME DLL from the SAME
// game dir, so they cannot be configured by per-file scenario.txt/votv-coop.ini --
// the framework passes each instance its role/peer/etc. via the environment instead.
std::string ReadEnv(const char* name) {
    char buf[256] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf) : std::string();
}

std::string ReadScenario() {
    // Env override first (LAN test framework), then scenario.txt, then default.
    const std::string env = ReadEnv("VOTVCOOP_SCENARIO");
    if (!env.empty()) return env;
    const std::wstring path = ModuleDir() + L"\\scenario.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) return "newgame";
    char line[64] = {};
    char* got = std::fgets(line, sizeof(line), f);
    std::fclose(f);
    if (!got) return "newgame";
    std::string s(line);
    // trim whitespace
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    return s.empty() ? "newgame" : s;
}

// Read a single "key=value" line from votv-coop.ini (section-agnostic). Returns
// the value (trimmed), or `def` if the key is absent. ASCII.
std::string ReadIniValue(const char* key, const char* def) {
    const std::wstring path = ModuleDir() + L"\\votv-coop.ini";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) return def;
    const std::string prefix = std::string(key) + "=";
    std::string result = def;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        s.erase(std::remove_if(s.begin(), s.end(),
                               [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }),
                s.end());
        if (s.rfind(prefix, 0) == 0) { result = s.substr(prefix.size()); break; }
    }
    std::fclose(f);
    return result;
}

// The single coop networking session (Phase 3). Host binds the LAN port; client
// targets the host. Drives the remote puppet from received pose snapshots and
// sends the local player's pose. Off unless a scenario starts it.
coop::net::Session g_session;

// Build the net Config from votv-coop.ini ("net.role=host|client", "net.peer=<ip>",
// "net.port=<n>"). `enabled` is set false when no role is configured (the default:
// hands-on play stays single-machine until coop is wired up on a 2nd box).
coop::net::Config ReadNetConfig(bool& enabled) {
    coop::net::Config c;
    // Env first (LAN test framework gives each instance its role), then ini.
    std::string role = ReadEnv("VOTVCOOP_NET_ROLE");
    if (role.empty()) role = ReadIniValue("net.role", "");
    enabled = (role == "host" || role == "client");
    c.role = (role == "client") ? coop::net::Role::Client : coop::net::Role::Host;

    std::string peer = ReadEnv("VOTVCOOP_NET_PEER");
    c.peerIp = peer.empty() ? ReadIniValue("net.peer", "127.0.0.1") : peer;

    std::string port = ReadEnv("VOTVCOOP_NET_PORT");
    if (port.empty()) port = ReadIniValue("net.port", "");
    if (!port.empty()) c.port = static_cast<uint16_t>(std::strtoul(port.c_str(), nullptr, 10));
    return c;
}

// The local player's display nickname (env first, then ini, then default).
std::wstring ReadNickname() {
    std::string nick = ReadEnv("VOTVCOOP_NET_NICK");
    if (nick.empty()) nick = ReadIniValue("net.nick", "Player");
    return std::wstring(nick.begin(), nick.end());
}

// Game thread: read the local player's pose into a network snapshot. z carries the
// FEET (capsule centre - half-height), so the remote machine's mesh-root puppet
// stands on the ground; yaw is the body facing; speed is horizontal velocity (cm/s).
bool ReadLocalPose(void* local, coop::net::PoseSnapshot& out) {
    if (!local) return false;
    const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(local);
    const ue_wrap::FRotator rot = ue_wrap::engine::GetActorRotation(local);
    const ue_wrap::FVector vel = ue_wrap::engine::GetActorVelocity(local);
    const float halfH = ue_wrap::puppet::GetCapsuleHalfHeight(local);
    out.x = loc.X;
    out.y = loc.Y;
    out.z = loc.Z - halfH;
    out.yaw = rot.Yaw;
    out.speed = std::sqrt(vel.X * vel.X + vel.Y * vel.Y);
    return true;
}

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

// The C++ orphan (replaces the Lua harness's SpawnOrphan/DriveOrphan): a
// coop::RemotePlayer spawned + pose-driven via our own CallFunction path.
coop::RemotePlayer g_orphan;

// Cached local mainPlayer_C for the net pump. FindObjectByClass walks the whole
// GUObjectArray, so we resolve it ONCE, hold it, re-validate with IsLive, and only
// re-find if it died (level change) -- never a full scan per tick (post-ship audit).
void* g_netLocal = nullptr;

// Game thread, ~send-rate: push the local player's pose to the session and apply
// the latest remote pose to the puppet (auto-spawning it on the first packet,
// methodology 3.5). `displayOffsetX` shifts the rendered puppet sideways so a
// LOOPBACK mirror (remote pose == our own) is visible next to us; 0 for real coop.
void NetPumpTick(float displayOffsetX) {
    // Lazily bring up the on-screen event feed once the GameInstance exists (it is the
    // persistent widget outer). One-time FindObjectByClass until it succeeds, then it
    // stops -- never a per-tick walk after init (post-ship audit).
    if (!ue_wrap::hud_feed::IsInitialized()) {
        if (void* gi = R::FindObjectByClass(P::name::GameInstanceClass)) ue_wrap::hud_feed::Init(gi);
    }

    if (g_netLocal && !R::IsLive(g_netLocal)) g_netLocal = nullptr;
    if (!g_netLocal) g_netLocal = R::FindObjectByClass(P::name::MainPlayerClass);
    if (g_netLocal) {
        coop::net::PoseSnapshot mine;
        if (ReadLocalPose(g_netLocal, mine)) g_session.SetLocalPose(mine);
    }

    coop::net::PoseSnapshot remote;
    if (g_session.TryGetRemotePose(remote)) {
        if (!g_orphan.valid()) {
            UE_LOGI("net: first remote pose -> auto-spawning remote puppet");
            if (!g_orphan.Spawn()) { UE_LOGW("net: remote puppet spawn failed; will retry"); return; }
        }
        const ue_wrap::FVector loc{remote.x + displayOffsetX, remote.y, remote.z};
        g_orphan.Drive(loc, remote.yaw, remote.speed);
    }

    // Surface session events (joins/disconnects) to the feed + send our Join.
    coop::event_feed::Update(g_session, &g_orphan);
}

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
        auto state = std::make_shared<std::atomic<int>>(0);  // 0 pending,1 not-ready,2 ok,3 failed
        Post([state, i] {
            if (g_orphan.valid()) { state->store(2); return; }
            void* local = R::FindObjectByClass(P::name::MainPlayerClass);
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
            state->store(g_orphan.Spawn() ? 2 : 3);
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
    const std::string slotA = ReadIniValue("save", "s_may2026");
    const std::wstring slot(slotA.begin(), slotA.end());  // ASCII slot name
    UE_LOGI("harness: target STORY save '%ls'", slot.c_str());
    for (int i = 0; i < 80; ++i) {  // ~120 s cap (boot + omega + level load)
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
            g_orphan.Spawn();
        });
        ::Sleep(2000);
        Post([] { Report("post-spawn"); });
        Post([] {
            if (g_orphan.valid()) {
                ue_wrap::FVector p = g_orphan.GetLocation();
                UE_LOGI("harness: orphan post-spawn pos=(%.0f,%.0f,%.0f)", p.X, p.Y, p.Z);
            }
        });

        // Pose-drive: teleport the orphan in +X steps, read back each time.
        for (int i = 1; i <= 5; ++i) {
            ::Sleep(3000);
            Post([i] {
                if (!g_orphan.valid()) { UE_LOGW("harness: drive %d -- no orphan", i); return; }
                ue_wrap::FVector p = g_orphan.GetLocation();
                p.X += 150.f;
                const bool ok = g_orphan.SetLocation(p);
                ue_wrap::FVector got = g_orphan.GetLocation();
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
        // Coop networking: if votv-coop.ini configures net.role, the puppet is
        // network-driven (auto-spawned on the first peer pose) and we send our pose;
        // otherwise the puppet is spawned locally + static (the pre-net behaviour).
        bool netEnabled = false;
        const coop::net::Config netCfg = ReadNetConfig(netEnabled);
        if (netEnabled) {
            coop::event_feed::SetLocalNickname(ReadNickname());
            g_session.Start(netCfg);
            UE_LOGI("harness: ==== PLAY READY (coop net %s) ====",
                    netCfg.role == coop::net::Role::Host ? "host" : "client");
            int tick = 0;
            for (;;) {
                Post([] { NetPumpTick(0.f); coop::nameplate::Update(); });
                if (++tick % 60 == 0) {  // ~every 2 s: stats for the LAN test framework
                    Post([] {
                        UE_LOGI("net stats: state=%d sent=%llu recv=%llu puppet=%d",
                                static_cast<int>(g_session.state()),
                                static_cast<unsigned long long>(g_session.packetsSent()),
                                static_cast<unsigned long long>(g_session.packetsRecv()),
                                g_orphan.valid() ? 1 : 0);
                    });
                }
                ::Sleep(33);  // ~30 Hz pose pump
            }
        } else {
            SpawnSecondPlayerWhenReady();
            UE_LOGI("harness: ==== PLAY READY (you have control) ====");
            // (No auto-screenshot here: GDI can't read the 3D swapchain in-process ->
            // black. Use the external tools/capture-window.ps1 for gameplay frames.)
            // Keep the 3D nameplate(s) glued above the head + facing the local player.
            for (;;) {
                Post([] { coop::nameplate::Update(); });
                ::Sleep(50);
            }
        }
    } else if (scenario == "netloopback") {
        // Autonomous Phase 3 plumbing test: ONE process, ONE session in loopback
        // (role=host, peer=self, initiate=true). The session sends the local
        // player's pose to itself over real UDP and receives it back; the puppet is
        // auto-spawned on the first packet (3.5) and Drive()n from the round-tripped
        // pose, offset +250 in X so the mirror is visibly beside the player. Proves
        // transport+serialization+session+Drive end-to-end without a 2nd machine.
        BootStorySaveBlocking();
        coop::net::Config cfg;
        cfg.role = coop::net::Role::Host;
        cfg.peerIp = "127.0.0.1";
        cfg.initiate = true;  // host targets itself for the loopback handshake
        coop::event_feed::SetLocalNickname(ReadNickname());
        g_session.Start(cfg);
        UE_LOGI("harness: ==== NETLOOPBACK running (self UDP on %u) ====", cfg.port);
        int tick = 0;
        for (;;) {
            Post([] { NetPumpTick(250.f); coop::nameplate::Update(); });
            if (++tick % 60 == 0) {  // ~every 2 s
                Post([] {
                    UE_LOGI("netloopback: state=%d sent=%llu recv=%llu puppet=%d",
                            static_cast<int>(g_session.state()),
                            static_cast<unsigned long long>(g_session.packetsSent()),
                            static_cast<unsigned long long>(g_session.packetsRecv()),
                            g_orphan.valid() ? 1 : 0);
                });
            }
            ::Sleep(33);  // ~30 Hz
        }
    } else if (scenario == "show") {
        // Autonomous visual confirm: spawn the puppet in front, hold idle, then
        // drive a walk (speed) for a few seconds to confirm the AnimBP animates
        // from our direct variable writes -- the Drive() path UDP will use.
        ::Sleep(2000);
        Post([] {
            UE_LOGI("show: === spawn skin-puppet ===");
            g_orphan.Spawn();
        });
        ::Sleep(3000);
        Post([] {
            if (!g_orphan.valid()) { UE_LOGW("show: no puppet"); return; }
            const ue_wrap::FVector at = g_orphan.GetLocation();
            UE_LOGI("show: drive WALK in place (speed=200) to test AnimBP locomotion");
            g_orphan.Drive(at, 0.f, 200.f);  // same loc/yaw, walking
        });
        ::Sleep(4000);
        Post([] {
            if (!g_orphan.valid()) return;
            const ue_wrap::FVector at = g_orphan.GetLocation();
            g_orphan.Drive(at, 0.f, 0.f);  // back to idle
            UE_LOGI("show: back to idle (speed=0)");
        });
        UE_LOGI("harness: ==== SHOW DONE ====");
    } else if (scenario == "skin") {
        // Investigate the player's visible-body setup: enumerate components of
        // the local pawn and a spawned orphan, and confirm SuperStruct offset.
        ::Sleep(2000);
        Post([] {
            R::DebugProbeSuperStructOffset();
            void* local = R::FindObjectByClass(P::name::MainPlayerClass);
            DumpComponents("local mainPlayer_C", local);
            g_orphan.Spawn();
        });
        ::Sleep(2000);
        Post([] { DumpComponents("orphan mainPlayer_C", g_orphan.actor()); });
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

    // Dev free-flying camera (HOME toggle). No-op unless votv-coop.ini enables it.
    dev::freecam::Init();

    auto* scenario = new std::string(ReadScenario());
    if (HANDLE t = ::CreateThread(nullptr, 0, TimelineThread, scenario, 0, nullptr)) {
        ::CloseHandle(t);
    } else {
        delete scenario;
        UE_LOGE("harness: failed to start timeline thread");
    }
}

}  // namespace harness
