#include "harness/harness.h"

#include "harness/screenshot.h"
#include "dev/freecam.h"
#include "dev/pos_hud.h"
#include "coop/event_feed.h"
#include "coop/nameplate.h"
#include "coop/net/session.h"
#include "coop/remote_player.h"
#include "coop/remote_prop.h"
#include "ue_wrap/hud_feed.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
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
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

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

// v4 held-prop edge detector: file-scope for the same reason as g_wasConnected.
// A static-local would carry a stale prop pointer + key across a session stop/
// restart, causing the next pump to fire SendPropRelease for the OLD session's
// key on the NEW session -- a real bug found by the audit. Cleared explicitly
// alongside g_wasConnected on each session.Start (post-ship audit 2026-05-24).
void* g_lastHeldProp = nullptr;
coop::net::WireKey g_lastHeldKey{};
uint64_t g_propEmitCount = 0;

// v5 (2026-05-24 post-RE): the v4 throw-impulse cache has been retired per
// RULE 2. The root-cause RE
// (research/findings/votv-throw-release-pipeline-RE-2026-05-24.md) showed the
// dominant launch energy is NOT a discrete AddImpulse call -- it is the
// kinematic-tracking velocity PhysX accumulates while the player flicks the
// camera (Bug B). The release-edge in NetPumpTick now reads the body's
// inherited linear+angular velocity directly via prop::GetPhysicsVelocity
// AFTER the engine has finished release+optional-AddImpulse on the same
// tick, so ONE number captures the full launch state. The
// GrabObserver_PrimComp_AddImpulse stays only as a diagnostic log line (its
// values are no longer cached or shipped).

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
// Bug C audit C-2 (2026-05-24): the propInventory_C class may not be loaded
// at the moment InstallGrabObservers first runs (engine loads BP classes on
// demand). Track its install separately so NetPumpTick can retry the takeObj
// observer registration each tick until the class appears, without
// re-attempting the 13 already-installed observers.
bool g_inventoryObserverInstalled = false;

// Bug C audit C-3 (2026-05-24): bounded retry queue for PropSpawn payloads
// that couldn't ship immediately because the reliable channel was busy
// (stop-and-wait: chat or PropRelease in-flight). Drained by NetPumpTick on
// each tick. Without this queue, a PropSpawn dropped here means the peer
// permanently has no actor with that Key -- subsequent PropPose updates
// would log "no local match" forever (FindByKeyString requires the prop
// to exist).
std::mutex g_pendingSpawnsMutex;
std::deque<coop::net::PropSpawnPayload> g_pendingSpawns;
// Sized for a snapshot bootstrap: ~2000 Aprop_C derivatives in a populated
// save × 160 B per payload = ~320 KB max queue memory. 4096 leaves headroom
// for both the initial snapshot AND any drops/spawns that fire during the
// snapshot drain. Stop-and-wait reliable means drain takes 5-20 s for a full
// snapshot (~10 ms per packet); during that window other PropSpawn events
// (inventory drops) get appended and ship in order. (Increased from 8 for
// Phase 5S0 -- 2026-05-24.)
constexpr size_t kMaxPendingSpawns = 4096;

void EnqueuePropSpawnForRetry(const coop::net::PropSpawnPayload& payload) {
    std::lock_guard<std::mutex> lk(g_pendingSpawnsMutex);
    if (g_pendingSpawns.size() >= kMaxPendingSpawns) {
        UE_LOGW("net: PropSpawn retry queue full (depth %zu / max %zu) -- dropping OLDEST so newer spawns aren't lost",
                g_pendingSpawns.size(), kMaxPendingSpawns);
        g_pendingSpawns.pop_front();
    }
    g_pendingSpawns.push_back(payload);
    UE_LOGI("net: PropSpawn enqueued for retry (depth now %zu)", g_pendingSpawns.size());
}

// --- Phase 5S0 snapshot enumeration state (audit I-2 chunked send) ------
//
// Audit I-2 (2026-05-24): SendPropSnapshotForPeer originally walked all
// ~2000 Aprop_C derivatives + did 2 ProcessEvent dispatches each
// (GetActorLocation + GetActorRotation) in one NetPumpTick frame -- a
// ~200-400 ms game-thread stall. Split the work: on connected-edge,
// populate g_snapshotCandidates with the live actor list (one fast GU
// walk + cheap descendant/CDO filtering, NO ProcessEvent). Then each
// subsequent NetPumpTick drains kSnapshotChunkSize candidates from the
// list (the per-candidate transform reads), enqueueing each result.
// kSnapshotChunkSize=100 keeps per-tick cost at ~5-15 ms (well below
// the 16.6 ms frame budget).
std::vector<void*> g_snapshotCandidates;  // game-thread only; no lock
size_t g_snapshotCandidateIdx = 0;
constexpr size_t kSnapshotChunkSize = 100;

// --- Phase 5S0 (Save Snapshot Bootstrap) -- snapshot-on-connect ----------
//
// USER DIRECTIVE 2026-05-24 ("client just reconnects to the host's game and
// all objects and states are force synced"): when the session reaches
// Connected, host enumerates every live Aprop_C derivative in GUObjectArray
// and broadcasts a PropSpawn for each. Client OnSpawn de-dupes on
// FindByKeyString -- existing actors (loaded from the same save) are skipped;
// missing actors (host-side mid-game spawn that the client never saw) are
// created. Result: client's world converges to host's prop set after every
// (re)connect. Mushroom desync from independent spawners gets corrected
// because client's old-location mushroom now shares Key with host's mushroom
// and the de-dupe path teleports it to host's transform.
//
// Trigger: edge detector in NetPumpTick (g_wasConnected false -> true) +
// role == Host. Re-fires on every reconnect.
//
// Cost: one GUObjectArray walk (~237k slots, ~2k candidates). Each candidate
// allocates a wstring for the CDO-skip check (same cost as the existing
// FindNearest scan in prop_wrap). One-shot per (re)connect, not per-tick.
//
// Stop-and-wait reliable channel: SendPropSpawn returns false when busy.
// We enqueue every snapshot prop via EnqueuePropSpawnForRetry so DrainPending
// takes over -- ~10 ms per packet, so 2000 props = ~20 s to drain. During
// that window the world keeps streaming via PropPose; no blocking.
// Phase 1 of snapshot (fast): walk GUObjectArray ONCE, collect live
// Aprop_C derivative actor pointers into g_snapshotCandidates. No
// ProcessEvent dispatch, no enqueue. Cheap O(NumObjects) walk + the
// descendant/CDO filter (one wstring allocation per ~2000 candidates).
// Returns the number of candidates collected.
size_t EnumerateSnapshotCandidates() {
    g_snapshotCandidates.clear();
    g_snapshotCandidateIdx = 0;
    int skippedCDO = 0, skippedDying = 0;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (!ue_wrap::prop::IsDescendantOfProp(obj)) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) { ++skippedCDO; continue; }
        if (!R::IsLive(obj)) { ++skippedDying; continue; }
        g_snapshotCandidates.push_back(obj);
    }
    UE_LOGI("snapshot: enumerated %zu Aprop_C live candidates (skipped %d CDOs, %d dying); will drain %zu per tick",
            g_snapshotCandidates.size(), skippedCDO, skippedDying, kSnapshotChunkSize);
    return g_snapshotCandidates.size();
}

// Forward-decl for the wire-suppress allowlist predicate; defined further
// down (with the lifecycle observers). DrainSnapshotChunk + the Init POST
// observer + event_feed all use the same predicate to keep the
// host-authoritative allowlist consistent across all broadcast sites.
bool IsWireSuppressedPropClass(const std::wstring& cls);

// Phase 2 of snapshot (chunked): per-tick, read transform + state for up
// to kSnapshotChunkSize candidates, build PropSpawnPayload, enqueue.
// Stops when g_snapshotCandidates is exhausted. Per-tick cost ~5-15 ms
// at chunk=100; total wall time for 2000 props ~20-30 ticks (~330 ms-
// 500 ms but spread across frames so no single-frame stall).
void DrainSnapshotChunk() {
    if (g_snapshotCandidateIdx >= g_snapshotCandidates.size()) return;
    // (std::min) parenthesized to defeat windows.h's `min` macro.
    const size_t limit = (std::min)(g_snapshotCandidateIdx + kSnapshotChunkSize,
                                    g_snapshotCandidates.size());
    int sent = 0;
    for (; g_snapshotCandidateIdx < limit; ++g_snapshotCandidateIdx) {
        void* obj = g_snapshotCandidates[g_snapshotCandidateIdx];
        // Re-validate liveness: an actor that was live during Phase-1
        // enumeration might have been GC'd in the intervening ticks.
        if (!obj || !R::IsLive(obj)) continue;
        coop::net::PropSpawnPayload p{};
        const std::wstring cls = R::ClassNameOf(obj);
        // Audit C-1 (2026-05-24): same wire-suppress allowlist as the
        // Init POST observer. Intermediate-variant classes (mushroom7_C)
        // never cross the wire -- host-authoritative. Without this, the
        // snapshot stream would queue N mushroom7_C entries per reconnect
        // that the client just drops; reliable stop-and-wait channel
        // means each wasted entry adds ~10 ms latency to snapshot drain.
        if (IsWireSuppressedPropClass(cls)) {
            UE_LOGI("snapshot: skipping intermediate-variant '%ls' actor %p (wire-suppressed)",
                    cls.c_str(), obj);
            continue;
        }
        p.className.len = 0;
        for (size_t j = 0; j < cls.size() && j < 63; ++j) {
            p.className.data[p.className.len++] = static_cast<char>(cls[j]);
        }
        const std::wstring keyStr = ue_wrap::prop::GetKeyString(obj);
        if (keyStr.empty()) continue;
        p.key.len = 0;
        for (size_t j = 0; j < keyStr.size() && j < 31; ++j) {
            p.key.data[p.key.len++] = static_cast<char>(keyStr[j]);
        }
        const auto loc = ue_wrap::engine::GetActorLocation(obj);
        const auto rot = ue_wrap::engine::GetActorRotation(obj);
        p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
        p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
        p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
        p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
        p.scaleX = 1.f; p.scaleY = 1.f; p.scaleZ = 1.f;
        p.physFlags = coop::net::propspawn_flags::kSimulatePhysics;
        if (ue_wrap::prop::IsHeavy(obj))  p.physFlags |= coop::net::propspawn_flags::kIsHeavy;
        if (ue_wrap::prop::IsFrozen(obj)) p.physFlags |= coop::net::propspawn_flags::kFrozen;
        p.initLinVelX = p.initLinVelY = p.initLinVelZ = 0.f;
        p.initAngVelX = p.initAngVelY = p.initAngVelZ = 0.f;
        EnqueuePropSpawnForRetry(p);
        ++sent;
    }
    if (g_snapshotCandidateIdx >= g_snapshotCandidates.size()) {
        UE_LOGI("snapshot: enumeration drain complete (%zu candidates processed); freeing candidate list",
                g_snapshotCandidates.size());
        g_snapshotCandidates.clear();
        g_snapshotCandidates.shrink_to_fit();
        g_snapshotCandidateIdx = 0;
    } else {
        UE_LOGI("snapshot: drained chunk -- this tick=%d, processed %zu/%zu",
                sent, g_snapshotCandidateIdx, g_snapshotCandidates.size());
    }
}

