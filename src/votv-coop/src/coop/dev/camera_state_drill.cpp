// coop/dev/camera_state_drill.cpp -- see coop/dev/camera_state_drill.h.

#include "coop/dev/camera_state_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/interactables/camera_toggle_intent.h"
#include "coop/interactables/interactable_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "coop/world/event_fire_sync.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/appliance.h"  // CallAction: the player's use, dispatched
#include "ue_wrap/devices/laptop.h"
#include "ue_wrap/devices/security_camera.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::camera_state_drill {
namespace {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace D  = coop::director;
namespace sc = ue_wrap::security_camera;
namespace EF = coop::event_fire_sync;
using ue_wrap::FVector;

constexpr const wchar_t* kCameraPath  = L"/Game/objects/prop_camera_bad.prop_camera_bad_C";
constexpr const wchar_t* kCameraClass = L"prop_camera_bad_C";
constexpr const wchar_t* kControllerClass = L"grayEventController_C";
constexpr const wchar_t* kGrayEvent = L"graysforest";

constexpr float    kBesideCm       = 150.f;  // HOST: the camera stands this far ahead of the host's player
constexpr float    kBesideEventCm  = 200.f;  // HOST (host-event): and this far from the event's controller, 120 up
constexpr int      kEventCycles    = 4;      // HOST (host-event): how many times it switches the camera on
constexpr uint64_t kEveryMs        = 250;
constexpr uint64_t kEventBoundMs   = 60000;  // HOST (host-event): the event must put the camera off within this
constexpr uint64_t kFindBoundMs    = 120000; // CLIENT: world-ready to a camera in its world
constexpr uint64_t kStateBoundMs   = 30000;  // CLIENT: the host's bit must reach this copy within this
constexpr uint64_t kEventWatchMs   = 120000; // CLIENT (host-event): the camera must read on, then off, within this
constexpr float    kLaptopNearCm   = 500.f;  // CLIENT (light): nearer than this to the laptop, it does not walk
constexpr int      kStandpoints    = 8;
constexpr float    kStandRingCm    = 150.f;
constexpr float    kStandReachCm   = 60.f;
constexpr int      kFirstWalkDeadlineS = 400;
constexpr int      kNextWalkDeadlineS  = 120;
constexpr int      kMaxScanned     = 16;

enum class Arm : uint8_t { Off, HostToggle, ClientToggle, Light, HostEvent, Join };
Arm ArmOf() {
    static const Arm arm = [] {
        const std::string& v = coop::config::ResolveString(::coop::config_registry::rows::camera_drill);
        return v == "host-toggle" ? Arm::HostToggle : v == "client-toggle" ? Arm::ClientToggle
             : v == "light" ? Arm::Light : v == "host-event" ? Arm::HostEvent : v == "join" ? Arm::Join : Arm::Off;
    }();
    return arm;
}
const char* ArmName() {
    switch (ArmOf()) {
    case Arm::HostToggle:   return "host-toggle";
    case Arm::ClientToggle: return "client-toggle";
    case Arm::Light:        return "light";
    case Arm::HostEvent:    return "host-event";
    case Arm::Join:         return "join";
    default:                return "off";
    }
}

enum class HStep : uint8_t { Wait, Index, Act, Watch, Done };
enum class CStep : uint8_t { Ready, Find, Walk, Act, Watch, Done };
HStep g_hstep = HStep::Wait;
CStep g_cstep = CStep::Ready;
uint64_t g_nextMs = 0;
uint64_t g_stepMs = 0;
ue_wrap::CachedObjRef g_hostCam;  // HOST: the camera it spawned
ue_wrap::CachedObjRef g_cam;      // CLIENT: the camera it reads
bool g_hostBefore = false;        // HOST (client-toggle, light): the bit as it stood, to see the client's request land
bool g_expectOff = false;         // HOST (host-event): it switched the camera on and waits for the event to put it off
int  g_cycles = 0;                // HOST (host-event): the times it switched the camera on
bool g_sawOn = false;             // CLIENT (host-event): the camera has read on
int  g_session = 0;               // CLIENT: world-ready sessions this process has run, for the join arm's lines
std::vector<FVector> g_ring;      // CLIENT (light): standpoints about the laptop
size_t g_stand = 0;
std::shared_ptr<D::BackgroundWalk> g_walk;

bool Due() {
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextMs) return false;
    g_nextMs = now + kEveryMs;
    return true;
}
void Go(CStep s) { g_cstep = s; g_stepMs = ::GetTickCount64(); }
bool Expired(uint64_t bound) { return ::GetTickCount64() - g_stepMs > bound; }

