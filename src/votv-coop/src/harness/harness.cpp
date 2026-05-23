#include "harness/harness.h"

#include "harness/screenshot.h"
#include "dev/freecam.h"
#include "dev/pos_hud.h"
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
#include <chrono>
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

// Game thread: read the local player's pose into a network snapshot. x/y/z carry
// the source actor's NATIVE world location (capsule centre on a Character) -- the
// engine's own frame, no derived feet-Z or centre-Z arithmetic. The receiver
// reconstructs the visible-body offset at the PUPPET'S ACTOR TRANSFORM (its
// SkeletalMeshActor's mesh comp is the root, so a sub-component RelLoc/RelRot
// shim is impossible). At puppet spawn the receiver measures the local
// mainPlayer's lowest visible bone Z and mesh world transform, derives the
// actor-Z + actor-Yaw additive offsets that ground the visible feet AND
// reconcile the BP-authored mesh-yaw convention, and applies them every
// ApplyToEngine. The wire carries one thing (where the source's actor is),
// one frame -- MTA-style.
bool ReadLocalPose(void* local, void* controller, coop::net::PoseSnapshot& out) {
    if (!local) return false;
    const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(local);
    const ue_wrap::FRotator actorRot = ue_wrap::engine::GetActorRotation(local);
    const ue_wrap::FVector vel = ue_wrap::engine::GetActorVelocity(local);
    // BODY yaw: read from the ACTOR (the real body facing direction). Earlier
    // attempt sent controller yaw -- but VOTV's Character body LAGS the camera
    // (head-leads-body is natural to the source), so sending controller yaw made
    // the puppet body show the CAMERA direction instead of the BODY direction
    // = sideways-when-camera-leads (user-confirmed regression).
    // HEAD pitch: read from the CONTROLLER (actor pitch is always 0 on an
    // upright character; the controller carries the real view pitch). Cached
    // by NetPumpTick to skip re-resolving GetController every tick.
    out.x = loc.X;
    out.y = loc.Y;
    // Z = source actor.Z (capsule centre on an ACharacter). This is UE4's
    // stable physics position -- CMC floor-snap keeps it within ~2 cm of
    // (ground + halfH), it does NOT swing through the +/-85 cm transient
    // that mesh_playerVisible.world.Z does during BP construction / save-load
    // init (Z-trace 2026-05-23 captured `mesh.world.Z = actor.Z + 2.57` for
    // 1 sec post-teleport, then a drop of 84 cm to settled).
    //
    // The receiver does the offset reconstruction at the puppet ACTOR transform:
    // puppet.actor.Z = wire.z + localMeshRelLocZ, where localMeshRelLocZ is the
    // raw USceneComponent::RelativeLocation.Z field (offset +0x11C) read ONCE
    // from the RECEIVER's own mainPlayer_C at puppet spawn. Same BP class on
    // every peer => identical authored RelLoc.Z constant. RULE 1 root-cause
    // fix per the code-architect verdict + MTA fidelity (MTA streams the
    // CEntitySA matrix.vPos = capsule centre, lets the engine reconstruct
    // visible body offset internally -- exactly the same shape).
    //
    // KNOWN LIMITATION (Phase 2 wire bump): when the SOURCE crouches, UE4's
    // ACharacter::Crouch reduces halfH AND adjusts Mesh.RelLoc.Z upward to
    // keep the feet pinned. The LOCAL's cached RelLoc.Z reflects standing
    // state; the puppet's visible mesh will sit ~(crouchedHalfH - standingHalfH)
    // cm too low when the source is crouched. Fix: stream a `bCrouched` bit
    // in PoseSnapshot.flags; receiver applies a different cached offset when
    // crouched. Tracked alongside the ragdoll sync wire change.
    out.z = loc.Z;
    // Normalize yaw and pitch into the canonical FRotator axis range (-180, 180]
    // BEFORE they go on the wire. UE4's AController::GetControlRotation returns
    // the RAW ControlRotation, which the input system accumulates as unnormalized
    // [0, 360): looking 10 deg DOWN reads back as Pitch=350 (not -10). Without
    // this normalize, pitch=350 fails coop::net::ValidatePose's (-90, 90) bound
    // and the ENTIRE packet is dropped on the receiver, freezing position+yaw+
    // everything while the source looks below horizontal -- root cause of the
    // hands-on "puppet freezes when host looks down then teleports on look-up"
    // bug, two converging agents 2026-05-23. Yaw is normalized too: same
    // unnormalized risk, and a normalized yaw keeps RemotePlayer::errorYaw_
    // symmetric around zero for cleaner linear LERP arcs. Mirrors MTA's
    // SCameraRotationSync bWrapInsteadOfClamp wire policy.
    out.yaw = ue_wrap::NormalizeAxis(actorRot.Yaw);
    const ue_wrap::FRotator ctlRot = controller
        ? ue_wrap::engine::GetControlRotation(controller)
        : actorRot;
    out.pitch = ue_wrap::NormalizeAxis(ctlRot.Pitch);
    // headYawDelta: the source's controller-yaw LEAD over its body yaw, in
    // (-180, 180]. The puppet's AnimBP headLookAt yaw component reads this so
    // the puppet's head turns to match where the source's CAMERA is looking
    // (free-look / camera-lead-body) -- decoupled from the body facing, which
    // is what makes the head-track-local-player default look glaringly wrong
    // on a puppet. Normalized for the same wire-boundary reason as yaw/pitch.
    out.headYawDelta = ue_wrap::NormalizeAxis(ctlRot.Yaw - actorRot.Yaw);
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
// Cached controller for the same pawn -- avoids 2 ProcessEvent dispatches per
// pump tick (GetController + GetControlRotation). Bound to g_netLocal's lifetime:
// nulled when g_netLocal is nulled (level change).
void* g_netLocalController = nullptr;

// Connected-state edge detection for the disconnect cleanup (Destroy the puppet).
// File-scope (NOT a static-local in NetPumpTick) so a future session restart can
// reset it explicitly via ResetNetState below -- otherwise the local-static would
// hold the prior session's value across the new Start (audit fix).
bool g_wasConnected = false;

// ---- Physics-prop pickup observers (Stage 1 of [[project-physics-object-pickup]]) ----
//
// Hook surface: engine-native UPhysicsHandleComponent UFunctions + BP-Timeline
// `grab` auto-functions on mainPlayer_C. The original SDK-header observers
// (smoothGrab/pickupObject/dropGrabObject/throwHoldingProp/switchToHeavyDrag/
// pickupObjectDirect/playerTryToGrab/canPickup) were proven non-dispatching by
// debug build 14d0787 hands-on test 2026-05-23 -- those are BP-pure inline
// functions on mainPlayer_C, so they never appear as ProcessEvent's `function`
// arg. Deleted per RULE 2. Full RE in
// research/findings/votv-physics-interaction-deep-re-2026-05-23.md.
//
// Primary observers (engine-native, universal -- catch every grab path):
//   GrabComponentAtLocation / GrabComponentAtLocationWithRotation  (pickup)
//   SetTargetLocation / SetTargetLocationAndRotation               (per-tick drive)
//   ReleaseComponent                                               (drop, PRE)
//
// Secondary observers (BP-Timeline level, mainPlayer_C scope -- triangulate):
//   InpActEvt_use_K2Node_InputActionEvent_41                       (E press)
//   grab__UpdateFunc (Timeline tick) / grab__FinishedFunc (Timeline end)
//
// The first SetTargetLocation observer fire of a new grab is when we LEARN
// what was grabbed (read mainPlayer.grabbing_actor) and start streaming.
bool g_grabObserversInstalled = false;

// --- Primary: engine PhysicsHandle (`self` IS the UPhysicsHandleComponent,
// owned by mainPlayer_C as `grabHandle`). To learn which prop is held, dereference
// the OuterPrivate chain or read the owning mainPlayer_C's grabbing_actor.

void GrabObserver_PHC_Grab(void* self, void* /*function*/, void* /*params*/) {
    UE_LOGI("grab_hook[PHC.Grab]: handle=%p (pickup -- light grab path)", self);
}

void GrabObserver_PHC_GrabWithRotation(void* self, void* /*function*/, void* /*params*/) {
    UE_LOGI("grab_hook[PHC.GrabWithRot]: handle=%p (pickup w/ rotation)", self);
}

void GrabObserver_PHC_SetTarget(void* self, void* /*function*/, void* /*params*/) {
    // Per-tick driver -- fires every frame the BP graph wants to move the held
    // prop. Hot. Log first 3 calls + every 30th after, so a short autonomous
    // test (5 calls) still sees observer activity but a real grab (~60 Hz x
    // seconds) doesn't drown the log.
    static uint64_t sCount = 0;
    const uint64_t n = ++sCount;
    if (n <= 3 || (n % 30) == 0) {
        UE_LOGI("grab_hook[PHC.SetTarget]: handle=%p (call #%llu)", self,
                static_cast<unsigned long long>(n));
    }
}

void GrabObserver_PHC_SetTargetWithRotation(void* self, void* /*function*/, void* /*params*/) {
    static uint64_t sCount = 0;
    const uint64_t n = ++sCount;
    if (n <= 3 || (n % 30) == 0) {
        UE_LOGI("grab_hook[PHC.SetTargetWithRot]: handle=%p (call #%llu)", self,
                static_cast<unsigned long long>(n));
    }
}

void GrabObserver_PHC_Release_PRE(void* self, void* /*function*/, void* /*params*/) {
    // Pre-dispatch: read GrabbedComponent BEFORE PhysX clears it (offset +176
    // confirmed by IDA decompile of ReleaseComponent_Impl @0x142D7C670).
    if (!self) {
        UE_LOGI("grab_hook[PHC.Release PRE]: handle=null");
        return;
    }
    void* comp = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(self) + P::off::UPhysicsHandleComponent_GrabbedComponent);
    UE_LOGI("grab_hook[PHC.Release PRE]: handle=%p released_component=%p", self, comp);
}