void SendPropSnapshotForPeer() {
    if (g_session.role() != coop::net::Role::Host) {
        UE_LOGI("snapshot: not host -- skipping (client receives, doesn't broadcast)");
        return;
    }
    if (!g_session.connected()) {
        UE_LOGW("snapshot: not connected -- skipping");
        return;
    }
    EnumerateSnapshotCandidates();  // Phase 1: cheap walk + filter; Phase 2 happens per-tick via DrainSnapshotChunk
}

// Audit I-2 (2026-05-24): mirror PropSpawn's retry queue for PropDestroy.
// Without this, a destroy that races a busy reliable channel (e.g. a chat
// message in flight) leaves a phantom actor on the peer until the next
// snapshot bootstrap reconciles -- materially worse than a missing spawn
// because the phantom persists, is interactable, and pollutes the scene.
// Queue capacity 1024 -- destroys are rarer than spawns (only on
// consumption / breakage), and each entry is just a WireKey (32 B) so
// 1024 * 32 = 32 KB max queue memory.
std::mutex g_pendingDestroysMutex;
std::deque<coop::net::WireKey> g_pendingDestroys;
constexpr size_t kMaxPendingDestroys = 1024;

void EnqueuePropDestroyForRetry(const coop::net::WireKey& key) {
    std::lock_guard<std::mutex> lk(g_pendingDestroysMutex);
    if (g_pendingDestroys.size() >= kMaxPendingDestroys) {
        UE_LOGW("net: PropDestroy retry queue full (depth %zu / max %zu) -- dropping OLDEST",
                g_pendingDestroys.size(), kMaxPendingDestroys);
        g_pendingDestroys.pop_front();
    }
    g_pendingDestroys.push_back(key);
    UE_LOGI("net: PropDestroy enqueued for retry (depth now %zu)", g_pendingDestroys.size());
}

void DrainPendingPropDestroys() {
    std::lock_guard<std::mutex> lk(g_pendingDestroysMutex);
    while (!g_pendingDestroys.empty()) {
        const auto& key = g_pendingDestroys.front();
        if (!g_session.SendPropDestroy(key)) break;  // channel busy; retry next tick
        UE_LOGI("net: PropDestroy dequeued + sent (remaining=%zu)",
                g_pendingDestroys.size() - 1);
        g_pendingDestroys.pop_front();
    }
}

// Drain pending PropSpawns into the reliable channel. Called per tick from
// NetPumpTick. Stops at the first SendPropSpawn=false (reliable channel
// still busy) so order is preserved.
void DrainPendingPropSpawns() {
    std::lock_guard<std::mutex> lk(g_pendingSpawnsMutex);
    while (!g_pendingSpawns.empty()) {
        const auto& payload = g_pendingSpawns.front();
        if (!g_session.SendPropSpawn(payload)) {
            break;  // channel busy; retry next tick. preserves order.
        }
        UE_LOGI("net: PropSpawn dequeued + sent (remaining=%zu)",
                g_pendingSpawns.size() - 1);
        g_pendingSpawns.pop_front();
    }
}

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
    // seconds) doesn't drown the log. ATOMIC because ProcessEvent CAN dispatch
    // from a task-graph worker under parallel anim (per game_thread.h:91-92);
    // the relaxed fetch_add is cheap and the counter semantics tolerate it.
    static std::atomic<uint64_t> sCount{0};
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3 || (n % 30) == 0) {
        UE_LOGI("grab_hook[PHC.SetTarget]: handle=%p (call #%llu)", self,
                static_cast<unsigned long long>(n));
    }
}