void HostFail(const char* what) {
    UE_LOGW("[CAMERA-DRILL] %s FAIL host: %s", ArmName(), what);
    g_hstep = HStep::Done;
}
void HostAbandon(const char* why) {
    UE_LOGW("[CAMERA-DRILL] ABANDONED %s host: %s", ArmName(), why);
    g_hstep = HStep::Done;
}
void ClientFail(const char* what) {
    UE_LOGW("[CAMERA-DRILL] %s FAIL %s", ArmName(), what);
    g_cstep = CStep::Done;
}
void ClientAbandon(const char* why) {
    UE_LOGW("[CAMERA-DRILL] ABANDONED %s client: %s", ArmName(), why);
    g_cstep = CStep::Done;
}

bool ReadBit(void* cam, bool light, bool& on) { return light ? sc::ReadLight(cam, on) : sc::ReadActive(cam, on); }
bool IsLightArm() { return ArmOf() == Arm::Light; }

// ---- HOST ------------------------------------------------------------------------------------------------

// The first instance of the class by name, the default object left out; null when none.
void* FirstInstance(const wchar_t* className) {
    void* cls = ue_wrap::object_index::ClassByName(className);
    if (!cls) return nullptr;
    void* found = nullptr;
    ue_wrap::object_index::ForEachInstance(cls,
        [](void* ctx, void* obj, int32_t) {
            auto* out = static_cast<void**>(ctx);
            if (!*out && !R::NameStartsWith(R::NameOf(obj), L"Default__")) *out = obj;
        },
        &found);
    return found;
}

// Spawns the camera: ahead of the player, or beside the gray event's controller for host-event. False once the arm
// has ended (said).
bool SpawnCamera(void* me) {
    FVector origin{};
    if (ArmOf() == Arm::HostEvent) {
        void* controller = FirstInstance(kControllerClass);
        FVector at{};
        if (!controller || !E::TryGetActorLocation(controller, at)) {
            HostAbandon("this world holds no grayEventController_C to put the camera beside");
            return false;
        }
        origin = {at.X + kBesideEventCm, at.Y, at.Z + 120.f};
    } else {
        FVector at{};
        const FVector fwd = E::GetActorForwardVector(me);
        if (!E::TryGetActorLocation(me, at)) return true;  // not readable yet: asked again next tick
        origin = {at.X + fwd.X * kBesideCm, at.Y + fwd.Y * kBesideCm, at.Z + 30.f};
    }
    void* cls = ue_wrap::asset_load::LoadObjectByPath(kCameraPath);
    if (!cls) { HostAbandon("prop_camera_bad_C did not load"); return false; }
    const ue_wrap::FRotator rot{0.f, 0.f, 0.f};
    void* cam = E::BeginDeferredSpawn(cls, origin, rot);
    if (!cam || !E::FinishDeferredSpawn(cam, origin, rot)) { HostAbandon("the camera did not spawn"); return false; }
    g_hostCam.Set(cam);
    const std::wstring key = sc::KeyOf(cam);
    if (key.empty()) {
        HostFail("the camera carries no usable prop key, so no lane can name it");
        return false;
    }
    UE_LOGI("camera_state_drill: host spawned camera key=%ls at=%.0f,%.0f,%.0f", key.c_str(), origin.X, origin.Y,
            origin.Z);
    g_hstep = HStep::Index;
    return true;
}

// The host's own use of the camera: the verb a player's E runs.
bool Use(void* cam, void* me) { return ue_wrap::appliance::CallAction(cam, me, sc::kUseAction); }