// --- Primary: UPhysicsConstraintComponent (HEAVY drag path). `self` IS the
// constraint component, owned by mainPlayer_C as `heavyGrab` (+0x4F0).
// Confirmed via SDK dump (mainPlayer.hpp:12). VOTV uses a physics CONSTRAINT
// joint between the player and a heavy prop instead of a kinematic handle.

void GrabObserver_PCC_SetConstrainedComponents(void* self, void* /*function*/, void* /*params*/) {
    UE_LOGI("grab_hook[PCC.SetConstrainedComponents]: constraint=%p (heavy grab START)", self);
}

void GrabObserver_PCC_BreakConstraint_PRE(void* self, void* /*function*/, void* /*params*/) {
    UE_LOGI("grab_hook[PCC.BreakConstraint PRE]: constraint=%p (heavy grab END)", self);
}

// --- Secondary: BP-Timeline + input (`self` IS mainPlayer_C). These prove
// the upstream dispatch path and let us read mainPlayer_C grab-state fields.

void GrabObserver_InpActEvt_use(void* self, void* /*function*/, void* /*params*/) {
    // Fires on E-press. The BP graph downstream of this event decides
    // pickup-vs-drop from grabbing_actor and plays the Timeline accordingly.
    // Reading grabbing_actor here = state AT press time (post-observer fires
    // AFTER the BP graph, so it'll show NEW state). For PRE-state we'd need
    // a pre-observer; skipping for Stage 1 -- not actionable yet.
    if (!self) return;
    void* grabbing = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(self) + P::off::mainPlayer_grabbing_actor);
    UE_LOGI("grab_hook[InpActEvt.use]: self=%p grabbing_actor(after)=%p", self, grabbing);
}