void GrabObserver_PHC_SetTargetWithRotation(void* self, void* /*function*/, void* /*params*/) {
    static std::atomic<uint64_t> sCount{0};
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;
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

// --- Throw signal: UPrimitiveComponent::AddImpulse. Fires whenever the BP
// throws a held prop (throwHoldingProp BP-pure is inlined and calls this on
// the released component). Also fires for non-throw impulses (explosions,
// hit reactions) -- the host disambiguates via context (was a grab active
// right before this fired? then it's a throw).

void GrabObserver_PrimComp_AddImpulse(void* self, void* /*function*/, void* params) {
    // Diagnostic-only post-RE 2026-05-24: the release-edge in NetPumpTick
    // reads the body's inherited velocity directly via prop::GetPhysicsVelocity
    // (which captures any AddImpulse the engine just applied AS PART OF
    // that velocity). So we just log here -- no cross-thread cache to populate.
    if (!self || !params) return;
    // Param frame layout (verified by autonomous test):
    //   FVector impulse    @ 0   (12 bytes)
    //   FName   BoneName   @ 12  (8 bytes)
    //   bool    bVelChange @ 20  (1 byte) + pad
    const ue_wrap::FVector imp = *reinterpret_cast<ue_wrap::FVector*>(params);
    UE_LOGI("grab_hook[PrimComp.AddImpulse]: component=%p impulse=(%.1f, %.1f, %.1f) (diagnostic, not shipped)",
            self, imp.X, imp.Y, imp.Z);
}

// "Cheap insurance" PRE-observers for SetPhysicsLinearVelocity +
// SetPhysicsAngularVelocityInDegrees -- audit issue #3 / 2026-05-24. If BP
// explicitly calls these on a released prop (instead of relying on inherited
// PhysX tracking velocity), the param frame carries the LITERAL launch
// velocity we want -- the GetPhysicsVelocity read in NetPumpTick may then be
// reading post-step values. These observers DON'T cache cross-thread; they
// just log. The hands-on test of Bug B confirms via log whether BP uses
// SetPhysicsLinearVelocity explicitly. If it does, a future commit can switch
// the wire-capture to the observer (lock-free atomic cache, RELEASE/ACQUIRE
// fenced -- same shape as the retired g_lastImpulse* pattern).
//
// Param frame layout for both:
//   FVector NewVel/NewAngVel  @ 0   (12 bytes)
//   bool    bAddToCurrent     @ 12  (1 byte) + pad
//   FName   BoneName          @ 16  (8 bytes)
void GrabObserver_PrimComp_SetLinearVelocity_PRE(void* self, void* /*function*/, void* params) {
    if (!self || !params) return;
    const ue_wrap::FVector v = *reinterpret_cast<ue_wrap::FVector*>(params);
    UE_LOGI("grab_hook[PrimComp.SetPhysicsLinearVelocity PRE]: component=%p NewVel=(%.1f, %.1f, %.1f) (diagnostic)",
            self, v.X, v.Y, v.Z);
}

void GrabObserver_PrimComp_SetAngularVelocity_PRE(void* self, void* /*function*/, void* params) {
    if (!self || !params) return;
    const ue_wrap::FVector v = *reinterpret_cast<ue_wrap::FVector*>(params);
    UE_LOGI("grab_hook[PrimComp.SetPhysicsAngularVelocityInDegrees PRE]: component=%p NewAngVel=(%.1f, %.1f, %.1f) (diagnostic)",
            self, v.X, v.Y, v.Z);
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
    // Atomic for the same reason as PHC.SetTarget above.
    if (!self) return;
    static std::atomic<uint64_t> sCount{0};
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;
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

// --- v5 Inc2: Aprop_C::Init POST observer -- the universal spawn detector.
//
// Catches EVERY Aprop derivative spawn (mushroom growth, world-gen, inventory
// drops on the spawning peer, etc.) via the BP UserConstructionScript that
// runs inside FinishSpawningActor. HOST broadcasts PropSpawn for each;
// CLIENT skips (host-authoritative for world state; client's own inventory
// drops still broadcast separately via the takeObj observer).
//
// Echo suppression: when this peer's OnSpawn applies a wire-received
// PropSpawn, it calls MarkIncomingSpawn(actor) BEFORE FinishSpawningActor.
// This observer then sees ConsumeIncomingSpawn(actor) == true and skips
// the broadcast back to the sender (no ping-pong).
//
// Save-load is naturally gated by g_session.connected() == false at that
// time (save loads pre-handshake; connected becomes true only after the
// peer joins). No broadcast bandwidth wasted on save-load.
// v5 Phase 5N Stream B (2026-05-24): is this prop class an intermediate-
// variant that should NEVER be wire-synced? Host-authoritative growth
// pipeline -- the intermediate state (mushroom7_C growing) stays internal
// to host; only the mature variant (mushroom_C) is broadcast. Renamed
// from "ClientSuppressed" to "WireSuppressed" (audit naming clarification
// 2026-05-24): the predicate gates THREE sites symmetrically:
//   (1) HOST broadcast in Aprop_C::Init POST -> never send
//   (2) HOST broadcast in DrainSnapshotChunk -> never enqueue for snapshot
//   (3) CLIENT local spawn destruction in Aprop_C::Init POST
//   (4) CLIENT wire-drop in event_feed.cpp PropSpawn handler
// Extend allowlist as new intermediate-variant cases surface in RE.
bool IsWireSuppressedPropClass(const std::wstring& cls) {
    return cls == P::name::PropMushroomGrowingClass;
}

// 2026-05-25 audit fix (post-ship audit issue #1): the subclass-aware Init
// observer registers on multiple Init UFunctions in the prop_C lineage (base
// + each override). If a subclass Init calls super (BP "Parent: ..." node),
// BOTH the override AND the base observer fire for the same actor with
// different function pointers but identical self. Without dedupe, host
// double-broadcasts PropSpawn and client runs DestroyLocalProp twice.
//
// Same set also dedupes against a hypothetical "Init called multiple times"
// path (UE4 doesn't normally do that for a single actor, but defensive).
//
// Cap mirrors the remote_prop incoming-set pattern: bounded 256, clear on
// overflow (a one-shot stale-skip is harmless because by the time the cap
// trips, all listed actors have either been destroyed or stably broadcast).
// Game-thread-only access (observer dispatch fires on game thread for BP
// UFunctions like Init).
std::unordered_set<void*> g_processedInitActors;
constexpr size_t kProcessedInitCap = 256;

void MarkProcessedInit(void* actor) {
    if (!actor) return;
    if (g_processedInitActors.size() >= kProcessedInitCap) g_processedInitActors.clear();
    g_processedInitActors.insert(actor);
}

bool HasProcessedInit(void* actor) {
    return actor && g_processedInitActors.count(actor) > 0;
}

void UnmarkProcessedInit(void* actor) {
    if (!actor) return;
    g_processedInitActors.erase(actor);
}

// v5 Phase 5N Stream B: destroy a wire-suppressed intermediate-variant
// prop via K2_DestroyActor reflection. Idempotent class resolution cached.
//
// IMPORTANT (audit I-3 2026-05-24): when called from inside
// Aprop_C::Init POST observer, the engine is STILL EXECUTING
// FinishSpawningActor -- the caller BP graph (e.g. mushroomMaster_C::form)
// is about to read the FinishSpawningActor return value and call
// spawnedNaturally() on it. Calling K2_DestroyActor here marks the actor
// PendingKill BEFORE form() continues. PendingKill actors in UE4 are
// addressable but flagged for GC; calling spawnedNaturally on a
// PendingKill is technically safe (no segfault) but burns a UFunction
// dispatch on a doomed actor.
//
// To make destroys clean: caller passes `deferred=true` to schedule the
// destroy via GT::Post (next-frame on game thread) -- form()'s BP
// continuation finishes first, THEN we destroy. Adds one frame of
// "phantom" intermediate-variant on client but eliminates any
// PendingKill-on-live-BP risk.
//
// MarkIncomingDestroy is forward-correct (future host-side suppression
// extensions would need it); currently dead on client because the PRE
// observer is host-only-gated.
void DestroyLocalProp(void* actor, bool deferred) {
    if (!actor) return;
    auto doDestroy = [actor]() {
        static void* sActorCls = nullptr;
        static void* sDestroyFn = nullptr;
        if (!sActorCls || !R::IsLive(sActorCls)) {
            sActorCls = R::FindClass(P::name::ActorClassName);
            sDestroyFn = nullptr;
        }
        if (sActorCls && !sDestroyFn) {
            sDestroyFn = R::FindFunction(sActorCls, P::name::DestroyActorFn);
        }
        if (!sDestroyFn) {
            UE_LOGW("spawner-suppress: K2_DestroyActor UFunction unresolved -- cannot destroy local %p", actor);
            return;
        }
        if (!R::IsLive(actor)) {
            UE_LOGI("spawner-suppress: deferred destroy target %p no longer live (already destroyed elsewhere) -- skip",
                    actor);
            return;
        }
        // Mark BEFORE calling destroy so OUR PRE-observer skips broadcast
        // (forward-correct; currently dead on client since the observer is
        // host-only).
        coop::remote_prop::MarkIncomingDestroy(actor);
        R::CallFunction(actor, sDestroyFn, nullptr);
    };
    if (deferred) {
        GT::Post(doDestroy);
    } else {
        doDestroy();
    }
}

void GrabObserver_Aprop_Init_POST(void* self, void* /*function*/, void* /*params*/) {
    if (!self) return;
    // Gate order (audit L-1 2026-05-24): cheapest checks FIRST so the hot path
    // short-circuits with minimal work. atomic load -> field read -> hash
    // lookup -> SuperStruct chain. Save-load fires this observer ~2000 times
    // pre-handshake; the connected() short-circuit avoids the hash lookup on
    // every one of those.
    if (!g_session.connected()) return;                       // pre-handshake save-load -> skip
    if (coop::remote_prop::ConsumeIncomingSpawn(self)) {
        UE_LOGI("grab_hook[Aprop.Init POST]: actor %p was wire-received -- skip broadcast (echo suppression)",
                self);
        return;
    }
    if (!R::IsLive(self)) return;
    // Defensive: confirm it's actually an Aprop derivative (the hook was
    // installed on PropClass but a fast misregistration would surface here).
    if (!ue_wrap::prop::IsDescendantOfProp(self)) return;
    // 2026-05-25 audit dedupe: with the subclass-aware Init scan, multiple
    // observers are registered along the prop_C lineage. If a subclass Init
    // calls super (BP "Parent" node), we see TWO firings for the same actor
    // (override + base). Skip the duplicate. Cleared on K2_DestroyActor PRE
    // so address reuse after GC works correctly.
    if (HasProcessedInit(self)) {
        UE_LOGI("grab_hook[Aprop.Init POST]: actor %p already processed -- skip (super-call dedupe)",
                self);
        return;
    }
    MarkProcessedInit(self);

    // v5 Phase 5N Stream B (2026-05-24): host-authoritative client-side
    // suppression for intermediate-variant props (mushroom7_C, etc.). On
    // CLIENT, if this is a locally-spawned intermediate variant (NOT in
    // incoming-set -- already checked above), destroy it. The HOST keeps
    // running the growth pipeline; the eventual mature variant
    // (mushroom_C) broadcast arrives through normal Inc2 channels.
    const std::wstring cls = R::ClassNameOf(self);
    if (g_session.role() == coop::net::Role::Client) {
        if (IsWireSuppressedPropClass(cls)) {
            UE_LOGI("spawner-suppress[client.Init]: scheduling deferred destroy for local intermediate variant '%ls' actor=%p (host-authoritative; mature variant will arrive via wire)",
                    cls.c_str(), self);
            // Audit I-3 (2026-05-24): defer the destroy via GT::Post so
            // the engine's FinishSpawningActor + the calling BP graph
            // (mushroomMaster_C::form's spawnedNaturally call on the
            // returned actor) complete BEFORE we mark PendingKill.
            DestroyLocalProp(self, /*deferred=*/true);
            return;
        }
        // Client doesn't broadcast world spawns (host-authoritative).
        return;
    }

    // From here on: role == Host.
    //
    // Audit C-2 (2026-05-24): host MUST NOT broadcast intermediate-variant
    // classes. The receiver drops them anyway but the round-trip wastes
    // reliable-channel bandwidth (~10 ms per packet on stop-and-wait) per
    // growing-mushroom spawn. Host-authoritative invariant: the
    // intermediate state stays internal; only the mature variant
    // broadcasts when host's growth-timer transforms.
    if (IsWireSuppressedPropClass(cls)) {
        UE_LOGI("spawner-suppress[host.Init]: skipping broadcast for intermediate-variant '%ls' actor=%p (host-authoritative; will broadcast mature variant on transform)",
                cls.c_str(), self);
        return;
    }

    coop::net::PropSpawnPayload p{};
    // `cls` reused from the Stream-B suppression check above (cached once).
    p.className.len = 0;
    for (size_t i = 0; i < cls.size() && i < 63; ++i) {
        p.className.data[p.className.len++] = static_cast<char>(cls[i]);
    }
    const std::wstring keyStr = ue_wrap::prop::GetKeyString(self);
    if (keyStr.empty()) {
        UE_LOGI("grab_hook[Aprop.Init POST]: actor %p (class '%ls') has empty key -- skip (unkeyed = non-syncable)",
                self, cls.c_str());
        return;
    }
    p.key.len = 0;
    for (size_t i = 0; i < keyStr.size() && i < 31; ++i) {
        p.key.data[p.key.len++] = static_cast<char>(keyStr[i]);
    }
    const auto loc = ue_wrap::engine::GetActorLocation(self);
    const auto rot = ue_wrap::engine::GetActorRotation(self);
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
    p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
    p.scaleX = 1.f; p.scaleY = 1.f; p.scaleZ = 1.f;
    p.physFlags = coop::net::propspawn_flags::kSimulatePhysics;
    if (ue_wrap::prop::IsHeavy(self))  p.physFlags |= coop::net::propspawn_flags::kIsHeavy;
    if (ue_wrap::prop::IsFrozen(self)) p.physFlags |= coop::net::propspawn_flags::kFrozen;
    p.initLinVelX = p.initLinVelY = p.initLinVelZ = 0.f;
    p.initAngVelX = p.initAngVelY = p.initAngVelZ = 0.f;
    UE_LOGI("grab_hook[Aprop.Init POST]: HOST broadcasting SPAWN cls='%ls' key='%ls' loc=(%.1f,%.1f,%.1f) heavy=%d frozen=%d",
            cls.c_str(), keyStr.c_str(), p.locX, p.locY, p.locZ,
            (p.physFlags & coop::net::propspawn_flags::kIsHeavy)  ? 1 : 0,
            (p.physFlags & coop::net::propspawn_flags::kFrozen)   ? 1 : 0);
    if (!g_session.SendPropSpawn(p)) {
        EnqueuePropSpawnForRetry(p);
    }
}

// --- v5 Inc2: K2_DestroyActor PRE observer -- the universal destroy detector.
//
// Catches every BP-initiated AActor::K2_DestroyActor for Aprop derivatives
// (food eaten, container broken, mushroom harvested, hostile gibbed, etc.).
// HOST broadcasts PropDestroy; CLIENT skips. Echo-suppressed: receiver's
// OnDestroy marks the actor in g_incomingDestroys before calling
// K2_DestroyActor; this observer consumes the mark and skips broadcast.
//
// PRE timing: we read self->Key BEFORE the engine starts destruction. The
// actor is still fully addressable at this point (mark-PendingKill happens
// inside the K2_DestroyActor body).
void GrabObserver_Actor_K2DestroyActor_PRE(void* self, void* /*function*/, void* /*params*/) {
    if (!self) return;
    // 2026-05-25 audit cleanup: evict from the Init-processed dedupe set
    // BEFORE the role gate so client-side Stream B destroys (deferred
    // DestroyLocalProp on mushroom7_C) also clear their entries. Otherwise
    // the set could fill with stale pointers from destroyed actors,
    // triggering the cap-clear path more often than needed. Cheap
    // erase-if-present (O(1) hash lookup; no-op for non-prop actor
    // destroys that were never marked).
    UnmarkProcessedInit(self);
    // Gate order (audit I-1 2026-05-24): this observer is hooked on AActor
    // base -> fires for EVERY actor destroy in the game (NPCs, projectiles,
    // UI actors, world cleanup, etc.), not just props. Cheapest checks
    // FIRST to short-circuit before the expensive SuperStruct walk:
    //   atomic load -> field read -> hash lookup -> class chain walk.
    if (!g_session.connected()) return;
    if (g_session.role() != coop::net::Role::Host) return;
    if (coop::remote_prop::ConsumeIncomingDestroy(self)) {
        UE_LOGI("grab_hook[K2_DestroyActor PRE]: actor %p was wire-received destroy -- skip broadcast",
                self);
        return;
    }
    if (!ue_wrap::prop::IsDescendantOfProp(self)) return;     // skip non-prop destroys (NPCs etc -- their own phase)
    const std::wstring keyStr = ue_wrap::prop::GetKeyString(self);
    if (keyStr.empty()) return;                               // unkeyed = non-syncable
    coop::net::WireKey wk{};
    wk.len = 0;
    for (size_t i = 0; i < keyStr.size() && i < 31; ++i) {
        wk.data[wk.len++] = static_cast<char>(keyStr[i]);
    }
    UE_LOGI("grab_hook[K2_DestroyActor PRE]: HOST broadcasting DESTROY actor=%p key='%ls'",
            self, keyStr.c_str());
    if (!g_session.SendPropDestroy(wk)) {
        EnqueuePropDestroyForRetry(wk);
    }
}

// --- v5 Bug C: UpropInventory_C::takeObj POST observer. THE bottom call all
// inventory drop paths funnel through (per
// research/findings/votv-inventory-drop-spawn-RE-2026-05-24.md). The signature
// is `(int32 Index, bool removeVol, bool& return, Fstruct_save& Output,
// AActor*& Object)` -- POST observer reads the Object out-param to get the
// just-spawned actor, then builds a PropSpawnPayload from its class + Key +
// transform + Aprop_C bools. NO echo loop: the receiver's OnSpawn spawns via
// GameplayStatics direct (BeginDeferredActorSpawnFromClass), NOT through this
// takeObj path, so the observer never fires for receiver-applied spawns.
//
// Param offset for `Object` is resolved lazily (one FindParamOffset per
// observer install, cached in a function-local static atomic).
void GrabObserver_PropInventory_TakeObj_POST(void* self, void* function, void* params) {
    if (!self || !params || !function) return;
    // Cache Object out-param offset. Atomic because the observer can dispatch
    // on either the game thread (typical) or a task-graph worker (per
    // game_thread.h note); -2 sentinel means "unresolved", -1 = "tried and
    // failed, don't retry every call".
    static std::atomic<int32_t> sObjectOff{-2};
    int32_t off = sObjectOff.load(std::memory_order_acquire);
    if (off == -2) {
        const int32_t resolved = R::FindParamOffset(function, L"Object");
        sObjectOff.store(resolved >= 0 ? resolved : -1, std::memory_order_release);
        off = resolved >= 0 ? resolved : -1;
        UE_LOGI("grab_hook[takeObj POST]: resolved Object out-param offset = %d", resolved);
    }
    if (off < 0) return;
    void* spawnedActor = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(params) + off);
    if (!spawnedActor || !R::IsLive(spawnedActor)) return;
    // Only broadcast when connected -- otherwise we'd queue PropSpawn forever.
    if (!g_session.connected()) {
        UE_LOGI("grab_hook[takeObj POST]: spawned %p but session not connected -- skipping broadcast",
                spawnedActor);
        return;
    }
    // v5 Inc2: gate takeObj observer to CLIENT-only. On host, the same drop
    // is already captured by the Aprop_C::Init POST observer (which runs
    // inside FinishSpawningActor, fired BEFORE takeObj's POST body returns).
    // Without this gate, host would double-broadcast every inventory drop.
    if (g_session.role() != coop::net::Role::Client) {
        UE_LOGI("grab_hook[takeObj POST]: host-side drop -- skip (Aprop_C::Init observer handles host broadcasts)");
        return;
    }
    // Defensive: only broadcast for Aprop_C derivatives. If a non-prop class
    // somehow flows through takeObj (BP refactor regression), the receiver's
    // OnSpawn would still try to spawn it -- which is fine but wastes bytes.
    if (!ue_wrap::prop::IsDescendantOfProp(spawnedActor)) {
        UE_LOGW("grab_hook[takeObj POST]: spawned actor %p is NOT a prop_C derivative -- skipping",
                spawnedActor);
        return;
    }

    coop::net::PropSpawnPayload p{};
    // ClassName: actor's UClass leaf name (e.g. "Aprop_equipment_flashlight_C").
    const std::wstring cls = R::ClassNameOf(spawnedActor);
    p.className.len = 0;
    for (size_t i = 0; i < cls.size() && i < 63; ++i) {
        p.className.data[p.className.len++] = static_cast<char>(cls[i]);
    }
    // Key: Aprop_C.Key.ToString() via prop_wrap. After takeObj completes,
    // loadData has restored Key from Fstruct_save -- this is the stable
    // cross-peer identifier.
    const std::wstring keyStr = ue_wrap::prop::GetKeyString(spawnedActor);
    p.key.len = 0;
    for (size_t i = 0; i < keyStr.size() && i < 31; ++i) {
        p.key.data[p.key.len++] = static_cast<char>(keyStr[i]);
    }
    // Transform: spawned actor's world location + rotation, normalized to
    // FRotator axis canonical range so the receiver's FRotator->FQuat
    // conversion lands on the right hemisphere (matches PropPose pattern).
    const auto loc = ue_wrap::engine::GetActorLocation(spawnedActor);
    const auto rot = ue_wrap::engine::GetActorRotation(spawnedActor);
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
    p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
    // Scale: BP-spawned props typically have (1,1,1) -- send literal 1's.
    // (Reading GetActorScale3D via reflection would be a 4th UFunction call
    // for a value that's almost always 1; defer until a real non-1 case
    // surfaces.)
    p.scaleX = 1.f; p.scaleY = 1.f; p.scaleZ = 1.f;
    // Physics flags: inventory drops always spawn with SimulatePhysics on
    // (Aprop_C.Init enables it). The heavy + frozen bits are data-driven on
    // propData -- read both so the receiver can grab-path prime correctly.
    p.physFlags = coop::net::propspawn_flags::kSimulatePhysics;
    if (ue_wrap::prop::IsHeavy(spawnedActor))  p.physFlags |= coop::net::propspawn_flags::kIsHeavy;
    if (ue_wrap::prop::IsFrozen(spawnedActor)) p.physFlags |= coop::net::propspawn_flags::kFrozen;
    // Initial velocity: a just-spawned prop is at rest (Aprop_C.Init wakes
    // the body but doesn't apply velocity). Tap-drop / place-drop both
    // produce a stationary spawn that gravity then takes over.
    p.initLinVelX = p.initLinVelY = p.initLinVelZ = 0.f;
    p.initAngVelX = p.initAngVelY = p.initAngVelZ = 0.f;

    UE_LOGI("grab_hook[takeObj POST]: SPAWN broadcast cls='%ls' key='%ls' loc=(%.1f, %.1f, %.1f) heavy=%d frozen=%d",
            cls.c_str(), keyStr.c_str(), p.locX, p.locY, p.locZ,
            (p.physFlags & coop::net::propspawn_flags::kIsHeavy)  ? 1 : 0,
            (p.physFlags & coop::net::propspawn_flags::kFrozen)   ? 1 : 0);
    // Try direct send first; if the reliable channel is busy (chat or
    // PropRelease in flight), enqueue for NetPumpTick to drain (audit C-3).
    // Dropping silently would leave the peer with no actor for this Key
    // forever -- PropPose updates can't resolve a Key that never spawned.
    if (!g_session.SendPropSpawn(p)) {
        EnqueuePropSpawnForRetry(p);
    }
}

// v5 Inc2: track Aprop_C::Init + AActor::K2_DestroyActor observer install
// separately from the core grab observers (same reason as the takeObj
// observer): they depend on BP/engine classes that may not be loaded at
// first InstallGrabObservers call. NetPumpTick retries each tick until
// the classes appear.
//
// 2026-05-25 mushroom RE finding: prop subclasses (prop_food_mushroom_C,
// other overriders) define their OWN Init UFunction (separate UObject with
// separate address). Our game_thread detour does pointer-equality dispatch
// against the registered observer target. Registering ONLY on
// prop_C::Init means the observer never fires for any subclass instance
// whose Init is overridden -- Stream B suppression + Inc2 continuous
// broadcast both silently dead for mushrooms. Fix: walk GUObjectArray
// ONCE when prop_C first loads, find every UFunction named "Init" whose
// owning UClass is in the prop_C lineage, register the POST observer on
// each. The set tracks dedupe so a re-scan (currently not used; documented
// limitation below) doesn't double-register.
//
// Known limitation: BP subclasses that LOAD LATER (e.g. streamed-level
// content with own Init overrides) are missed by this one-shot scan. If
// hands-on testing surfaces a late-load miss, promote to a periodic
// rescan (cost: one GUObjectArray walk per N seconds; cheap when
// throttled).
// g_propInitScanDone (renamed 2026-05-25 from g_propInitObserverInstalled,
// audit #3): records that the one-shot subclass-aware Init UFunction scan
// has completed. The vector below records WHICH UFunctions were
// registered; this flag records WHETHER we ran the scan at all (so we
// don't re-walk GUObjectArray every NetPumpTick). Both states are
// necessary -- the flag could be derived from "vector non-empty + scan
// guard latched" but separating them keeps the gate semantics readable.
bool g_propInitScanDone             = false;
bool g_propDestroyObserverInstalled = false;
std::vector<void*> g_registeredPropInitFns;

void InstallPropLifecycleObserversIfReady() {
    if (!g_propInitScanDone) {
        // Gate: wait for prop_C base class to load. IsClassDescendantOfProp
        // returns false until then so the scan would yield zero registrations
        // -- no point doing the GUObjectArray walk before the gate's open.
        void* propBase = R::FindClass(P::name::PropClass);
        if (propBase) {
            // One-shot GUObjectArray scan. Cost profile: ~237k slots, each
            // with one ClassNameOf wstring allocation + compare with
            // L"Function" -- O(N) with N total UObjects, ~50-200 ms one-shot
            // single tick stall during world load. NOT a per-frame cost; runs
            // exactly once when prop_C first appears in the obj array.
            const std::wstring kInitName(P::name::PropInitFn);
            const int32_t n = R::NumObjects();
            int registered = 0;
            for (int32_t i = 0; i < n; ++i) {
                void* obj = R::ObjectAt(i);
                if (!obj) continue;
                // Cheap-first: class-name check (exactly L"Function" excludes
                // DelegateFunction / SparseDelegateFunction); then function-
                // name check; then SuperStruct walk for the prop lineage.
                if (R::ClassNameOf(obj) != L"Function") continue;
                if (R::ToString(R::NameOf(obj)) != kInitName) continue;
                void* owningCls = R::OuterOf(obj);
                if (!ue_wrap::prop::IsClassDescendantOfProp(owningCls)) continue;
                // Dedupe (linear search; expected pop is small ~3-10).
                bool already = false;
                for (void* fn : g_registeredPropInitFns) {
                    if (fn == obj) { already = true; break; }
                }
                if (already) continue;
                if (ue_wrap::game_thread::RegisterPostObserver(obj, GrabObserver_Aprop_Init_POST)) {
                    const std::wstring owner = R::ToString(R::NameOf(owningCls));
                    UE_LOGI("grab_hook: registered POST observer for %ls::Init @ %p (subclass-aware)",
                            owner.c_str(), obj);
                    g_registeredPropInitFns.push_back(obj);
                    ++registered;
                } else {
                    const std::wstring owner = R::ToString(R::NameOf(owningCls));
                    UE_LOGW("grab_hook: failed to register Init observer for %ls (observer table full)",
                            owner.c_str());
                }
            }
            UE_LOGI("grab_hook: subclass-aware Init scan: %d new registrations (total %zu Init UFunctions hooked across prop_C lineage)",
                    registered, g_registeredPropInitFns.size());
            // Mark installed once we've found AT LEAST the base prop_C::Init.
            // If the scan found zero, propBase resolved but the base Init
            // UFunction isn't in the obj array yet (very unlikely -- the
            // UClass and its UFunctions load together). Keep retrying next
            // tick to be safe.
            if (!g_registeredPropInitFns.empty()) {
                g_propInitScanDone = true;
            }
        }
    }
    if (!g_propDestroyObserverInstalled) {
        // K2_DestroyActor is on AActor base class -- always loaded by the
        // time we're past the initial class lookup.
        if (void* actorCls = R::FindClass(P::name::ActorClassName)) {
            if (void* fn = R::FindFunction(actorCls, P::name::DestroyActorFn)) {
                if (ue_wrap::game_thread::RegisterPreObserver(fn, GrabObserver_Actor_K2DestroyActor_PRE)) {
                    UE_LOGI("grab_hook: registered PRE observer for %ls.%ls @ %p (continuous destroy broadcast)",
                            P::name::ActorClassName, P::name::DestroyActorFn, fn);
                    g_propDestroyObserverInstalled = true;
                }
            } else {
                UE_LOGW("grab_hook: %ls.%ls UFunction not found -- destroy broadcast disabled",
                        P::name::ActorClassName, P::name::DestroyActorFn);
                g_propDestroyObserverInstalled = true;  // stop retry
            }
        }
    }
}

// Helper: install ONLY the propInventory_C::takeObj POST observer. Separate
// from InstallGrabObservers (audit C-2) so NetPumpTick can re-attempt this
// observer each tick if propInventory_C is loaded later than the core
// observers. Idempotent via g_inventoryObserverInstalled.
void InstallInventoryObserverIfReady() {
    if (g_inventoryObserverInstalled) return;
    void* invCls = R::FindClass(P::name::PropInventoryClass);
    if (!invCls) return;  // not loaded yet -- retry next tick
    void* fn = R::FindFunction(invCls, P::name::PropInventoryTakeObjFn);
    if (!fn) {
        // The class exists but the function doesn't -- this is a permanent
        // mismatch (class renamed or function moved). Log once and stop
        // retrying so we don't spam the log.
        UE_LOGW("grab_hook: %ls.%ls UFunction not found -- Bug C disabled permanently this session",
                P::name::PropInventoryClass, P::name::PropInventoryTakeObjFn);
        g_inventoryObserverInstalled = true;  // stop the retry loop
        return;
    }
    if (!ue_wrap::game_thread::RegisterPostObserver(fn, GrabObserver_PropInventory_TakeObj_POST)) {
        UE_LOGW("grab_hook: failed to register takeObj POST observer (table full?)");
        return;
    }
    UE_LOGI("grab_hook: registered POST observer for %ls.%ls @ %p (Bug C inventory drop ready)",
            P::name::PropInventoryClass, P::name::PropInventoryTakeObjFn, fn);
    g_inventoryObserverInstalled = true;
}

// One-shot UFunction-pointer resolution + observer install. Idempotent. Two
// classes involved: UPhysicsHandleComponent (engine, 5 observers) and
// mainPlayer_C (VOTV BP content, 3 observers). FindFunction does NOT climb
// to super, so each must be resolved from its own class. Unresolved
// UFunctions log a warning (BP recooks can renumber the K2Node ordinal in
// the InpActEvt name -- localized failure, others still install).
void InstallGrabObservers() {
    // Always attempt the lifecycle observers (audit C-2 pattern): independent
    // of the core observers so late-loading BP classes still get hooked.
    if (g_grabObserversInstalled) {
        InstallInventoryObserverIfReady();
        InstallPropLifecycleObserversIfReady();
        return;
    }

    void* phcCls = R::FindClass(P::name::PhysicsHandleComponentClass);
    void* pccCls = R::FindClass(P::name::PhysicsConstraintComponentClass);
    void* primCls = R::FindClass(P::name::PrimitiveComponentClass);
    void* playerCls = R::FindClass(P::name::MainPlayerClass);
    if (!phcCls || !pccCls || !primCls || !playerCls) {
        UE_LOGW("grab_hook: class not found yet (PHC=%p, PCC=%p, PrimComp=%p, mainPlayer=%p) -- will retry next tick",
                phcCls, pccCls, primCls, playerCls);
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

    // Throw signal: engine PrimitiveComponent.AddImpulse.
    reg(primCls, P::name::PrimitiveComponentClass,
        P::name::AddImpulseFn,               GrabObserver_PrimComp_AddImpulse,          /*pre=*/false);

    // Diagnostic-only velocity-set observers (audit issue #3, 2026-05-24).
    // Capture whether BP explicitly calls SetPhysics*Velocity on release.
    reg(primCls, P::name::PrimitiveComponentClass,
        P::name::SetPhysicsLinearVelocityFn,           GrabObserver_PrimComp_SetLinearVelocity_PRE,  /*pre=*/true);
    reg(primCls, P::name::PrimitiveComponentClass,
        P::name::SetPhysicsAngularVelocityInDegreesFn, GrabObserver_PrimComp_SetAngularVelocity_PRE, /*pre=*/true);

    // Secondary: BP-Timeline + input on mainPlayer_C.
    reg(playerCls, P::name::MainPlayerClass,
        P::name::MainPlayerUseInputEventFn,  GrabObserver_InpActEvt_use,      /*pre=*/false);
    reg(playerCls, P::name::MainPlayerClass,
        P::name::MainPlayerGrabUpdateFn,     GrabObserver_grab_Update,        /*pre=*/false);
    reg(playerCls, P::name::MainPlayerClass,
        P::name::MainPlayerGrabFinishedFn,   GrabObserver_grab_Finished_PRE,  /*pre=*/true);

    g_grabObserversInstalled = true;
    UE_LOGI("grab_hook: Stage 1+ core observers installed (5 PHC + 2 PCC + 1 PrimComp.AddImpulse + 2 PrimComp.SetVel + 3 BP-Timeline) -- press E on a prop to see hook lines");

    // v5 Bug C: try takeObj observer NOW; if propInventory_C isn't loaded
    // yet, NetPumpTick will retry each tick via this same helper.
    InstallInventoryObserverIfReady();
    // v5 Inc2: same for the universal Aprop_C::Init POST observer (continuous
    // spawn broadcast) and AActor::K2_DestroyActor PRE observer (destroy
    // broadcast).
    InstallPropLifecycleObserversIfReady();
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
    // PropResult: same shape as before, but populated by prop::FindNearest.
    // Keep the local struct (rather than direct prop::NearestResult) so the
    // heavy + nearest fields are co-located across two scan calls.
    struct PropResult {
        void* prop = nullptr; void* mesh = nullptr; float dist = 0.f; std::wstring cls;
        void* heavyProp = nullptr; void* heavyMesh = nullptr; float heavyDist = 0.f; std::wstring heavyCls;
        int totalHeavy = 0;
    };
    auto pr = std::make_shared<PropResult>();
    done->store(0);
    GT::Post([rsv, pr, done, haveAnchor, envAnchor] {
        const ue_wrap::FVector pLoc = haveAnchor ? envAnchor
                                                : ue_wrap::engine::GetActorLocation(rsv->player);
        UE_LOGI("grab_test: scan anchor = (%.0f, %.0f, %.0f) [%s]",
                pLoc.X, pLoc.Y, pLoc.Z, haveAnchor ? "env" : "player");
        // Light scan (any prop). Stage 2 helper handles class-walk +
        // CDO skip + heavy/static/frozen reads + Key resolve.
        ue_wrap::prop::ScanStats stats;
        const auto best = ue_wrap::prop::FindNearest(pLoc, /*wantHeavy=*/false, &stats);
        pr->prop = best.prop;
        pr->mesh = best.mesh;
        pr->dist = best.dist;
        pr->cls  = best.className;
        pr->totalHeavy = stats.totalHeavy;
        UE_LOGI("grab_test: prop scan scanned=%d candidates=%d totalHeavy=%d "
                "nearest=%p (%ls) dist=%.1f mesh=%p",
                stats.totalScanned, stats.candidates, stats.totalHeavy,
                pr->prop, pr->cls.c_str(), pr->dist, pr->mesh);
        if (pr->prop) {
            const auto key = ue_wrap::prop::GetKey(pr->prop);
            UE_LOGI("grab_test: prop fields -- heavy=%d Static=%d frozen=%d Key='%ls' (idx=%d num=%d)",
                    best.heavy ? 1 : 0, best.isStatic ? 1 : 0, best.isFrozen ? 1 : 0,
                    best.keyString.c_str(), key.ComparisonIndex, key.Number);
        }
        // Second scan: heavy-only. Same helper, different filter (one extra
        // ~237k UObject walk; the ChunkedFixedUObjectArray walk is cache-
        // friendly enough that two passes is fine for a one-shot diagnostic).
        const auto bestHeavy = ue_wrap::prop::FindNearest(pLoc, /*wantHeavy=*/true);
        pr->heavyProp = bestHeavy.prop;
        pr->heavyMesh = bestHeavy.mesh;
        pr->heavyDist = bestHeavy.dist;
        pr->heavyCls  = bestHeavy.className;
        if (pr->heavyProp) {
            UE_LOGI("grab_test: nearest HEAVY=%p (%ls) dist=%.1f mesh=%p",
                    pr->heavyProp, pr->heavyCls.c_str(), pr->heavyDist, pr->heavyMesh);
        } else {
            UE_LOGI("grab_test: NO heavy props in scene (totalHeavy=0) -- skip heavy-grab arm");
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

    // ---- 3. (HOST) Compute hand position (player + forward * 60 cm + Z+80 cm
    // so the prop sits at the camera's face level, IN FRAME for the screenshot).
    // The host's camera is at the head bone (~+80 cm above actor centre), so
    // putting the prop at actor.Z+80 + forward 60 cm puts it about an arm's
    // length in front of the camera centre.
    auto handPos = std::make_shared<ue_wrap::FVector>();
    done->store(0);
    GT::Post([rsv, pr, handPos, done] {
        ue_wrap::FVector pLoc = ue_wrap::engine::GetActorLocation(rsv->player);
        ue_wrap::FVector fwd  = ue_wrap::engine::GetActorForwardVector(rsv->player);
        handPos->X = pLoc.X + fwd.X * 60.f;
        handPos->Y = pLoc.Y + fwd.Y * 60.f;
        handPos->Z = pLoc.Z + 80.f;
        // Teleport the prop to the hand position (physics may fight back but
        // the kinematic-target will overwrite each tick).
        ue_wrap::engine::SetActorLocation(pr->prop, *handPos);
        UE_LOGI("grab_test: teleported prop -> (%.1f, %.1f, %.1f) (camera-level, in frame)",
                handPos->X, handPos->Y, handPos->Z);
        done->store(1);
    });
    while (done->load() == 0) ::Sleep(5);

    // ---- 4. Call grabHandle.GrabComponentAtLocation(propMesh, NAME_None, handPos).
    // ALSO write mainPlayer.grabbing_actor + grabbing_component directly so the
    // BP-state matches the native PHC state. A real BP-driven grab sets these
    // fields too; the NetPumpTick emit reads mainPlayer.grabbing_actor to
    // decide whether to send PropPose. Without these writes the autonomous
    // grab would never emit on the wire (only proves the local PhysX side).
    done->store(0);
    GT::Post([rsv, pr, handPos, done] {
        std::vector<uint8_t> frame(static_cast<size_t>(rsv->grabFrameSize), 0);
        *reinterpret_cast<void**>(frame.data() + rsv->grabPComp) = pr->mesh;
        *reinterpret_cast<R::FName*>(frame.data() + rsv->grabPBone) = R::FName{0, 0};
        *reinterpret_cast<ue_wrap::FVector*>(frame.data() + rsv->grabPLoc) = *handPos;
        const bool ok = R::CallFunction(rsv->grabHandle, rsv->grabFn, frame.data());
        // Mirror to BP state. mainPlayer.grabbing_actor + grabbing_component
        // are what NetPumpTick checks each frame to decide "host is holding
        // -> send PropPose". Real BP grabs set these in pickupObject (the
        // BP-pure function that VOTV's BP graph calls inline).
        *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(rsv->player) + P::off::mainPlayer_grabbing_actor) = pr->prop;
        *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(rsv->player) + P::off::mainPlayer_grabbing_component) = pr->mesh;
        UE_LOGI("grab_test: CallFunction(GrabComponentAtLocation) -> %d + wrote mainPlayer.grabbing_{actor,component}", ok);
        done->store(1);
    });
    while (done->load() == 0) ::Sleep(5);

    // ---- 5. Hold the prop while TILTING THE CAMERA UPWARD so the suitcase
    // visibly rises with the look direction (proves it's truly camera-attached,
    // not pinned to a static world location). Drive SetTargetLocation from
    // the LIVE camera location + forward each tick. Tilt the controller pitch
    // up gradually between screenshots so the comparison is unambiguous.
    // 30 ticks * 500ms = 15 sec hold -- long enough for lan-test to capture
    // BOTH cross-peer screenshot pairs deep inside the hold window.
    UE_LOGI("grab_test: holding + tilting camera up over 15 sec (2 screenshots: before-tilt vs after-tilt)");

    // Per-tick: read camera, optionally tilt pitch up, compute target from
    // CAMERA forward (not actor forward -- camera and actor differ in pitch),
    // call SetTargetLocation.
    auto driveTick = [&](float pitchDelta) {
        done->store(0);
        GT::Post([rsv, pitchDelta, done] {
            // Apply camera pitch delta on the controller.
            void* ctrl = ue_wrap::engine::GetController(rsv->player);
            if (ctrl && pitchDelta != 0.f) {
                ue_wrap::FRotator rot = ue_wrap::engine::GetControlRotation(ctrl);
                // GetControlRotation returns RAW unnormalized [0, 360): looking
                // 10 deg DOWN reads back as Pitch=350 not -10. Without this
                // NormalizeAxis the > 60 clamp fires on the first tick (350 +
                // any delta > 60) and snaps the camera instead of tilting
                // gradually -- invalidating the visual proof screenshots.
                // Audit 2026-05-24.
                rot.Pitch = ue_wrap::NormalizeAxis(rot.Pitch);
                rot.Pitch += pitchDelta;
                if (rot.Pitch >  60.f) rot.Pitch =  60.f;
                if (rot.Pitch < -60.f) rot.Pitch = -60.f;
                ue_wrap::engine::SetControlRotation(ctrl, rot);
            }
            // Read LIVE camera state (includes the pitch we just set; the
            // camera manager re-aligns to the controller's view rotation each
            // tick).
            const ue_wrap::FVector  camLoc = ue_wrap::engine::GetCameraLocation();
            const ue_wrap::FRotator camRot = ue_wrap::engine::GetCameraRotation();
            // FRotator -> forward FVector. UE4 convention:
            //   yaw   rotates around +Z (counter-clockwise from +X)
            //   pitch rotates around +Y (positive = looking up)
            //   X = cos(pitch) * cos(yaw)
            //   Y = cos(pitch) * sin(yaw)
            //   Z = sin(pitch)
            const float kDeg2Rad = 3.14159265358979323846f / 180.f;
            const float py = camRot.Pitch * kDeg2Rad;
            const float yw = camRot.Yaw   * kDeg2Rad;
            const float cp = std::cos(py), sp = std::sin(py);
            const float cy = std::cos(yw), sy = std::sin(yw);
            ue_wrap::FVector fwd{cp * cy, cp * sy, sp};
            ue_wrap::FVector target{camLoc.X + fwd.X * 60.f,
                                    camLoc.Y + fwd.Y * 60.f,
                                    camLoc.Z + fwd.Z * 60.f};
            std::vector<uint8_t> frame(static_cast<size_t>(rsv->stFrameSize), 0);
            *reinterpret_cast<ue_wrap::FVector*>(frame.data() + rsv->stPLoc) = target;
            R::CallFunction(rsv->grabHandle, rsv->setTargetFn, frame.data());
            done->store(1);
        });
        while (done->load() == 0) ::Sleep(5);
    };

    // Drive 30 ticks over 15 sec. First 8 ticks: no tilt (BEFORE state stable).
    // Ticks 9-20 (12 ticks * +3 deg = +36 deg): tilt pitch up. Last 10 ticks:
    // hold steady at tilted pose. Screenshots at i=6 (deep in BEFORE phase)
    // and i=26 (deep in AFTER phase) so lan-test cross-peer captures land
    // squarely inside each phase.
    for (int i = 0; i < 30; ++i) {
        ::Sleep(500);
        const float pitchDelta = (i >= 8 && i < 20) ? 3.f : 0.f;
        driveTick(pitchDelta);
        if (i == 6) {
            const bool ok = harness::screenshot::Capture(L"grab-before-tilt");
            UE_LOGI("grab_test: HOST SCREENSHOT 1 (before tilt, level camera) saved=%d", ok);
        }
        if (i == 26) {
            const bool ok = harness::screenshot::Capture(L"grab-after-tilt");
            UE_LOGI("grab_test: HOST SCREENSHOT 2 (after +36deg pitch tilt) saved=%d", ok);
        }
    }

    // ---- 6. Release + Throw arm: AddImpulse FIRST (caches the impulse via
    // the AddImpulse observer), then ReleaseComponent + clear grabbing_actor
    // in the SAME game-thread lambda so NetPumpTick can't observe the
    // intermediate state. Without this single-lambda atomicity, NetPumpTick
    // (running asynchronously at 60Hz) could detect the release-edge between
    // the prior AddImpulse and the clear -> sees grabbing_actor still set ->
    // no edge fired. Or worse: fires the edge BEFORE AddImpulse caches ->
    // throw path can't engage. Doing it all in one lambda guarantees the
    // post-state NetPumpTick observes is (impulse cached, grabbing_actor
    // cleared) -- the held->released edge fires WITH the impulse in cache.
    done->store(0);
    GT::Post([rsv, pr, done] {
        void* primCls = R::FindClass(P::name::PrimitiveComponentClass);
        void* addImpulseFn = R::FindFunction(primCls, P::name::AddImpulseFn);
        if (!addImpulseFn) { UE_LOGW("grab_test: AddImpulse UFunction not found"); done->store(2); return; }
        const int32_t frameSize = R::FunctionFrameSize(addImpulseFn);
        const int32_t pImp  = R::FindParamOffset(addImpulseFn, L"impulse");
        const int32_t pBone = R::FindParamOffset(addImpulseFn, L"BoneName");
        const int32_t pVel  = R::FindParamOffset(addImpulseFn, L"bVelChange");
        if (pImp < 0 || pBone < 0 || pVel < 0) {
            UE_LOGW("grab_test: AddImpulse param offsets missing (imp=%d bone=%d vel=%d)", pImp, pBone, pVel);
            done->store(2); return;
        }
        // (a) AddImpulse FIRST. Z=500 cm/s impulse on the prop's mesh while
        //     it is still kinematic (Phys grab still active) -- becomes part
        //     of the body's inherited velocity once we ReleaseComponent next
        //     and PhysX flips back to dynamic sim. The AddImpulse observer
        //     fires inline as a diagnostic log line (its values are NOT
        //     cached cross-tick; the release-edge in NetPumpTick reads the
        //     body's combined velocity via prop::GetPhysicsVelocity instead).
        std::vector<uint8_t> frame(static_cast<size_t>(frameSize), 0);
        *reinterpret_cast<ue_wrap::FVector*>(frame.data() + pImp) = ue_wrap::FVector{0.f, 0.f, 500.f};
        *reinterpret_cast<R::FName*>(frame.data() + pBone) = R::FName{0, 0};
        *reinterpret_cast<bool*>(frame.data() + pVel) = false;
        const bool impOk = R::CallFunction(pr->mesh, addImpulseFn, frame.data());
        // (b) ReleaseComponent (PhysX releases the kinematic constraint;
        //     fires PHC.Release PRE observer).
        const bool relOk = R::CallFunction(rsv->grabHandle, rsv->releaseFn, nullptr);
        // (c) Clear BP state -- the held->released edge NetPumpTick checks.
        //     By the time NetPumpTick runs next, grabbing_actor is null and
        //     the body has inherited the AddImpulse-derived velocity ->
        //     release-edge reads it via prop::GetPhysicsVelocity and ships
        //     it in SendPropRelease.
        *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(rsv->player) + P::off::mainPlayer_grabbing_actor) = nullptr;
        *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(rsv->player) + P::off::mainPlayer_grabbing_component) = nullptr;
        UE_LOGI("grab_test: AddImpulse=%d Release=%d (atomic in same game-thread tick)",
                impOk, relOk);
        done->store(1);
    });
    while (done->load() == 0) ::Sleep(5);

    // ---- 6c. POST-THROW: wait 700 ms for the prop to fly + start its arc,
    // then capture HOST screenshot. The lan-test framework captures a
    // matching CLIENT screenshot at the same wall-clock moment for the
    // cross-peer comparison (client should show its OWN local suitcase
    // flying via the v4 wire's AddImpulse). 700 ms = enough for the throw
    // arc to be visible but before the suitcase lands and stops moving.
    ::Sleep(700);
    {
        const bool ok = harness::screenshot::Capture(L"grab-post-throw");
        UE_LOGI("grab_test: HOST SCREENSHOT 3 (post-throw, mid-flight) saved=%d", ok);
    }

    // ---- 7. (HOST + heavy prop nearby) Heavy-grab arm: drive
    // UPhysicsConstraintComponent observers. Different class than PHC.
    if (pr->heavyProp && pr->heavyMesh) {
        UE_LOGI("grab_test: now driving HEAVY arm (PCC.SetConstrainedComponents + BreakConstraint)");
        struct HeavyResolved {
            void* heavyGrab = nullptr;
            void* pccCls = nullptr;
            void* setConstrainedFn = nullptr;
            void* breakFn = nullptr;
            int32_t scFrame = 0;
            int32_t scPComp1 = -1, scPBone1 = -1, scPComp2 = -1, scPBone2 = -1;
            bool ok = false;
        };
        auto hr = std::make_shared<HeavyResolved>();
        done->store(0);
        GT::Post([rsv, hr, done] {
            hr->heavyGrab = *reinterpret_cast<void**>(
                reinterpret_cast<uint8_t*>(rsv->player) + P::off::mainPlayer_heavyGrab);
            if (!hr->heavyGrab) { UE_LOGW("grab_test: mainPlayer.heavyGrab is null"); done->store(2); return; }
            hr->pccCls = R::FindClass(P::name::PhysicsConstraintComponentClass);
            hr->setConstrainedFn = R::FindFunction(hr->pccCls, P::name::SetConstrainedComponentsFn);
            hr->breakFn          = R::FindFunction(hr->pccCls, P::name::BreakConstraintFn);
            if (!hr->setConstrainedFn || !hr->breakFn) {
                UE_LOGW("grab_test: PCC UFunction lookup failed (set=%p break=%p)",
                        hr->setConstrainedFn, hr->breakFn);
                done->store(2); return;
            }
            hr->scFrame  = R::FunctionFrameSize(hr->setConstrainedFn);
            hr->scPComp1 = R::FindParamOffset(hr->setConstrainedFn, L"Component1");
            hr->scPBone1 = R::FindParamOffset(hr->setConstrainedFn, L"BoneName1");
            hr->scPComp2 = R::FindParamOffset(hr->setConstrainedFn, L"Component2");
            hr->scPBone2 = R::FindParamOffset(hr->setConstrainedFn, L"BoneName2");
            UE_LOGI("grab_test: PCC resolve heavyGrab=%p pccCls=%p setFn=%p frame=%d "
                    "(C1@%d B1@%d C2@%d B2@%d) breakFn=%p",
                    hr->heavyGrab, hr->pccCls, hr->setConstrainedFn, hr->scFrame,
                    hr->scPComp1, hr->scPBone1, hr->scPComp2, hr->scPBone2, hr->breakFn);
            hr->ok = (hr->scPComp1 >= 0 && hr->scPBone1 >= 0 && hr->scPComp2 >= 0 && hr->scPBone2 >= 0);
            done->store(hr->ok ? 1 : 2);
        });
        while (done->load() == 0) ::Sleep(5);
        if (hr->ok) {
            // Call heavyGrab.SetConstrainedComponents(nullptr, NAME_None,
            // heavyProp.mesh, NAME_None). nullptr Component1 means "constrain
            // Component2 to the constraint's own transform" which is the
            // simplest test -- we just want to verify the PCC observer fires.
            done->store(0);
            GT::Post([rsv, pr, hr, done] {
                std::vector<uint8_t> frame(static_cast<size_t>(hr->scFrame), 0);
                *reinterpret_cast<void**>(frame.data() + hr->scPComp1) = nullptr;
                *reinterpret_cast<R::FName*>(frame.data() + hr->scPBone1) = R::FName{0, 0};
                *reinterpret_cast<void**>(frame.data() + hr->scPComp2) = pr->heavyMesh;
                *reinterpret_cast<R::FName*>(frame.data() + hr->scPBone2) = R::FName{0, 0};
                const bool ok = R::CallFunction(hr->heavyGrab, hr->setConstrainedFn, frame.data());
                UE_LOGI("grab_test: CallFunction(PCC.SetConstrainedComponents) -> %d "
                        "(expect grab_hook[PCC.SetConstrainedComponents])", ok);
                done->store(1);
            });
            while (done->load() == 0) ::Sleep(5);

            ::Sleep(1000);

            done->store(0);
            GT::Post([hr, done] {
                const bool ok = R::CallFunction(hr->heavyGrab, hr->breakFn, nullptr);
                UE_LOGI("grab_test: CallFunction(PCC.BreakConstraint) -> %d "
                        "(expect grab_hook[PCC.BreakConstraint PRE])", ok);
                done->store(1);
            });
            while (done->load() == 0) ::Sleep(5);
        }
    } else {
        UE_LOGI("grab_test: no nearby heavy prop -- skipping heavy-grab arm");
    }

    // ---- 8. (HOST) Timeline-force arm: drive `grab` Timeline via
    // PlayFromStart so we trigger the 3 BP-Timeline observers without a
    // real E-press. The `grab` UTimelineComponent is at mainPlayer+0x0728;
    // its UpdateFunc/FinishedFunc bindings are auto-emitted by the BP
    // compiler and dispatched via OwningActor.ProcessEvent on each tick /
    // at end. RISK: VOTV's grab__UpdateFunc body may read state (e.g.
    // mainPlayer.grabbing_actor) that's null right now -- if not defensive,
    // could crash. Best to call once after a clean state (post-release).
    UE_LOGI("grab_test: trying to force `grab` Timeline play (closes the last 3 BP-Timeline observer gaps)");
    done->store(0);
    GT::Post([rsv, done] {
        void* timeline = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(rsv->player) + P::off::mainPlayer_grabTimeline);
        if (!timeline) {
            UE_LOGW("grab_test: mainPlayer.grab Timeline is null -- skipping force-play");
            done->store(2); return;
        }
        void* tlCls = R::FindClass(P::name::TimelineComponentClass);
        void* playFromStartFn = R::FindFunction(tlCls, P::name::TimelinePlayFromStartFn);
        if (!playFromStartFn) {
            UE_LOGW("grab_test: TimelineComponent.PlayFromStart UFunction not found");
            done->store(2); return;
        }
        UE_LOGI("grab_test: forcing grab Timeline=%p .PlayFromStart()", timeline);
        const bool ok = R::CallFunction(timeline, playFromStartFn, nullptr);
        UE_LOGI("grab_test: CallFunction(grab.PlayFromStart) -> %d "
                "(expect grab_hook[grab.Update] x3 + grab_hook[grab.Finished PRE])", ok);
        done->store(1);
    });
    while (done->load() == 0) ::Sleep(5);

    // Give the Timeline a few seconds to tick and finish.
    ::Sleep(3000);

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
    if (g_wasConnected && !isConnected) {
        // Peer-gone: drop the puppet AND restore physics on any prop the
        // receiver was kinematically driving (without this, a remote prop
        // mid-grab at disconnect would stay frozen in air forever -- the
        // 500 ms stream-stop timeout fires from inside remote_prop::Tick
        // but only if Tick keeps running; relying on it solely is fragile.
        // Audit fix 2026-05-24).
        if (g_orphan.valid()) g_orphan.Destroy();
        coop::remote_prop::ForceRelease();
        // Audit C-1 (2026-05-24): clear the pending-spawn queue + any
        // half-drained snapshot enumeration. Their contents belong to the
        // now-dead session; shipping them to the NEXT peer (which may be a
        // DIFFERENT machine after IP change) would carry stale or wrong-
        // peer state. A fresh snapshot enqueues on the next connected-edge.
        size_t droppedSpawns, droppedDestroys;
        {
            std::lock_guard<std::mutex> lk(g_pendingSpawnsMutex);
            droppedSpawns = g_pendingSpawns.size();
            g_pendingSpawns.clear();
        }
        {
            std::lock_guard<std::mutex> lk(g_pendingDestroysMutex);
            droppedDestroys = g_pendingDestroys.size();
            g_pendingDestroys.clear();
        }
        size_t snapPending = 0;
        if (!g_snapshotCandidates.empty()) {
            snapPending = g_snapshotCandidates.size() - g_snapshotCandidateIdx;
            g_snapshotCandidates.clear();
            g_snapshotCandidates.shrink_to_fit();
            g_snapshotCandidateIdx = 0;
        }
        // 2026-05-25 audit: clear the Init-processed dedupe set on
        // disconnect. Stale entries from a previous session would
        // incorrectly suppress legitimate spawns from the NEXT peer's
        // snapshot. (The registered observers stay; only the per-actor
        // state needs reset.)
        const size_t initProcessedDropped = g_processedInitActors.size();
        g_processedInitActors.clear();
        UE_LOGI("net: disconnected -- cleared %zu pending PropSpawn(s) + %zu pending PropDestroy(s) + %zu un-enumerated snapshot candidate(s) + %zu Init-processed entries",
                droppedSpawns, droppedDestroys, snapPending, initProcessedDropped);
    }
    // Phase 5S0 (snapshot bootstrap): on every false->true transition of
    // Connected, the host enumerates Aprop_C derivatives and broadcasts
    // each as a PropSpawn. Client receives + de-dupes (skip if local Key
    // already resolves, update transform to converge). Naturally re-fires
    // on every reconnect since g_wasConnected resets in the disconnect
    // branch above. NO trigger from client side (asymmetric: host pushes,
    // client receives).
    if (!g_wasConnected && isConnected) {
        UE_LOGI("net: connected edge -- triggering Phase 5S0 snapshot");
        SendPropSnapshotForPeer();
    }
    g_wasConnected = isConnected;

    // Per-tick snapshot work (Phase 5S0 audit I-2): process kSnapshotChunkSize
    // candidates per tick if a snapshot enumeration is in progress. Cheap
    // when no enumeration is active (no-op on empty vector). Splits the
    // GetActorLocation + Rotation cost across many frames instead of one.
    if (isConnected) DrainSnapshotChunk();

    // Drain any pending PropSpawn payloads that couldn't ship at observer
    // fire time because the reliable channel was busy (audit C-3). Cheap
    // when the queue is empty (mutex lock + size check). Order preserved.
    // Same pattern for PropDestroy (audit I-2 2026-05-24).
    if (isConnected) {
        DrainPendingPropSpawns();
        DrainPendingPropDestroys();
    }

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
        InstallGrabObservers();  // self-gates on g_grabObserversInstalled + retries inventory observer (audit C-2)
        coop::net::PoseSnapshot mine;
        if (ReadLocalPose(g_netLocal, g_netLocalController, mine)) g_session.SetLocalPose(mine);

        // v4: held-prop replication. Read mainPlayer.grabbing_actor; if non-
        // null, build a PropPoseSnapshot from the prop's current world transform
        // and publish to the net thread. On the edge held -> not-held, send a
        // RELIABLE PropRelease so the peer re-enables SimulatePhysics (and we
        // never rely solely on the 500 ms stream-stop timeout).
        // State is FILE-SCOPE (g_lastHeldProp/Key/g_propEmitCount) -- a
        // static-local would carry stale state across session restart.
        void* heldActor = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(g_netLocal) + P::off::mainPlayer_grabbing_actor);
        if (heldActor && R::IsLive(heldActor)) {
            const std::wstring keyW = ue_wrap::prop::GetKeyString(heldActor);
            coop::net::PropPoseSnapshot pp{};
            pp.key.len = 0;
            for (size_t i = 0; i < keyW.size() && i < 31; ++i) {
                // The save UUIDs are ASCII; this lossless narrowing is fine.
                pp.key.data[pp.key.len++] = static_cast<char>(keyW[i]);
            }
            const auto loc = ue_wrap::engine::GetActorLocation(heldActor);
            const auto rot = ue_wrap::engine::GetActorRotation(heldActor);
            pp.x = loc.X; pp.y = loc.Y; pp.z = loc.Z;
            // Normalize at the wire boundary: physics-prop rotation accumulates
            // through FQuat<->Euler conversions and can end up at Yaw=359.8 or
            // Pitch=-270; receiver's canonical (-180,180] guard would reject.
            // Mirrors PoseSnapshot's NormalizeAxis at ReadLocalPose (audit
            // 2026-05-24).
            pp.pitch = ue_wrap::NormalizeAxis(rot.Pitch);
            pp.yaw   = ue_wrap::NormalizeAxis(rot.Yaw);
            pp.roll  = ue_wrap::NormalizeAxis(rot.Roll);
            g_session.SetLocalPropPose(true, pp);
            // Throttled emit log: first 3 + every 60th, matches receiver
            // throttle so the two logs can be diff'd line-for-line.
            const uint64_t n = ++g_propEmitCount;
            if (n <= 3 || (n % 60) == 0) {
                UE_LOGI("net: PropPose emit #%llu -> world(%.1f, %.1f, %.1f) rot(%.1f, %.1f, %.1f) key.len=%d",
                        static_cast<unsigned long long>(n),
                        pp.x, pp.y, pp.z, pp.pitch, pp.yaw, pp.roll,
                        static_cast<int>(pp.key.len));
            }
            g_lastHeldProp = heldActor;
            g_lastHeldKey = pp.key;
        } else if (g_lastHeldProp) {
            // Edge: was holding, now not. Stop sending PropPose + tell peer.
            //
            // v5: read the body's CURRENT linear+angular velocity via
            // prop::GetPhysicsVelocity. By the time this branch runs, the
            // engine has already executed (on this tick): the BP graph
            // clearing grabbing_actor, the PHC.ReleaseComponent call, and
            // any post-release AddImpulse the BP issues. PhysX has not
            // stepped yet, so the body still carries the inherited
            // kinematic-tracking velocity (the "вжух" mouse-flick launch
            // energy) PLUS any impulse-derived velocity, summed into ONE
            // velocity. We forward both linear + angular so the receiver
            // can SetPhysicsLinearVelocity + SetPhysicsAngularVelocityInDegrees
            // for an identical launch.
            g_session.SetLocalPropPose(false, {});
            ue_wrap::prop::VelocityState vel{};
            if (R::IsLive(g_lastHeldProp)) {
                vel = ue_wrap::prop::GetPhysicsVelocity(g_lastHeldProp);
            }
            const float linMagSq = vel.linearCmS.X * vel.linearCmS.X +
                                   vel.linearCmS.Y * vel.linearCmS.Y +
                                   vel.linearCmS.Z * vel.linearCmS.Z;
            UE_LOGI("net: held -> released (vel.ok=%d linVel=(%.1f, %.1f, %.1f) |v|=%.1f cm/s angVel=(%.1f, %.1f, %.1f))",
                    vel.ok ? 1 : 0,
                    vel.linearCmS.X, vel.linearCmS.Y, vel.linearCmS.Z,
                    std::sqrt(linMagSq),
                    vel.angularDegS.X, vel.angularDegS.Y, vel.angularDegS.Z);
            g_session.SendPropRelease(g_lastHeldKey,
                                      vel.linearCmS.X, vel.linearCmS.Y, vel.linearCmS.Z,
                                      vel.angularDegS.X, vel.angularDegS.Y, vel.angularDegS.Z);
            g_lastHeldProp = nullptr;
            g_lastHeldKey = {};
        }
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

    // v4: receiver-side held-prop driver. Drains the latest PropPose from the
    // session and applies it (lookup-by-Key on first arrival, transform writes
    // thereafter). Stream-stop timeout (>500 ms) treated as implicit release.
    coop::remote_prop::Tick(g_session);

    // Surface session events (joins/disconnects) to the feed + send our Join.
    // Pass g_netLocal so remote_prop::OnRelease can call Aprop_C.thrown(player)
    // for the natural throw-sound dispatch (Path B in
    // research/findings/votv-throw-sound-path-2026-05-24.md).
    coop::event_feed::Update(g_session, &g_orphan, g_netLocal);

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
            // v4: reset held-prop edge state so a session restart doesn't
            // carry a stale prop pointer/key from the prior session (audit
            // fix 2026-05-24).
            g_lastHeldProp = nullptr;
            g_lastHeldKey = {};
            g_propEmitCount = 0;
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
                    InstallGrabObservers();  // self-gates on g_grabObserversInstalled + retries inventory observer (audit C-2)
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
        // Same reset as the play branch -- audit fe68c03 missed this site.
        g_lastHeldProp = nullptr;
        g_lastHeldKey = {};
        g_propEmitCount = 0;
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