void HostAct(void* cam, void* me) {
    bool before = false;
    sc::ReadActive(cam, before);
    switch (ArmOf()) {
    case Arm::HostToggle: {
        bool after = false;
        if (!Use(cam, me) || !sc::ReadActive(cam, after) || after == before) {
            HostFail("the host's own use of the camera did not toggle it");
            return;
        }
        UE_LOGI("camera_state_drill: host toggled key=%ls active %d -> %d", sc::KeyOf(cam).c_str(), before ? 1 : 0,
                after ? 1 : 0);
        g_hstep = HStep::Done;
        return;
    }
    case Arm::ClientToggle:
    case Arm::Light:
        // The client presses; the host only watches its own camera take the request.
        ReadBit(cam, IsLightArm(), g_hostBefore);
        UE_LOGI("camera_state_drill: host waits for the client's request on key=%ls (%s is %d)",
                sc::KeyOf(cam).c_str(), IsLightArm() ? "isLight" : "active", g_hostBefore ? 1 : 0);
        g_hstep = HStep::Watch;
        return;
    case Arm::HostEvent:
        if (!Use(cam, me)) { HostFail("the host's own use of the camera did not run"); return; }
        ++g_cycles;
        g_expectOff = true;
        g_stepMs = ::GetTickCount64();
        if (!EF::HostFire(EF::FireKind::RunEvent, kGrayEvent, L"None")) {
            HostFail("the dev fire of graysforest was refused");
            return;
        }
        UE_LOGI("camera_state_drill: host switched key=%ls on (1 of %d) and fired %ls", sc::KeyOf(cam).c_str(),
                kEventCycles, kGrayEvent);
        g_hstep = HStep::Watch;
        return;
    case Arm::Join: {
        bool active = false, light = false;
        if (!Use(cam, me) || !sc::WriteLight(cam, true) || !sc::CallUpd(cam) || !sc::ReadActive(cam, active) ||
            !sc::ReadLight(cam, light) || !active || !light) {
            HostFail("the host could not switch the camera's two bits on");
            return;
        }
        UE_LOGI("camera_state_drill: host READY toggled key=%ls active=1 isLight=1", sc::KeyOf(cam).c_str());
        g_hstep = HStep::Done;
        return;
    }
    default:
        g_hstep = HStep::Done;
        return;
    }
}

void HostWatch(void* cam, void* me) {
    if (ArmOf() == Arm::HostEvent) {
        bool on = false;
        if (!sc::ReadActive(cam, on)) return;
        if (g_expectOff && !on) {
            UE_LOGI("camera_state_drill: host: the event put key=%ls off (cycle %d of %d)", sc::KeyOf(cam).c_str(),
                    g_cycles, kEventCycles);
            g_expectOff = false;
            if (g_cycles >= kEventCycles) { g_hstep = HStep::Done; return; }
            if (!Use(cam, me)) { HostFail("the host's own use of the camera did not run"); return; }
            ++g_cycles;
            g_expectOff = true;
            g_stepMs = ::GetTickCount64();
            UE_LOGI("camera_state_drill: host switched key=%ls on (%d of %d)", sc::KeyOf(cam).c_str(), g_cycles,
                    kEventCycles);
        } else if (g_expectOff && Expired(kEventBoundMs)) {
            UE_LOGW("camera_state_drill: host: the event never put the camera off in %llu ms -- nothing to read off",
                    static_cast<unsigned long long>(kEventBoundMs));
            g_hstep = HStep::Done;
        }
        return;
    }
    // client-toggle, light: the request has landed once the bit differs from the one it began at.
    bool now = false;
    if (!ReadBit(cam, IsLightArm(), now) || now == g_hostBefore) return;
    UE_LOGI("camera_state_drill: host: the client's request reached key=%ls, %s %d -> %d", sc::KeyOf(cam).c_str(),
            IsLightArm() ? "isLight" : "active", g_hostBefore ? 1 : 0, now ? 1 : 0);
    g_hstep = HStep::Done;
}

void HostTick(coop::net::Session* s) {
    if (!s->running()) return;
    void* me = coop::players::Registry::Get().Local();
    if (!me) return;
    if (g_hstep == HStep::Wait) {
        // The live arms wait for a client's world; the join arm does not, so the camera is toggled before anyone joins.
        if (ArmOf() != Arm::Join && !s->AnyWorldReadyPeer()) return;
        float worldNow = 0.f;
        if (!E::TryGetWorldTimeSeconds(worldNow)) return;
        SpawnCamera(me);
        return;
    }
    if (g_hstep == HStep::Done || !Due()) return;
    void* cam = g_hostCam.Get();
    if (!cam) { HostAbandon("the camera died"); return; }
    if (g_hstep == HStep::Index) {
        // A change on a camera the lane does not index is counted, never sent: wait for the lane to name it.
        bool on = false;
        if (coop::interactable_sync::CameraKey(cam).empty() || !sc::ReadActive(cam, on)) return;
        UE_LOGI("camera_state_drill: host READY camera key=%ls (the camera lane indexes it)", sc::KeyOf(cam).c_str());
        g_hstep = HStep::Act;
    }
    if (g_hstep == HStep::Act) { HostAct(cam, me); return; }
    if (g_hstep == HStep::Watch) HostWatch(cam, me);
}

// ---- CLIENT ----------------------------------------------------------------------------------------------

struct Scan { void* found[kMaxScanned]; int n = 0; };