void GrabObserver_grab_Update(void* self, void* /*function*/, void* /*params*/) {
    // Per-tick Timeline update. Log first 3 + every 30th after (same throttle
    // policy as PHC.SetTarget so a short autonomous test still sees activity).
    if (!self) return;
    static uint64_t sCount = 0;
    const uint64_t n = ++sCount;
    if (n > 3 && (n % 30) != 0) return;
    void* grabbing = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(self) + P::off::mainPlayer_grabbing_actor);
    bool grabsHeavy = *reinterpret_cast<bool*>(
        reinterpret_cast<uint8_t*>(self) + P::off::mainPlayer_grabsHeavy);
    bool heavy = *reinterpret_cast<bool*>(
        reinterpret_cast<uint8_t*>(self) + P::off::mainPlayer_Heavy);
    float grabLen = *reinterpret_cast<float*>(
        reinterpret_cast<uint8_t*>(self) + P::off::mainPlayer_grabLen);
    UE_LOGI("grab_hook[grab.Update]: holding=%p grabsHeavy=%d Heavy=%d grabLen=%.1f (call #%llu)",
            grabbing, grabsHeavy ? 1 : 0, heavy ? 1 : 0, grabLen,
            static_cast<unsigned long long>(n));
}

void GrabObserver_grab_Finished_PRE(void* self, void* /*function*/, void* /*params*/) {
    // Pre-dispatch: read held prop BEFORE FinishedFunc clears it.
    if (!self) return;
    void* grabbing = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(self) + P::off::mainPlayer_grabbing_actor);
    UE_LOGI("grab_hook[grab.Finished PRE]: was holding=%p", grabbing);
}

// One-shot UFunction-pointer resolution + observer install. Idempotent. Two
// classes involved: UPhysicsHandleComponent (engine, 5 observers) and
// mainPlayer_C (VOTV BP content, 3 observers). FindFunction does NOT climb
// to super, so each must be resolved from its own class. Unresolved
// UFunctions log a warning (BP recooks can renumber the K2Node ordinal in
// the InpActEvt name -- localized failure, others still install).
void InstallGrabObservers() {
    if (g_grabObserversInstalled) return;

    void* phcCls = R::FindClass(P::name::PhysicsHandleComponentClass);
    void* pccCls = R::FindClass(P::name::PhysicsConstraintComponentClass);
    void* playerCls = R::FindClass(P::name::MainPlayerClass);
    if (!phcCls || !pccCls || !playerCls) {
        UE_LOGW("grab_hook: class not found yet (PHC=%p, PCC=%p, mainPlayer=%p) -- will retry next tick",
                phcCls, pccCls, playerCls);
        return;
    }

    auto reg = [](void* cls, const wchar_t* clsName, const wchar_t* fnName,
                  ue_wrap::game_thread::ProcessEventObserverFn cb, bool pre) {
        void* fn = R::FindFunction(cls, fnName);
        if (!fn) {
            UE_LOGW("grab_hook: UFunction '%ls' not found on %ls", fnName, clsName);
            return;
        }
        const bool ok = pre
            ? ue_wrap::game_thread::RegisterPreObserver(fn, cb)
            : ue_wrap::game_thread::RegisterPostObserver(fn, cb);
        if (!ok) {
            UE_LOGW("grab_hook: register %ls failed (table full or null cb)", fnName);
        } else {
            UE_LOGI("grab_hook: registered %s observer for %ls.%ls @ %p",
                    pre ? "PRE" : "POST", clsName, fnName, fn);
        }
    };

    // Primary: engine PhysicsHandle (LIGHT grab path).
    reg(phcCls, P::name::PhysicsHandleComponentClass,
        P::name::GrabComponentAtLocationFn,             GrabObserver_PHC_Grab,                  /*pre=*/false);
    reg(phcCls, P::name::PhysicsHandleComponentClass,
        P::name::GrabComponentAtLocationWithRotationFn, GrabObserver_PHC_GrabWithRotation,      /*pre=*/false);
    reg(phcCls, P::name::PhysicsHandleComponentClass,
        P::name::SetTargetLocationFn,                   GrabObserver_PHC_SetTarget,             /*pre=*/false);
    reg(phcCls, P::name::PhysicsHandleComponentClass,
        P::name::SetTargetLocationAndRotationFn,        GrabObserver_PHC_SetTargetWithRotation, /*pre=*/false);
    reg(phcCls, P::name::PhysicsHandleComponentClass,
        P::name::ReleaseComponentFn,                    GrabObserver_PHC_Release_PRE,           /*pre=*/true);

    // Primary: engine PhysicsConstraint (HEAVY grab path -- different class).
    reg(pccCls, P::name::PhysicsConstraintComponentClass,
        P::name::SetConstrainedComponentsFn, GrabObserver_PCC_SetConstrainedComponents, /*pre=*/false);
    reg(pccCls, P::name::PhysicsConstraintComponentClass,
        P::name::BreakConstraintFn,          GrabObserver_PCC_BreakConstraint_PRE,      /*pre=*/true);

    // Secondary: BP-Timeline + input on mainPlayer_C.
    reg(playerCls, P::name::MainPlayerClass,
        P::name::MainPlayerUseInputEventFn,  GrabObserver_InpActEvt_use,      /*pre=*/false);
    reg(playerCls, P::name::MainPlayerClass,
        P::name::MainPlayerGrabUpdateFn,     GrabObserver_grab_Update,        /*pre=*/false);
    reg(playerCls, P::name::MainPlayerClass,
        P::name::MainPlayerGrabFinishedFn,   GrabObserver_grab_Finished_PRE,  /*pre=*/true);

    g_grabObserversInstalled = true;
    UE_LOGI("grab_hook: Stage 1 observers installed (5 PHC + 2 PCC + 3 BP-Timeline) -- press E on a prop to see hook lines");
}