void Collect(void* ctx, void* obj, int32_t) {
    auto* sc2 = static_cast<Scan*>(ctx);
    if (sc2->n < kMaxScanned && !R::NameStartsWith(R::NameOf(obj), L"Default__")) sc2->found[sc2->n++] = obj;
}

// The nearest prop_camera_bad_C of this world that carries a usable key, and how many there are.
void* NearestCamera(const FVector& from, int& count) {
    count = 0;
    void* cls = ue_wrap::object_index::ClassByName(kCameraClass);
    if (!cls) return nullptr;
    Scan scan;
    ue_wrap::object_index::ForEachInstance(cls, &Collect, &scan);
    void* best = nullptr;
    float bestD2 = 0.f;
    for (int i = 0; i < scan.n; ++i) {
        FVector p{};
        if (sc::KeyOf(scan.found[i]).empty() || !E::TryGetActorLocation(scan.found[i], p)) continue;
        ++count;
        const float d2 = (p.X - from.X) * (p.X - from.X) + (p.Y - from.Y) * (p.Y - from.Y) +
                         (p.Z - from.Z) * (p.Z - from.Z);
        if (!best || d2 < bestD2) { best = scan.found[i]; bestD2 = d2; }
    }
    return best;
}

void Pass(void* cam, const char* what) {
    UE_LOGI("[CAMERA-DRILL] %s PASS %s key=%ls session=%d", ArmName(), what, sc::KeyOf(cam).c_str(), g_session);
    g_cstep = CStep::Done;
}

// The press the client makes: it must reach the host and leave this copy as it was.
bool PressHeld(void* cam, void* me) {
    const bool light = IsLightArm();
    bool before = false;
    if (!ReadBit(cam, light, before)) { ClientAbandon("the camera's bit did not read"); return false; }
    if (before) { ClientAbandon("the camera is already on, so a press would turn it off and be read as the host's"); return false; }
    const uint64_t sent = coop::camera_toggle_intent::SentCount();
    bool ran = false;
    if (light) {
        void* widget = sc::LaptopWidget();
        if (!widget || !sc::SetLaptopCam(widget, cam)) { ClientAbandon("this client's world holds no laptop interface widget"); return false; }
        ran = sc::CallLaptopLightClick(widget);
    } else {
        ran = ue_wrap::appliance::CallAction(cam, me, sc::kUseAction);
    }
    if (!ran) { ClientAbandon("the press could not be dispatched"); return false; }
    bool after = false;
    ReadBit(cam, light, after);
    if (coop::camera_toggle_intent::SentCount() <= sent) {
        ClientFail("the press ran here instead of reaching the host (no request was sent)");
        return false;
    }
    if (after != before) {
        ClientFail("the press moved this client's own copy before the host answered");
        return false;
    }
    UE_LOGI("camera_state_drill: client pressed key=%ls: refused here, request sent (%s still %d)", sc::KeyOf(cam).c_str(),
            light ? "isLight" : "active", after ? 1 : 0);
    return true;
}

void WatchCamera(void* cam) {
    bool active = false, light = false;
    if (!sc::ReadActive(cam, active) || !sc::ReadLight(cam, light)) { ClientAbandon("the camera's bits did not read"); return; }
    switch (ArmOf()) {
    case Arm::HostToggle:
        if (active) { Pass(cam, "the host's toggle reached this copy: active=1"); return; }
        if (Expired(kStateBoundMs)) ClientFail("the host's toggle never reached this copy (active is still 0)");
        return;
    case Arm::ClientToggle:
        if (active) { Pass(cam, "refused here, performed by the host, read back: active=1"); return; }
        if (Expired(kStateBoundMs)) ClientFail("the host's answer never came (active is still 0)");
        return;
    case Arm::Light:
        if (light) { Pass(cam, "refused here, performed by the host, read back: isLight=1"); return; }
        if (Expired(kStateBoundMs)) ClientFail("the host's answer never came (isLight is still 0)");
        return;
    case Arm::HostEvent:
        if (!g_sawOn && active) {
            g_sawOn = true;
            UE_LOGI("camera_state_drill: client reads key=%ls on", sc::KeyOf(cam).c_str());
        } else if (g_sawOn && !active) {
            Pass(cam, "read on, then off, the host having only switched it on");
            return;
        }
        if (Expired(kEventWatchMs))
            ClientFail(g_sawOn ? "the camera read on and never off (the event's off never reached this copy)"
                               : "the camera never read on (the host's switch-on never reached this copy)");
        return;
    case Arm::Join:
        if (active && light) { Pass(cam, "read both bits on: active=1 isLight=1"); return; }
        if (Expired(kStateBoundMs)) {
            char what[96];
            std::snprintf(what, sizeof what, "the joiner reads active=%d isLight=%d, not both on", active ? 1 : 0, light ? 1 : 0);
            ClientFail(what);
        }
        return;
    default:
        g_cstep = CStep::Done;
        return;
    }
}