// ---- Autonomous grab test (no user E-press required) --------------------
// Forced-grab routine: find the nearest Aprop_C derivative, teleport it to
// the player's hand, then drive `grabHandle.GrabComponentAtLocation /
// SetTargetLocation / ReleaseComponent` via reflection (these UFunctions are
// ProcessEvent-dispatched and observable -- so this routine exercises the
// FULL Stage-1 observer pipeline end-to-end without a real keypress).
//
// Gated by env VOTVCOOP_RUN_GRAB_TEST="1" + role=Host. Spawned from the
// netEnabled play loop on a dedicated thread; all engine work goes through
// GT::Post so we never touch UObject state off the game thread.
//
// Expected log lines (on host) after this runs:
//   grab_hook[PHC.Grab]       (once, at GrabComponentAtLocation call)
//   grab_hook[PHC.SetTarget]  (once per second, throttled to 1-in-30)
//   grab_hook[PHC.Release PRE](once, at ReleaseComponent call -- logs the
//                              GrabbedComponent we read off the handle)
//
// Pieces this verifies:
//   1. FindClass + FindFunction resolution for engine-native UFunctions
//   2. ProcessEvent dispatch path through reflection::CallFunction
//   3. Our observer detour catches engine-native UFunctions (not just BP)
//   4. The +176 GrabbedComponent field read in PHC.Release PRE
//
// What this does NOT verify (still requires hands-on or further code):
//   - VOTV BP-graph reaction to the forced grab (the BP doesn't know we
//     called the native UFunction so mainPlayer.grabbing_actor stays null)
//   - Heavy drag path (UPhysicsConstraintComponent) -- different setup
//   - Receiver-side reception (no wire shipped yet -- Stage 4 blocker)

// Pass-2 verification: when run on BOTH peers (host AND client), each does
// a prop scan; comparing their nearest-prop Key.ComparisonIndex tells us
// whether cooked-content FNames are stable cross-peer (which is the
// hypothesis Stage 4's wire serialization depends on). Only the HOST does
// the actual grab/move/release calls -- client is scan-only.
void RunAutonomousGrabTest() {
    const std::string roleEnv = ReadEnv("VOTVCOOP_NET_ROLE");
    const bool isHost = (roleEnv != "client");  // default Host if unset
    const char* roleStr = isHost ? "host" : "client";
    UE_LOGI("grab_test: starting autonomous routine on %s (waiting 10 s for stabilization)", roleStr);
    ::Sleep(10000);

    // ---- 1. Resolve actors + components (game thread).
    struct Resolved {
        void* player = nullptr;
        void* grabHandle = nullptr;
        void* propBase = nullptr;
        void* phcCls = nullptr;
        void* grabFn = nullptr;
        void* setTargetFn = nullptr;
        void* releaseFn = nullptr;
        int32_t grabFrameSize = 0;
        int32_t grabPComp = -1, grabPBone = -1, grabPLoc = -1;
        int32_t stFrameSize = 0;
        int32_t stPLoc = -1;
        bool ok = false;
    };
    auto rsv = std::make_shared<Resolved>();
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([rsv, done] {
        rsv->player = R::FindObjectByClass(P::name::MainPlayerClass);
        if (!rsv->player) { UE_LOGW("grab_test: mainPlayer not found"); done->store(2); return; }
        rsv->grabHandle = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(rsv->player) + P::off::mainPlayer_grabHandle);
        if (!rsv->grabHandle) { UE_LOGW("grab_test: mainPlayer.grabHandle is null"); done->store(2); return; }
        rsv->propBase = R::FindClass(P::name::PropClass);
        if (!rsv->propBase) { UE_LOGW("grab_test: prop_C UClass not found"); done->store(2); return; }
        rsv->phcCls = R::FindClass(P::name::PhysicsHandleComponentClass);
        rsv->grabFn      = R::FindFunction(rsv->phcCls, P::name::GrabComponentAtLocationFn);
        rsv->setTargetFn = R::FindFunction(rsv->phcCls, P::name::SetTargetLocationFn);
        rsv->releaseFn   = R::FindFunction(rsv->phcCls, P::name::ReleaseComponentFn);
        if (!rsv->grabFn || !rsv->setTargetFn || !rsv->releaseFn) {
            UE_LOGW("grab_test: PHC UFunction lookup failed (grab=%p set=%p rel=%p)",
                    rsv->grabFn, rsv->setTargetFn, rsv->releaseFn);
            done->store(2); return;
        }
        rsv->grabFrameSize = R::FunctionFrameSize(rsv->grabFn);
        rsv->grabPComp = R::FindParamOffset(rsv->grabFn, L"Component");
        rsv->grabPBone = R::FindParamOffset(rsv->grabFn, L"InBoneName");
        rsv->grabPLoc  = R::FindParamOffset(rsv->grabFn, L"GrabLocation");
        rsv->stFrameSize = R::FunctionFrameSize(rsv->setTargetFn);
        rsv->stPLoc = R::FindParamOffset(rsv->setTargetFn, L"NewLocation");
        UE_LOGI("grab_test: resolve OK player=%p grabHandle=%p propBase=%p "
                "grabFn=%p frame=%d (Comp@%d Bone@%d Loc@%d) "
                "setTargetFn=%p frame=%d (Loc@%d) releaseFn=%p",
                rsv->player, rsv->grabHandle, rsv->propBase,
                rsv->grabFn, rsv->grabFrameSize, rsv->grabPComp, rsv->grabPBone, rsv->grabPLoc,
                rsv->setTargetFn, rsv->stFrameSize, rsv->stPLoc, rsv->releaseFn);
        rsv->ok = (rsv->grabPComp >= 0 && rsv->grabPBone >= 0 && rsv->grabPLoc >= 0 && rsv->stPLoc >= 0);
        done->store(rsv->ok ? 1 : 2);
    });
    while (done->load() == 0) ::Sleep(5);
    if (!rsv->ok) { UE_LOGW("grab_test: resolve failed -- aborting"); return; }

    // ---- 2. Find nearest Aprop_C derivative (super-walk over GUObjectArray).
    // Anchor: env VOTVCOOP_GRAB_TEST_ANCHOR_{X,Y,Z} if set, else the local
    // player's world location. The env-anchor lets BOTH peers scan around
    // the SAME world point -- which is how the cross-peer Key string match
    // is validated (without an env anchor each peer finds its own nearest).
    const std::string anchXs = ReadEnv("VOTVCOOP_GRAB_TEST_ANCHOR_X");
    const std::string anchYs = ReadEnv("VOTVCOOP_GRAB_TEST_ANCHOR_Y");
    const std::string anchZs = ReadEnv("VOTVCOOP_GRAB_TEST_ANCHOR_Z");
    const bool haveAnchor = !anchXs.empty() && !anchYs.empty() && !anchZs.empty();
    const ue_wrap::FVector envAnchor{
        haveAnchor ? static_cast<float>(std::atof(anchXs.c_str())) : 0.f,
        haveAnchor ? static_cast<float>(std::atof(anchYs.c_str())) : 0.f,
        haveAnchor ? static_cast<float>(std::atof(anchZs.c_str())) : 0.f};
    struct PropResult { void* prop = nullptr; void* mesh = nullptr; float dist = 0.f; std::wstring cls; };
    auto pr = std::make_shared<PropResult>();
    done->store(0);
    GT::Post([rsv, pr, done, haveAnchor, envAnchor] {
        ue_wrap::FVector pLoc = haveAnchor ? envAnchor : ue_wrap::engine::GetActorLocation(rsv->player);
        UE_LOGI("grab_test: scan anchor = (%.0f, %.0f, %.0f) [%s]",
                pLoc.X, pLoc.Y, pLoc.Z, haveAnchor ? "env" : "player");
        const int32_t n = R::NumObjects();
        float bestD2 = 1e18f;
        int candidates = 0, scanned = 0;
        for (int32_t i = 0; i < n; ++i) {
            void* obj = R::ObjectAt(i);
            if (!obj) continue;
            ++scanned;
            // Skip CDOs (Default__*) -- they have no world location and aren't grabbable.
            std::wstring nm = R::ToString(R::NameOf(obj));
            if (nm.rfind(L"Default__", 0) == 0) continue;
            // Walk super chain: is class a descendant of prop_C?
            void* cls = R::ClassOf(obj);
            bool isProp = false;
            for (int hops = 0; hops < 16 && cls; ++hops) {
                if (cls == rsv->propBase) { isProp = true; break; }
                cls = *reinterpret_cast<void**>(
                    reinterpret_cast<uint8_t*>(cls) + P::off::UStruct_SuperStruct);
            }
            if (!isProp) continue;
            ++candidates;
            // Read location via the K2 BP-callable (works for any AActor subclass).
            ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(obj);
            const float dx = loc.X - pLoc.X, dy = loc.Y - pLoc.Y, dz = loc.Z - pLoc.Z;
            const float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < bestD2) {
                bestD2 = d2;
                pr->prop = obj;
                pr->cls = R::ClassNameOf(obj);
            }
        }
        pr->dist = (pr->prop ? std::sqrt(bestD2) : 0.f);
        if (pr->prop) {
            pr->mesh = *reinterpret_cast<void**>(
                reinterpret_cast<uint8_t*>(pr->prop) + P::off::Aprop_StaticMesh);
        }
        UE_LOGI("grab_test: prop scan scanned=%d candidates=%d nearest=%p (%ls) dist=%.1f mesh=%p",
                scanned, candidates, pr->prop, pr->cls.c_str(), pr->dist, pr->mesh);
        // Stage 2 preview: log the prop's identity + heavy/static/frozen
        // bools. Validates the propData / Key / Static / frozen offsets that
        // Stage 4-7 will need for the wire. Aprop_propData_heavy = 0x02CC,
        // Aprop_Static = 0x02D8, Aprop_frozen = 0x02DA, Aprop_Key = 0x02E0.
        if (pr->prop) {
            auto* p = reinterpret_cast<uint8_t*>(pr->prop);
            const bool heavy  = *reinterpret_cast<bool*>(p + P::off::Aprop_propData_heavy);
            const bool staticP = *reinterpret_cast<bool*>(p + P::off::Aprop_Static);
            const bool frozen = *reinterpret_cast<bool*>(p + P::off::Aprop_frozen);
            const R::FName key = *reinterpret_cast<R::FName*>(p + P::off::Aprop_Key);
            const std::wstring keyStr = R::ToString(key);
            UE_LOGI("grab_test: prop fields -- heavy=%d Static=%d frozen=%d Key='%ls' (idx=%d num=%d)",
                    heavy ? 1 : 0, staticP ? 1 : 0, frozen ? 1 : 0,
                    keyStr.c_str(), key.ComparisonIndex, key.Number);
        }
        done->store(pr->prop && pr->mesh ? 1 : 2);
    });
    while (done->load() == 0) ::Sleep(5);
    if (done->load() != 1) { UE_LOGW("grab_test: no suitable prop found -- aborting"); return; }

    // CLIENT: scan + log only. Don't drive any UFunctions -- we're a passive
    // verifier checking whether the Key.ComparisonIndex matches the host's
    // (proves cooked-content FNames are stable cross-peer; logged above).
    if (!isHost) {
        UE_LOGI("grab_test: CLIENT scan-only complete -- compare prop fields above with HOST log");
        UE_LOGI("grab_test: DONE (client scan-only)");
        return;
    }

    // ---- 3. (HOST) Compute hand position (player + forward * 80 cm + Z+50 cm).
    auto handPos = std::make_shared<ue_wrap::FVector>();
    done->store(0);
    GT::Post([rsv, pr, handPos, done] {
        ue_wrap::FVector pLoc = ue_wrap::engine::GetActorLocation(rsv->player);
        ue_wrap::FVector fwd  = ue_wrap::engine::GetActorForwardVector(rsv->player);
        handPos->X = pLoc.X + fwd.X * 80.f;
        handPos->Y = pLoc.Y + fwd.Y * 80.f;
        handPos->Z = pLoc.Z + 50.f;
        // Teleport the prop to the hand position (physics may fight back but
        // the kinematic-target will overwrite each tick).
        ue_wrap::engine::SetActorLocation(pr->prop, *handPos);
        UE_LOGI("grab_test: teleported prop -> (%.1f, %.1f, %.1f)", handPos->X, handPos->Y, handPos->Z);
        done->store(1);
    });
    while (done->load() == 0) ::Sleep(5);

    // ---- 4. Call grabHandle.GrabComponentAtLocation(propMesh, NAME_None, handPos).
    done->store(0);
    GT::Post([rsv, pr, handPos, done] {
        std::vector<uint8_t> frame(static_cast<size_t>(rsv->grabFrameSize), 0);
        *reinterpret_cast<void**>(frame.data() + rsv->grabPComp) = pr->mesh;
        // NAME_None (FName{0,0}).
        *reinterpret_cast<R::FName*>(frame.data() + rsv->grabPBone) = R::FName{0, 0};
        *reinterpret_cast<ue_wrap::FVector*>(frame.data() + rsv->grabPLoc) = *handPos;
        const bool ok = R::CallFunction(rsv->grabHandle, rsv->grabFn, frame.data());
        UE_LOGI("grab_test: CallFunction(GrabComponentAtLocation) -> %d (expect grab_hook[PHC.Grab] above/below)", ok);
        done->store(1);
    });
    while (done->load() == 0) ::Sleep(5);

    // ---- 5. Per-tick SetTargetLocation for 5 seconds.
    UE_LOGI("grab_test: now driving SetTargetLocation 5x (1 sec apart)");
    for (int i = 0; i < 5; ++i) {
        ::Sleep(1000);
        done->store(0);
        const float zJitter = 50.f + static_cast<float>(i) * 5.f;
        GT::Post([rsv, handPos, zJitter, done, i] {
            std::vector<uint8_t> frame(static_cast<size_t>(rsv->stFrameSize), 0);
            ue_wrap::FVector newLoc{handPos->X, handPos->Y, handPos->Z + zJitter - 50.f};
            *reinterpret_cast<ue_wrap::FVector*>(frame.data() + rsv->stPLoc) = newLoc;
            const bool ok = R::CallFunction(rsv->grabHandle, rsv->setTargetFn, frame.data());
            UE_LOGI("grab_test: tick %d SetTargetLocation((%.1f,%.1f,%.1f)) -> %d",
                    i, newLoc.X, newLoc.Y, newLoc.Z, ok);
            done->store(1);
        });
        while (done->load() == 0) ::Sleep(5);
    }

    // ---- 6. ReleaseComponent.
    done->store(0);
    GT::Post([rsv, done] {
        const bool ok = R::CallFunction(rsv->grabHandle, rsv->releaseFn, nullptr);
        UE_LOGI("grab_test: CallFunction(ReleaseComponent) -> %d (expect grab_hook[PHC.Release PRE] above/below)", ok);
        done->store(1);
    });
    while (done->load() == 0) ::Sleep(5);

    UE_LOGI("grab_test: DONE -- autonomous grab routine complete");
}