void ClientTick() {
    void* me = coop::players::Registry::Get().Local();
    if (!me) return;
    switch (g_cstep) {
    case CStep::Ready:
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        ++g_session;
        Go(CStep::Find);
        return;
    case CStep::Find: {
        FVector at{};
        if (!E::TryGetActorLocation(me, at)) return;
        int count = 0;
        void* cam = NearestCamera(at, count);
        // The press arms need this client's own lane to index the camera: its key is what the request names.
        const bool presses = ArmOf() == Arm::ClientToggle || IsLightArm();
        if (!cam || (presses && coop::interactable_sync::CameraKey(cam).empty())) {
            if (Expired(kFindBoundMs)) ClientFail("no camera of the lane ever stood in this world");
            return;
        }
        g_cam.Set(cam);
        bool active = false, light = false;
        sc::ReadActive(cam, active);
        sc::ReadLight(cam, light);
        UE_LOGI("camera_state_drill: client sees camera key=%ls active=%d isLight=%d (%d camera(s))",
                sc::KeyOf(cam).c_str(), active ? 1 : 0, light ? 1 : 0, count);
        if (!IsLightArm()) { Go(presses ? CStep::Act : CStep::Watch); return; }
        // The light button is the laptop's: the host judges the sender's reach of a device showing its interface; this
        // client walks to the base laptop, one of them.
        ue_wrap::laptop::EnsureResolved();
        void* laptop = ue_wrap::laptop::Instance();
        FVector lap{};
        if (!laptop || !E::TryGetActorLocation(laptop, lap)) { ClientAbandon("this world has no base laptop"); return; }
        const float dx = lap.X - at.X, dy = lap.Y - at.Y, dz = lap.Z - at.Z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) <= kLaptopNearCm) { Go(CStep::Act); return; }
        g_ring = D::ReachableStandpoints(me, lap, kStandRingCm, kStandpoints, kStandReachCm);
        UE_LOGI("camera_state_drill: client: a route reaches %zu of the %d standpoints about the laptop", g_ring.size(),
                kStandpoints);
        if (g_ring.empty()) { ClientAbandon("no NavMesh route reaches a standpoint about the laptop"); return; }
        g_stand = 0;
        g_walk = D::StartBackgroundWalk(g_ring[0], kStandReachCm, kFirstWalkDeadlineS);
        Go(CStep::Walk);
        return;
    }
    case CStep::Walk: {
        const int state = g_walk ? g_walk->state.load() : 2;
        if (state == 0) return;
        if (state == 2) {
            if (++g_stand >= g_ring.size()) { ClientAbandon("no standpoint about the laptop was reached"); return; }
            g_walk = D::StartBackgroundWalk(g_ring[g_stand], kStandReachCm, kNextWalkDeadlineS);
            return;
        }
        UE_LOGI("camera_state_drill: client: walked to standpoint %zu beside the laptop", g_stand + 1);
        Go(CStep::Act);
        return;
    }
    case CStep::Act: {
        void* cam = g_cam.Get();
        if (!cam) { ClientAbandon("the camera died"); return; }
        if (!PressHeld(cam, me)) return;
        Go(CStep::Watch);
        return;
    }
    case CStep::Watch: {
        void* cam = g_cam.Get();
        if (!cam) { ClientAbandon("the camera died"); return; }
        WatchCamera(cam);
        return;
    }
    default:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (ArmOf() == Arm::Off || !s) return;
    if (s->role() == coop::net::Role::Host) {
        HostTick(s);
        return;
    }
    if (!s->connected() || g_cstep == CStep::Done) return;
    if (g_cstep == CStep::Ready || Due()) ClientTick();
}

void OnDisconnect() {
    if (ArmOf() == Arm::Off) return;
    if (g_walk && g_walk->state.load() == 0) D::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
    g_ring.clear();
    g_cstep = CStep::Ready;
    g_sawOn = false;
    g_cam.Reset();
    g_stepMs = g_nextMs = 0;
    // The host's camera outlives a client, so a rejoin's client finds it again; one that is gone is spawned afresh.
    if (!g_hostCam.Get()) {
        g_hstep = HStep::Wait;
        g_expectOff = false;
        g_cycles = 0;
    }
}

}  // namespace coop::dev::camera_state_drill