DWORD WINAPI AutonomousGrabTestThread(LPVOID) {
    RunAutonomousGrabTest();
    return 0;
}

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

    // Detect peer disconnect (Connected -> Handshaking/Disconnected). DESTROY the
    // puppet -- a frozen-in-place puppet of a peer who already quit is confusing
    // and clutters the world. event_feed will also have posted "X left the game"
    // (a chat-feed line that now expires via hud_feed::Tick). If the peer ever
    // reconnects, NetPumpTick auto-spawns a fresh puppet on the first new pose.
    const bool isConnected = (g_session.state() == coop::net::ConnState::Connected);
    if (g_wasConnected && !isConnected && g_orphan.valid()) g_orphan.Destroy();
    g_wasConnected = isConnected;

    if (g_netLocal && !R::IsLive(g_netLocal)) { g_netLocal = nullptr; g_netLocalController = nullptr; }
    if (!g_netLocal) g_netLocal = R::FindObjectByClass(P::name::MainPlayerClass);
    if (g_netLocal) {
        // Re-resolve the controller only when missing or invalidated; the
        // controller pointer stays stable between possess events. Caching here
        // saves ~250 ProcessEvent dispatches/sec at 125 Hz pump (post-ship audit).
        if (g_netLocalController && !R::IsLive(g_netLocalController)) g_netLocalController = nullptr;
        if (!g_netLocalController) g_netLocalController = ue_wrap::engine::GetController(g_netLocal);
        // One-shot install of the grab-observer hooks (Stage 1 of [[project-
        // physics-object-pickup]]). Class is reachable now that local is live;
        // idempotent on g_grabObserversInstalled.
        if (!g_grabObserversInstalled) InstallGrabObservers();
        coop::net::PoseSnapshot mine;
        if (ReadLocalPose(g_netLocal, g_netLocalController, mine)) g_session.SetLocalPose(mine);
    }

    coop::net::PoseSnapshot remote;
    bool isNew = false;
    const bool havePose = g_session.TryGetRemotePose(remote, &isNew);
    if (havePose) {
        if (!g_orphan.valid()) {
            // Spawn-retry backoff: BeginDeferredActorSpawnFromClass refuses if the world
            // is mid-transition (the OMEGA->story transition under 2-instance CPU
            // contention can take many seconds; refused spawns reach hundreds/sec at
            // 60 Hz pump rate -- pure log noise + wasted reflection calls). Only retry
            // once per second after a failure (RULE 1: don't crutch the engine, just
            // wait for it).
            using namespace std::chrono;
            static steady_clock::time_point sNextSpawnAttempt{};
            const auto now = steady_clock::now();
            if (now < sNextSpawnAttempt) return;
            UE_LOGI("net: first remote pose -> auto-spawning remote puppet");
            if (!g_orphan.Spawn()) {
                UE_LOGW("net: remote puppet spawn failed; will retry in 1 s");
                sNextSpawnAttempt = now + seconds(1);
                return;
            }
        }
        // Only RE-BASE the interpolation on a NEW packet; re-pushing the latest
        // every frame would zero `errorPos_` mid-window and freeze motion. The
        // per-frame advance happens in Tick() below.
        if (isNew) {
            coop::net::PoseSnapshot withOffset = remote;
            withOffset.x += displayOffsetX;  // loopback mirror shift (0 for real coop)
            g_orphan.SetTargetPose(withOffset);
        }
    }
    if (g_orphan.valid()) g_orphan.Tick();

    // Surface session events (joins/disconnects) to the feed + send our Join.
    coop::event_feed::Update(g_session, &g_orphan);

    // Expire old chat-feed lines (10 s TTL) so a "X joined the game" line
    // doesn't linger forever like the early version did.
    ue_wrap::hud_feed::Tick();
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
            const std::string xs = ReadEnv("VOTVCOOP_AUTOTEST_X");
            const std::string ys = ReadEnv("VOTVCOOP_AUTOTEST_Y");
            const std::string zs = ReadEnv("VOTVCOOP_AUTOTEST_Z");
            if (!xs.empty() && !ys.empty() && !zs.empty()) {
                const float ax = static_cast<float>(std::atof(xs.c_str()));
                const float ay = static_cast<float>(std::atof(ys.c_str()));
                const float az = static_cast<float>(std::atof(zs.c_str()));
                const std::string yaws   = ReadEnv("VOTVCOOP_AUTOTEST_YAW");
                const std::string pitchs = ReadEnv("VOTVCOOP_AUTOTEST_PITCH");
                const float ayaw   = yaws.empty()   ? 0.f : static_cast<float>(std::atof(yaws.c_str()));
                const float apitch = pitchs.empty() ? 0.f : static_cast<float>(std::atof(pitchs.c_str()));
                // RETRY LOOP: K2_SetActorLocation across large distances on an
                // ACharacter can be silently reverted by VOTV's player constraints
                // (initial diagnostic showed the client's 14-m autotest teleport
                // never stuck while the host's 50-cm one did -- the engine snapped
                // the actor back near the save spawn). K2_TeleportTo is the
                // engine's proper "cross-world teleport" path that bypasses those
                // checks; combine it with a polling retry loop that verifies the
                // actor's actual world position matches the target within 2 m
                // tolerance, since the BP-callable can also lose to a one-frame
                // post-init reposition. Up to 5 s of retries; the loop quits
                // early on success.
                const ue_wrap::FVector target{ax, ay, az};
                const ue_wrap::FRotator targetRot{apitch, ayaw, 0.f};
                bool teleported = false;
                for (int attempt = 0; attempt < 50 && !teleported; ++attempt) {
                    auto okFlag = std::make_shared<std::atomic<int>>(0);  // 0=pending,1=ok,2=nope
                    Post([target, targetRot, ayaw, okFlag] {
                        void* local = R::FindObjectByClass(P::name::MainPlayerClass);
                        if (!local) { okFlag->store(2); return; }
                        ue_wrap::engine::TeleportTo(local, target, targetRot);
                        if (void* ctrl = ue_wrap::engine::GetController(local)) {
                            ue_wrap::engine::SetControlRotation(ctrl, targetRot);
                        }
                        // Verify: read back the actor's world position; success
                        // if within 200 cm of the target on every axis.
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
                    void* local = R::FindObjectByClass(P::name::MainPlayerClass);
                    const auto cur = local ? ue_wrap::engine::GetActorLocation(local) : ue_wrap::FVector{};
                    UE_LOGI("autotest teleport: target=(%.0f,%.0f,%.0f) yaw=%.1f pitch=%.1f "
                            "-> actual=(%.0f,%.0f,%.0f) settled=%d",
                            ax, ay, az, ayaw, apitch, cur.X, cur.Y, cur.Z, teleported ? 1 : 0);
                });
                ::Sleep(100);
            } else if (netCfg.role == coop::net::Role::Client) {
                Post([] {
                    void* local = R::FindObjectByClass(P::name::MainPlayerClass);
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
            coop::event_feed::SetLocalNickname(ReadNickname());
            g_wasConnected = false;  // fresh edge-detector for the disconnect cleanup
            g_session.Start(netCfg);
            UE_LOGI("harness: ==== PLAY READY (coop net %s) ====",
                    netCfg.role == coop::net::Role::Host ? "host" : "client");

            // Autonomous grab test: env-gated, runs on BOTH peers. Host drives
            // the full grab/move/release routine through engine native
            // PhysicsHandle UFunctions; client does scan-only (so we can compare
            // Key.ComparisonIndex cross-peer to verify cooked-content FNames are
            // stable -- the hypothesis Stage 4's wire serialization depends on).
            // See RunAutonomousGrabTest() for the expected log lines.
            if (ReadEnv("VOTVCOOP_RUN_GRAB_TEST") == "1") {
                UE_LOGI("harness: VOTVCOOP_RUN_GRAB_TEST=1 (%s) -- spawning grab test thread",
                        netCfg.role == coop::net::Role::Host ? "host" : "client");
                if (HANDLE h = ::CreateThread(nullptr, 0, AutonomousGrabTestThread, nullptr, 0, nullptr)) {
                    ::CloseHandle(h);
                }
            }

            // Autotest CONTINUED CORRECTION: VOTV's player late-init (BeginPlay /
            // possess / save-restore) can revert the autotest teleport AFTER it
            // appeared to succeed (user-observed: client lands at the correct
            // autotest coords briefly, then VOTV reverts the position once the
            // instance "fully loads"). The fix is to keep re-applying the
            // teleport for ~30 s after session start -- each iteration is a
            // no-op if the actor is already within 200 cm of the target.
            const bool autotestActive = !xs.empty() && !ys.empty() && !zs.empty();
            const float atx = autotestActive ? static_cast<float>(std::atof(xs.c_str())) : 0.f;
            const float aty = autotestActive ? static_cast<float>(std::atof(ys.c_str())) : 0.f;
            const float atz = autotestActive ? static_cast<float>(std::atof(zs.c_str())) : 0.f;
            const float atyaw   = autotestActive ? static_cast<float>(std::atof(ReadEnv("VOTVCOOP_AUTOTEST_YAW").c_str()))   : 0.f;
            const float atpitch = autotestActive ? static_cast<float>(std::atof(ReadEnv("VOTVCOOP_AUTOTEST_PITCH").c_str())) : 0.f;
            using namespace std::chrono;
            const auto autotestUntil = steady_clock::now() + seconds(30);

            int tick = 0;
            for (;;) {
                Post([] { NetPumpTick(0.f); coop::nameplate::Update(); });
                // Autotest correction tick: every ~250 ms while still inside the
                // 30 s window, re-check actor position and re-teleport if it
                // drifted outside the 200-cm tolerance. Catches VOTV's late
                // BeginPlay / save-restore reverting our teleport.
                if (autotestActive && steady_clock::now() < autotestUntil && (tick % 30 == 0)) {
                    Post([atx, aty, atz, atyaw, atpitch] {
                        void* local = R::FindObjectByClass(P::name::MainPlayerClass);
                        if (!local) return;
                        const auto cur = ue_wrap::engine::GetActorLocation(local);
                        const float dx = cur.X - atx, dy = cur.Y - aty, dz = cur.Z - atz;
                        if (std::fabs(dx) > 200.f || std::fabs(dy) > 200.f || std::fabs(dz) > 200.f) {
                            const ue_wrap::FVector target{atx, aty, atz};
                            const ue_wrap::FRotator targetRot{atpitch, atyaw, 0.f};
                            ue_wrap::engine::TeleportTo(local, target, targetRot);
                            if (void* ctrl = ue_wrap::engine::GetController(local)) {
                                ue_wrap::engine::SetControlRotation(ctrl, targetRot);
                            }
                            UE_LOGI("autotest correction: was (%.0f,%.0f,%.0f), retargeting (%.0f,%.0f,%.0f)",
                                    cur.X, cur.Y, cur.Z, atx, aty, atz);
                        }
                    });
                }
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
                        void* lp = R::FindObjectByClass(P::name::MainPlayerClass);
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
                                g_orphan.valid() ? "" : " (puppet not spawned)");
                        if (g_orphan.valid()) {
                            const auto pp = g_orphan.GetLocation();
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
                                g_orphan.valid() ? 1 : 0);
                        // Position diagnostic: log the local actor AND the puppet
                        // (if alive) world positions. Lets us see whether the
                        // autotest teleport stuck + whether the pose stream is
                        // updating the puppet position vs leaving it at spawn.
                        if (void* lp = R::FindObjectByClass(P::name::MainPlayerClass)) {
                            const auto loc = ue_wrap::engine::GetActorLocation(lp);
                            const auto rot = ue_wrap::engine::GetActorRotation(lp);
                            ue_wrap::FRotator cRot{};
                            if (void* c = ue_wrap::engine::GetController(lp)) {
                                cRot = ue_wrap::engine::GetControlRotation(c);
                            }
                            UE_LOGI("pos diag: local actor=(%.0f,%.0f,%.0f) actorYaw=%.1f ctrl(P=%.1f Y=%.1f)",
                                    loc.X, loc.Y, loc.Z, rot.Yaw, cRot.Pitch, cRot.Yaw);
                        }
                        if (g_orphan.valid()) {
                            const auto p = g_orphan.GetLocation();
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
            for (;;) {
                Post([] {
                    if (!g_grabObserversInstalled) InstallGrabObservers();
                    coop::nameplate::Update();
                });
                ::Sleep(50);
            }
        }
    } else if (scenario == "netloopback") {
        // Autonomous Phase 3 plumbing test: ONE process, ONE session in loopback
        // (role=host, peer=self, initiate=true). The session sends the local
        // player's pose to itself over real UDP and receives it back; the puppet is
        // auto-spawned on the first packet (3.5) and Drive()n from the round-tripped
        // pose, offset +250 in X so the mirror is visibly beside the player. Proves
        // transport+serialization+session+interp end-to-end without a 2nd machine.
        BootStorySaveBlocking();
        coop::net::Config cfg;
        cfg.role = coop::net::Role::Host;
        cfg.peerIp = "127.0.0.1";
        cfg.initiate = true;  // host targets itself for the loopback handshake
        coop::event_feed::SetLocalNickname(ReadNickname());
        g_wasConnected = false;  // fresh edge-detector for the disconnect cleanup
        g_session.Start(cfg);
        UE_LOGI("harness: ==== NETLOOPBACK running (self UDP on %u) ====", cfg.port);
        int tick = 0;
        for (;;) {
            Post([] { NetPumpTick(250.f); coop::nameplate::Update(); });
            if (++tick % 120 == 0) {  // ~every 2 s at 60 Hz
                Post([] {
                    UE_LOGI("netloopback: state=%d sent=%llu recv=%llu puppet=%d",
                            static_cast<int>(g_session.state()),
                            static_cast<unsigned long long>(g_session.packetsSent()),
                            static_cast<unsigned long long>(g_session.packetsRecv()),
                            g_orphan.valid() ? 1 : 0);
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
            g_orphan.Spawn();
        });
        ::Sleep(3000);
        Post([] {
            if (!g_orphan.valid()) { UE_LOGW("show: no puppet"); return; }
            const ue_wrap::FVector at = g_orphan.GetLocation();
            UE_LOGI("show: drive WALK in place (speed=200) to test AnimBP locomotion");
            // Same loc/yaw, just bump speed -- the first SetTargetPose since spawn
            // snaps (hasPose_ false), then Tick applies. AnimBP locomotion picks it up.
            coop::net::PoseSnapshot s{at.X, at.Y, at.Z, /*yaw*/0.f, /*pitch*/0.f, /*speed*/200.f};
            g_orphan.SetTargetPose(s);
            g_orphan.Tick();
        });
        ::Sleep(4000);
        Post([] {
            if (!g_orphan.valid()) return;
            const ue_wrap::FVector at = g_orphan.GetLocation();
            coop::net::PoseSnapshot s{at.X, at.Y, at.Z, /*yaw*/0.f, /*pitch*/0.f, /*speed*/0.f};
            g_orphan.SetTargetPose(s);
            g_orphan.Tick();
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

    // Dev pos + camera-rotation overlay (F2 toggle). No-op unless ini enables it.
    dev::pos_hud::Init();

    auto* scenario = new std::string(ReadScenario());
    if (HANDLE t = ::CreateThread(nullptr, 0, TimelineThread, scenario, 0, nullptr)) {
        ::CloseHandle(t);
    } else {
        delete scenario;
        UE_LOGE("harness: failed to start timeline thread");
    }
}

}  // namespace harness
