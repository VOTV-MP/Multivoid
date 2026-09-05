// coop/dev/atv_probe.cpp -- the ATV probe: a per-ATV sample line every half second (the game's
// matched four-body read, velocities, the suspension gate, vitals) and, when armed, a drive arm
// that seats the local player through the game's own verb, releases the handbrake and pulses
// the throttle until enough driven time is banked. See the header.

#include "coop/dev/atv_probe.h"
#include "coop/dev/atv_tire_probe.h"

#include "coop/config/config.h"
#include "coop/element/object_scan_hub.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "coop/interactables/atv_condition_sync.h"
#include "coop/interactables/atv_corrector.h"
#include "coop/interactables/atv_hit_guard.h"
#include "coop/interactables/atv_sync.h"   // OwnsTick -- which SIDE of the mirror this sample is
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::atv_probe {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

bool g_enabled   = false;
bool g_sitArmed  = false;
bool g_checked   = false;
bool g_installed = false;
std::chrono::steady_clock::time_point g_firstValid{};

// The arm waits this long after the first sample whose body is placed, not after boot: the ATV
// sits in GUObjectArray at the origin for many samples before its components exist.
constexpr int kSitDelayMs = 25000;
// How much cumulative driven time the arm wants. Two peers agree to a fraction of a centimetre
// at rest, so a seated-but-stationary ATV re-measures a case that passes; the corrector is
// under load only while the rig moves. Cumulative, not contiguous: a base full of geometry is
// not a test track, and a full-throttle run hits something within seconds and ragdolls the
// driver out, so the arm re-seats and counts only the driven time.
constexpr int kDriveMs = 20000;
// Full throttle saturates torqAlpha in about a second, and the rig then travels fast enough that
// the next thing it meets ends the run; pulsing keeps it moving, which is all the corrector
// needs.
constexpr int kPulseOnMs  = 250;
constexpr int kPulseOffMs = 750;
// A crash costs a re-seat; capped so a rig wedged against a wall cannot loop for good.
constexpr int kMaxReseats = 6;
// The seat verb is refused, not queued, while the player is mid-fall, so a refusal is worth a
// couple of retries.
constexpr int kSitRetries    = 3;
constexpr int kSitRetryMs    = 2000;

// The drive arm. ATV_C::playerSit is not the seat path; the seat verb is actionName(player,
// hit, name) with name "sit", gated three deep: the player's fall velocity, empty hands
// (checkEquip) and an unoccupied seat overlap. The seat body attaches and teleports the player
// onto the seat, possesses the ATV and sets isDriven, so it needs no proximity, which is why
// the arm may call it from wherever the player spawned. Torque then needs isDriven and a
// non-zero torqAlpha, whose producer bails whole on empty, brake, brokenn, underwater or a dead
// battery. A parked ATV sits on its handbrake, so the arm releases it through the game's own
// setBrake; brake and input_forward are the fields the game's own key handlers write, so this
// is the input path.
enum class Arm { Wait, Sit, Drive, Done };
Arm g_arm = Arm::Wait;
int  g_sitTries = 0;
std::chrono::steady_clock::time_point g_lastSitTry{};
std::chrono::steady_clock::time_point g_driveStart{};
std::chrono::steady_clock::time_point g_lastDriveSample{};
long long g_drivenMs = 0;     // cumulative time the rig was ACTUALLY driven
int  g_reseats   = 0;
bool g_pulseOn   = false;
std::chrono::steady_clock::time_point g_pulseEdge{};
// A run ends when its evidence is collected: once the arm is done and a settle window has
// passed, one DONE line is printed, which mp.py's --done-marker ends the run on.
constexpr int kDoneSettleMs = 30000;
std::chrono::steady_clock::time_point g_armDoneAt{};
bool g_doneAnnounced = false;
void* g_armAtv = nullptr;

void* g_actionNameFn = nullptr;  // ATV_C::actionName(player, hit, name)
void* g_dismountFn   = nullptr;  // ATV_C::dismount()
void* g_setBrakeFn    = nullptr;  // ATV_C::setBrake()

struct BoolField { int32_t off = -1; uint8_t mask = 0; };
BoolField g_bInputFwd, g_bBrake, g_bEmpty, g_bBroken, g_bUnderwater;
int32_t g_torqOff = -1;
int32_t g_speedOff = -1;

bool ReadBool(void* obj, const BoolField& f) {
    if (!obj || f.off < 0 || !f.mask) return false;
    return (*(reinterpret_cast<uint8_t*>(obj) + f.off) & f.mask) != 0;
}

bool WriteBool(void* obj, const BoolField& f, bool v) {
    if (!obj || f.off < 0 || !f.mask) return false;
    uint8_t* byte = reinterpret_cast<uint8_t*>(obj) + f.off;
    if (v) *byte |= f.mask; else *byte &= static_cast<uint8_t>(~f.mask);
    return true;
}

// Field offsets the ue_wrap ATV surface does not expose, resolved by name.
int32_t g_batteryOff = -1;
int32_t g_dirtOff    = -1;
int32_t g_dirtVelOff = -1;
// The value the tick gates its suspension on: Array_Contains(wheelsOnSurface, ...) guards the
// up-force and picks the mass scale on the wheel roots, and the array is written from inside
// the wheel hit delegates, the ones atv_hit_guard cancels on every non-owner. A mirror whose
// count is zero while its author's is not is a rig whose suspension we switched off.
int32_t g_wheelsOnSurfaceOff = -1;   // TArray: Num is the int32 at +8
int32_t g_airtimeOff         = -1;
int32_t g_tirescountOff      = -1;
bool    g_offsResolved = false;

// The values, not the length: wheelsOnSurface is a TArray<bool> whose default already has four
// elements, so its Num is 4 by construction, and the tick's gate tests the four values. A
// TArray<bool> stores one byte per element. Returns a 4-bit mask, or -1 for unreadable, which
// must stay distinguishable from 0.
int32_t WheelsOnSurfaceMask(void* obj, int32_t off) {
    if (!obj || off < 0) return -1;
    uint8_t* arr = reinterpret_cast<uint8_t*>(obj) + off;
    void* data = *reinterpret_cast<void**>(arr);
    const int32_t num = *reinterpret_cast<int32_t*>(arr + 8);
    if (!data || num < 1 || num > 64) return -1;   // torn/garbage read, not a finding
    int32_t mask = 0;
    const int32_t n = num < 4 ? num : 4;
    for (int32_t i = 0; i < n; ++i)
        if (reinterpret_cast<uint8_t*>(data)[i]) mask |= (1 << i);
    return mask;
}

// tirescount is an IntProperty; read as a float it type-puns 4 into a value that prints as a
// plausible 0.0.
int32_t ReadInt(void* obj, int32_t off) {
    if (!obj || off < 0) return -1;
    return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
}

void* g_partsFn = nullptr;   // ATV_C::vehicleGetParts (8 out-params, no in-params)

// The hub hands matches into `g_pending`; `g_atvs` is the completed swap.
std::vector<void*> g_pending;
std::vector<void*> g_atvs;

std::chrono::steady_clock::time_point g_lastSample{};
uint32_t g_sample = 0;

constexpr int kSampleMs = 500;
// The counters have to reach a log the archive has: atv_sync prints the hit-guard and corrector
// tallies once, from OnDisconnect, and every autonomous scenario kills both peers rather than
// disconnecting. Three relaxed loads every 10 s.
constexpr int kCounterLogMs = 10000;
std::chrono::steady_clock::time_point g_lastCounterLog{};

bool ResolveOffsets() {
    if (g_offsResolved) return true;
    void* cls = R::FindClass(L"ATV_C");
    if (!cls) return false;
    g_batteryOff = R::FindPropertyOffset(cls, L"battery");
    g_dirtOff    = R::FindPropertyOffset(cls, L"dirt");
    g_dirtVelOff = R::FindPropertyOffset(cls, L"dirtVel");
    g_wheelsOnSurfaceOff = R::FindPropertyOffset(cls, L"wheelsOnSurface");
    g_airtimeOff         = R::FindPropertyOffset(cls, L"airtime");
    g_tirescountOff      = R::FindPropertyOffset(cls, L"tirescount");
    g_partsFn    = R::FindFunction(cls, L"vehicleGetParts");
    g_torqOff    = R::FindPropertyOffset(cls, L"torqAlpha");
    g_speedOff   = R::FindPropertyOffset(cls, L"speed");

    // The arm's own surface, resolved whether or not the arm is armed, so a run whose arm never
    // fires still records which of these was missing.
    g_actionNameFn = R::FindFunction(cls, L"actionName");
    g_dismountFn   = R::FindFunction(cls, L"dismount");
    g_setBrakeFn   = R::FindFunction(cls, L"setBrake");
    R::FindBoolProperty(cls, L"input_forward", g_bInputFwd.off,    g_bInputFwd.mask);
    R::FindBoolProperty(cls, L"brake",         g_bBrake.off,       g_bBrake.mask);
    R::FindBoolProperty(cls, L"empty",         g_bEmpty.off,       g_bEmpty.mask);
    R::FindBoolProperty(cls, L"brokenn",       g_bBroken.off,      g_bBroken.mask);
    R::FindBoolProperty(cls, L"underwater",    g_bUnderwater.off,  g_bUnderwater.mask);

    g_offsResolved = true;
    UE_LOGI("atv_probe: resolved battery=%d dirt=%d dirtVel=%d torqAlpha=%d speed=%d "
            "wheelsOnSurface=%d airtime=%d tirescount=%d "
            "vehicleGetParts=%p actionName=%p dismount=%p setBrake=%p "
            "input_forward=%d/%02x brake=%d/%02x empty=%d broken=%d underwater=%d",
            g_batteryOff, g_dirtOff, g_dirtVelOff, g_torqOff, g_speedOff,
            g_wheelsOnSurfaceOff, g_airtimeOff, g_tirescountOff,
            g_partsFn, g_actionNameFn, g_dismountFn, g_setBrakeFn,
            g_bInputFwd.off, g_bInputFwd.mask, g_bBrake.off, g_bBrake.mask,
            g_bEmpty.off, g_bBroken.off, g_bUnderwater.off);
    return true;
}

// Can the whole rig be written, or only its root? Every write to a mirrored ATV addresses one
// of its five bodies: the game's own teleportVehicle re-places the wheels, but the velocity
// write reaches the root component only, and the four wheels are separate components. A
// rig-wide write needs the component pointers, and this census answers, once, from a live ATV,
// which of the rig's component properties exist, which are non-null on an instance and what
// class each is. The names are the components the hit delegates are bound to, plus
// car1_frontWheel_L, which appears in no delegate name and may not exist; a miss on it is data.
const wchar_t* const kRigComponentNames[] = {
    L"mesh",
    L"car1_Capsule",
    L"car1_frontWheel_R",
    L"car1_frontWheel_L",
    L"car1_frontWheelRoot",
    L"car1_backWheel_R",
    L"car1_backWheel_L",
    L"car1_backWheelRoot",
};

bool g_rigCensusDone = false;

void CensusRigComponents(void* atv) {
    if (g_rigCensusDone || !atv) return;
    g_rigCensusDone = true;
    void* cls = R::ClassOf(atv);
    if (!cls) { UE_LOGW("[ATVP] rig census: ClassOf(atv) null"); return; }
    for (const wchar_t* nm : kRigComponentNames) {
        const int32_t off = R::FindPropertyOffset(cls, nm);
        if (off < 0) {
            UE_LOGI("[ATVP] rig component '%ls': NOT A PROPERTY on this class", nm);
            continue;
        }
        void* comp = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(atv) + off);
        if (!comp) {
            UE_LOGI("[ATVP] rig component '%ls': off=0x%X but NULL on this instance", nm, off);
            continue;
        }
        UE_LOGI("[ATVP] rig component '%ls': off=0x%X ptr=%p class='%ls'",
                nm, off, comp, R::ClassNameOf(comp).c_str());
    }
}

float ReadFloat(void* obj, int32_t off) {
    if (!obj || off < 0) return -1.f;
    return *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + off);
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The five terms that skip the torque block whole, plus what the throttle produced. Logged every
// sample while driving, so a run in which the ATV never moves names its own reason.
void LogGates(void* atv, const char* phase) {
    UE_LOGI("[ATVP] ARM %s: driven=%d empty=%d brake=%d broken=%d underwater=%d batt=%.2f "
            "fwd=%d torq=%.3f speed=%.2f",
            phase, ue_wrap::atv::IsDriven(atv) ? 1 : 0,
            ReadBool(atv, g_bEmpty) ? 1 : 0, ReadBool(atv, g_bBrake) ? 1 : 0,
            ReadBool(atv, g_bBroken) ? 1 : 0, ReadBool(atv, g_bUnderwater) ? 1 : 0,
            ReadFloat(atv, g_batteryOff), ReadBool(atv, g_bInputFwd) ? 1 : 0,
            ReadFloat(atv, g_torqOff), ReadFloat(atv, g_speedOff));
}

// The scan-hub consumer.
bool HubEnsure()            { return ue_wrap::atv::EnsureResolved(); }
bool HubIsInstance(void* o) { return ue_wrap::atv::IsAtv(o); }
void HubPassBegin(void*, bool)  { g_pending.clear(); }
void HubMatch(void*, void* obj) { if (obj) g_pending.push_back(obj); }
size_t HubPassComplete(void*, bool, uint32_t) {
    g_atvs.swap(g_pending);
    g_pending.clear();
    return g_atvs.size();
}

// One ATV, one line. Everything here is a read.
void SampleOne(void* atv, size_t idx) {
    const std::wstring key = ue_wrap::atv::GetKeyString(atv);

    // The game's own matched four-body read. Unavailable, the vitals half is still logged: a
    // partial line is evidence, a missing line is ambiguous.
    ue_wrap::FVector bodyL{}, frL{}, flL{}, bkL{};
    ue_wrap::FRotator bodyR{};
    bool haveParts = false;
    if (g_partsFn) {
        ue_wrap::ParamFrame pf(g_partsFn);
        if (pf.valid() && ue_wrap::Call(atv, pf)) {
            bodyL = pf.Get<ue_wrap::FVector>(L"body_location");
            bodyR = pf.Get<ue_wrap::FRotator>(L"body_rotation");
            frL   = pf.Get<ue_wrap::FVector>(L"frontRight_location");
            flL   = pf.Get<ue_wrap::FVector>(L"frontLeft_location");
            bkL   = pf.Get<ue_wrap::FVector>(L"back_location");
            haveParts = true;
        }
    }

    // The quantity the corrector writes: the root body only, the same body the velocity write
    // addresses, so the two are comparable. Both components; the angular one explains why a parked
    // author still routed packets onto the write path.
    ue_wrap::FVector velL{}, velA{};
    ue_wrap::engine::GetActorRootPhysicsVelocity(atv, velL, velA);

    CensusRigComponents(atv);

    // wos: the wheels the rig believes are on a surface (the suspension gate); mass: the root
    // body's, so a configuration difference is readable in the same line.
    const int32_t wos      = WheelsOnSurfaceMask(atv, g_wheelsOnSurfaceOff);
    const float   airtime  = ReadFloat(atv, g_airtimeOff);      // a float
    const int32_t tirescnt = ReadInt(atv, g_tirescountOff);     // an int
    const float   mass     = ue_wrap::engine::GetActorRootMass(atv);

    const float fuel    = ue_wrap::atv::GetFuel(atv);
    const float health  = ue_wrap::atv::GetHealth(atv);
    const float battery = ReadFloat(atv, g_batteryOff);
    const float dirt    = ReadFloat(atv, g_dirtOff);
    const float dirtVel = ReadFloat(atv, g_dirtVelOff);
    const bool  driven  = ue_wrap::atv::IsDriven(atv);
    const void* occ     = ue_wrap::atv::GetOccupantPlayer(atv);
    // Which side of the mirror this sample is from, read through atv_sync's own published set
    // rather than a re-derived predicate: an instrument that reimplements the code under test
    // agrees with itself, not with it.
    const bool  ownsTick = coop::atv_sync::OwnsTick(atv);

    // The tire state rides its own line and its own TU: the four arrays are the only observable of
    // a defect whose verb (processTire) dispatches EX_LocalVirtualFunction and cannot be hooked.
    // Emitted before the parts branch, so a tire line exists even without a rig read.
    coop::atv_tire_probe::Sample(atv, idx, key.c_str(), ownsTick, g_sample);

    if (haveParts) {
        // The wheel-to-body distances are rotation-invariant, so they isolate suspension travel
        // from the body tipping; a frozen rig holds them to the bit, a live one breathes.
        const float dFR = Dist(frL, bodyL), dFL = Dist(flL, bodyL), dBK = Dist(bkL, bodyL);
        UE_LOGI("[ATVP] n=%u i=%zu key='%ls' driven=%d owns=%d occ=%p "
                "body=(%.1f,%.1f,%.1f) rot=(%.1f,%.1f,%.1f) "
                "vel=(%.1f,%.1f,%.1f) angv=(%.1f,%.1f,%.1f) "
                "partZ=(%.1f,%.1f,%.1f) rideH=%.2f "
                "wos=0x%X airtime=%.2f tirescnt=%d mass=%.1f "
                "susFR=%.3f susFL=%.3f susBK=%.3f "
                "fuel=%.3f batt=%.3f dirt=%.4f dirtVel=%.4f hp=%.2f",
                g_sample, idx, key.c_str(), driven ? 1 : 0, ownsTick ? 1 : 0, occ,
                bodyL.X, bodyL.Y, bodyL.Z, bodyR.Pitch, bodyR.Yaw, bodyR.Roll,
                velL.X, velL.Y, velL.Z, velA.X, velA.Y, velA.Z,
                frL.Z, flL.Z, bkL.Z,
                // The ride height, the body's Z above the mean of its three rig bodies: the wheel
                // distances are 3-D lengths over a mostly horizontal arm, so a 40 cm vertical
                // deformation moves them about a centimetre, inside the normal-travel band, blind
                // to the one axis that ever failed.
                bodyL.Z - (frL.Z + flL.Z + bkL.Z) / 3.f,
                wos, airtime, tirescnt, mass,
                dFR, dFL, dBK, fuel, battery, dirt, dirtVel, health);
    } else {
        ue_wrap::FVector loc{}; ue_wrap::FRotator rot{};
        ue_wrap::atv::GetRootTransform(atv, loc, rot);
        UE_LOGI("[ATVP] n=%u i=%zu key='%ls' driven=%d owns=%d occ=%p "
                "body=(%.1f,%.1f,%.1f) rot=(%.1f,%.1f,%.1f) "
                "vel=(%.1f,%.1f,%.1f) angv=(%.1f,%.1f,%.1f) NOPARTS "
                "wos=0x%X airtime=%.2f tirescnt=%d mass=%.1f "
                "fuel=%.3f batt=%.3f dirt=%.4f dirtVel=%.4f hp=%.2f",
                g_sample, idx, key.c_str(), driven ? 1 : 0, ownsTick ? 1 : 0, occ,
                loc.X, loc.Y, loc.Z, rot.Pitch, rot.Yaw, rot.Roll,
                velL.X, velL.Y, velL.Z, velA.X, velA.Y, velA.Z,
                wos, airtime, tirescnt, mass,
                fuel, battery, dirt, dirtVel, health);
    }
}

// Is there another peer whose world is loaded, somebody who can mirror this rig? The scan
// starts at slot 0 and skips our own: the world-ready flag means different things per role (a
// host marks a client slot on its ClientWorldReady; a client marks slot 0 at its connect edge),
// so a loop over slots 1..N is correct on a host and vacuously false on a client.
bool AnyPeerWorldReady(coop::net::Session& s) {
    const int mine = static_cast<int>(coop::players::Registry::Get().LocalPeerId());
    for (int slot = 0; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        if (slot == mine) continue;
        if (s.IsSlotWorldReady(slot)) return true;
    }
    return false;
}

}  // namespace

void Install() {
    if (!g_checked) {
        g_checked = true;
        g_enabled  = ::coop::config::ResolveFlag(::coop::config_registry::rows::atv_probe);
        g_sitArmed = ::coop::config::ResolveFlag(::coop::config_registry::rows::atv_probe_sit);
        if (g_enabled) UE_LOGI("atv_probe: ENABLED (ini [dev] atv_probe=1) -- sampling every %d ms "
                               "(drive arm %s on THIS peer)", kSampleMs,
                               g_sitArmed ? "ARMED" : "off");
    }
    if (!g_enabled || g_installed) return;
    g_installed = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        /*name*/           "atv_probe",
        /*ctx*/            nullptr,
        /*EnsureResolved*/ &HubEnsure,
        /*IsInstance*/     &HubIsInstance,
        /*OnPassBegin*/    &HubPassBegin,
        /*OnMatch*/        &HubMatch,
        /*OnPassComplete*/ &HubPassComplete,
        /*settleScans*/    15,   // ATVs are a static class: never force full passes for us
    });
    UE_LOGI("atv_probe: scan-hub consumer registered");
}

// Which of the seat verb's three gates refused. Two are cheap and side-effect free (the fall
// velocity is a field read; checkEquip is a pure read of the held item), so the arm measures
// them; the third (the seat overlap) is left to elimination, with the occupant pointer as the
// nearest proxy.
void DiagnoseSeatRefusal(void* local, void* atv) {
    void* pcls = R::ClassOf(local);
    float fallZ = 0.f;
    bool haveFall = false;
    if (pcls) {
        const int32_t off = R::FindPropertyOffset(pcls, L"fallVeloc");
        if (off >= 0) {
            const ue_wrap::FVector v =
                *reinterpret_cast<ue_wrap::FVector*>(reinterpret_cast<uint8_t*>(local) + off);
            fallZ = v.Z;
            haveFall = true;
        }
    }

    int emptyHands = -1;              // -1 = could not ask
    std::wstring holdingClass = L"?";
    if (pcls) {
        if (void* fn = R::FindFunction(pcls, L"checkEquip")) {
            ue_wrap::ParamFrame pf(fn);
            if (pf.valid() && ue_wrap::Call(local, pf))
                emptyHands = pf.Get<bool>(L"empty") ? 1 : 0;
        }
        const int32_t hoff = R::FindPropertyOffset(pcls, L"holding_actor");
        if (hoff >= 0) {
            void* held = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(local) + hoff);
            holdingClass = held ? R::ClassNameOf(held) : std::wstring(L"<none>");
        }
    }

    UE_LOGW("[ATVP] ARM sit gates: fallVeloc.Z=%s (need |Z|<800), checkEquip.empty=%s "
            "(need 1), holding_actor=%ls, atv.occupant=%p (need null)",
            haveFall ? std::to_string(fallZ).c_str() : "?",
            emptyHands < 0 ? "?" : (emptyHands ? "1" : "0"),
            holdingClass.c_str(), ue_wrap::atv::GetOccupantPlayer(atv));
}

// Seat the local player through the game's own verb. True once the arm is finished with the
// attempt (seated, or out of retries).
bool TrySit(void* atv) {
    if (!g_actionNameFn) {
        UE_LOGW("[ATVP] ARM sit: actionName unresolved -- arm cannot fire");
        return true;
    }
    void* local = coop::players::Registry::Get().Local();
    if (!local) return false;   // not in gameplay yet; retry next tick

    // The verb dispatches on a string compare, so the frame carries a real FString aliasing this
    // local for the synchronous call; the callee only compares it.
    std::wstring verb = L"sit";
    R::FString fs{ verb.data(),
                   static_cast<int32_t>(verb.size()) + 1,
                   static_cast<int32_t>(verb.size()) + 1 };

    ue_wrap::ParamFrame pf(g_actionNameFn);
    if (!pf.valid()) { UE_LOGE("[ATVP] ARM sit: ParamFrame(actionName) invalid"); return true; }
    pf.Set<void*>(L"player", local);          // `hit` stays the frame's zeroed HitResult
    pf.Set<R::FString>(L"name", fs);
    const bool ok = ue_wrap::Call(atv, pf);
    ++g_sitTries;

    const bool driven = ue_wrap::atv::IsDriven(atv);
    UE_LOGI("[ATVP] ARM sit: actionName(local=%p, 'sit') on atv=%p -> %s, try %d/%d, driven=%d",
            local, atv, ok ? "called" : "CALL FAILED", g_sitTries, kSitRetries, driven ? 1 : 0);
    if (driven) return true;
    DiagnoseSeatRefusal(local, atv);
    if (g_sitTries >= kSitRetries) {
        // The three gates are named in the log; the seat overlap cannot be read without a second
        // write into the player.
        UE_LOGW("[ATVP] ARM sit: REFUSED %d times -- the seat verb is gated on "
                "|fallVeloc.Z|<800, empty hands (checkEquip), and an unoccupied playerHit "
                "[disasm @46420/@46522/@46645]; the run cannot exercise the corrector",
                g_sitTries);
        return true;
    }
    return false;
}

// Release the handbrake and hold the throttle: the fields the game's own key handlers write;
// setBrake is what makes `brake` physical.
void StartDrive(void* atv) {
    LogGates(atv, "pre-drive");
    if (ReadBool(atv, g_bBrake)) {
        WriteBool(atv, g_bBrake, false);
        if (g_setBrakeFn) {
            ue_wrap::ParamFrame pf(g_setBrakeFn);
            if (pf.valid()) ue_wrap::Call(atv, pf);
        } else {
            UE_LOGW("[ATVP] ARM drive: setBrake unresolved -- `brake` cleared but the "
                    "constraint is still applied; torque will be fought");
        }
    }
    WriteBool(atv, g_bInputFwd, true);
    const auto now = std::chrono::steady_clock::now();
    g_driveStart = now;
    g_lastDriveSample = now;
    g_pulseOn = true;
    g_pulseEdge = now;
    LogGates(atv, "drive-start");
}

void StopDrive(void* atv) {
    WriteBool(atv, g_bInputFwd, false);
    LogGates(atv, "drive-stop");
    if (g_dismountFn) {
        ue_wrap::ParamFrame pf(g_dismountFn);
        if (pf.valid()) ue_wrap::Call(atv, pf);
    }
    UE_LOGI("[ATVP] ARM done: dismount %s, driven=%d, banked %lld ms over %d ejection(s)",
            g_dismountFn ? "called" : "UNRESOLVED", ue_wrap::atv::IsDriven(atv) ? 1 : 0,
            g_drivenMs, g_reseats);
}

void Tick(coop::net::Session& session, bool isHost) {
    if (!g_enabled) return;
    if (!ResolveOffsets()) return;
    if (g_atvs.empty()) return;

    const auto now = std::chrono::steady_clock::now();
    if (g_lastSample.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastSample).count() < kSampleMs)
        return;
    g_lastSample = now;
    ++g_sample;

    for (size_t i = 0; i < g_atvs.size(); ++i) {
        if (g_atvs[i]) SampleOne(g_atvs[i], i);
    }

    if (g_lastCounterLog.time_since_epoch().count() == 0 ||
        std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastCounterLog).count()
            >= kCounterLogMs) {
        g_lastCounterLog = now;
        const auto hg = coop::atv_hit_guard::ReadCounters();
        const auto cc = coop::atv_corrector::ReadCounters();
        UE_LOGI("[ATVP] counters hitguard=%s %llu neutered / %llu allowed / %llu UNRESOLVED  "
                "corrector=%llu nudged / %llu warped / %llu cut / %llu parked-replace",
                hg.armed ? "armed" : "NEVER-ARMED",
                static_cast<unsigned long long>(hg.neutered),
                static_cast<unsigned long long>(hg.allowed),
                static_cast<unsigned long long>(hg.unresolved),
                static_cast<unsigned long long>(cc.corrections),
                static_cast<unsigned long long>(cc.warps),
                static_cast<unsigned long long>(cc.stallWarps),
                static_cast<unsigned long long>(cc.restPlaces));
        // The condition lane's counters: our apply-site counts, blind to the game's own callers.
        const auto cd = coop::atv_condition_sync::ReadCounters();
        UE_LOGI("[ATVP] condition applied=%llu verbs tires=%llu dirt=%llu spare=%llu health=%llu "
                "presence-skipped-differing=%llu deferred=%llu invalid-blocks=%llu",
                static_cast<unsigned long long>(cd.applied),
                static_cast<unsigned long long>(cd.updTiresCalled),
                static_cast<unsigned long long>(cd.updDirtCalled),
                static_cast<unsigned long long>(cd.updSpareCalled),
                static_cast<unsigned long long>(cd.updHealthCalled),
                static_cast<unsigned long long>(cd.presenceSkippedDiffering),
                static_cast<unsigned long long>(cd.deferred),
                static_cast<unsigned long long>(cd.invalidBlocks));
    }

    // The evidence-complete announce: once the arm is done and the settle window has passed, once.
    if (g_sitArmed && g_arm == Arm::Done && !g_doneAnnounced) {
        if (g_armDoneAt.time_since_epoch().count() == 0) {
            g_armDoneAt = now;
        } else if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_armDoneAt).count()
                       >= kDoneSettleMs) {
            g_doneAnnounced = true;
            UE_LOGI("[ATV-PROBE] DONE -- drive target banked (%lld ms) + %d s settle; evidence "
                    "complete, the run may end", g_drivenMs, kDoneSettleMs / 1000);
        }
    }

    // Which peer drives is the ini's call: the seat verb refuses a player whose hands are full, and
    // a host's save may have him holding something, while the client-authored direction is the
    // harder one (the host as the mirror). Set [dev] atv_probe_sit=1 on exactly one peer, or two
    // authors race for the same rig.
    (void)isHost;
    if (!g_sitArmed || g_arm == Arm::Done || g_atvs.empty()) return;

    // The target latch. g_atvs is rebuilt every pass and index 0 is not a stable identity, so the
    // arm holds one actor across the drive; but index 0 can be a placed actor on one pass and an
    // unplaced one on the next, so the latch happens at the sit edge, after the rig is proven
    // placed.
    void* atv = g_armAtv ? g_armAtv : g_atvs[0];

    if (g_arm == Arm::Wait) {
        // Say why we are still waiting: the gate can fail to fire for four reasons, and every few
        // seconds the blocking one is named.
        static uint64_t s_lastWhyMs = 0;
        const uint64_t nowMs =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count());
        const bool why = (nowMs - s_lastWhyMs) >= 5000;
        if (why) s_lastWhyMs = nowMs;

        // The delay is clocked from the first sample that is both placed and mirrored by somebody;
        // anchored on the ATV alone, the drive window would open while the client still downloads
        // the save.
        if (!AnyPeerWorldReady(session)) {
            if (why) UE_LOGI("[ATVP] ARM wait: no peer is world-ready yet");
            g_firstValid = {};
            return;
        }
        // Clocked from the first placed sample, not from boot.
        ue_wrap::FVector loc{}; ue_wrap::FRotator rot{};
        if (!ue_wrap::atv::GetRootTransform(atv, loc, rot)) {
            if (why) UE_LOGW("[ATVP] ARM wait: GetRootTransform(%p) failed", atv);
            return;
        }
        if (std::fabs(loc.X) < 1e-6f && std::fabs(loc.Y) < 1e-6f && std::fabs(loc.Z) < 1e-6f) {
            if (why) UE_LOGI("[ATVP] ARM wait: atv=%p is not placed yet (body at origin); "
                             "%zu ATV(s) in the index", atv, g_atvs.size());
            g_firstValid = {};
            return;
        }
        if (g_firstValid.time_since_epoch().count() == 0) {
            g_firstValid = now;
            UE_LOGI("[ATVP] ARM wait: clock started -- seating in %d ms", kSitDelayMs);
            return;
        }
        const long long waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_firstValid).count();
        if (waited < kSitDelayMs) {
            if (why) UE_LOGI("[ATVP] ARM wait: %lld/%d ms", waited, kSitDelayMs);
            return;
        }
        g_armAtv = atv;   // latch ONLY now: proven placed, with a world-ready peer to mirror it
        g_arm = Arm::Sit;
        UE_LOGI("[ATVP] ARM: target latched atv=%p at (%.1f,%.1f,%.1f)", atv, loc.X, loc.Y, loc.Z);
    }

    if (g_arm == Arm::Sit) {
        if (g_lastSitTry.time_since_epoch().count() != 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastSitTry).count() < kSitRetryMs)
            return;
        g_lastSitTry = now;
        if (!TrySit(atv)) return;
        if (!ue_wrap::atv::IsDriven(atv)) {
            if (g_reseats > 0)
                UE_LOGW("[ATVP] ARM: could not re-seat after ejection %d -- banked %lld ms "
                        "of driven time (wanted %d)", g_reseats, g_drivenMs, kDriveMs);
            g_arm = Arm::Done;
            return;
        }
        StartDrive(atv);
        g_arm = Arm::Drive;
        return;
    }

    if (g_arm == Arm::Drive) {
        LogGates(atv, "driving");

        // Only genuinely driven time is banked: a crash empties the seat, and the clock must not
        // run through the recovery.
        const long long dt = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_lastDriveSample).count();
        g_lastDriveSample = now;
        const bool driven = ue_wrap::atv::IsDriven(atv);
        if (driven) g_drivenMs += dt;

        if (!driven) {
            // Ejected. Re-seat through the same verb; the seat body teleports the player in, so it
            // works from wherever the ragdoll came to rest.
            if (g_reseats >= kMaxReseats) {
                UE_LOGW("[ATVP] ARM: ejected %d times and out of re-seats -- banked %lld ms "
                        "of driven time (wanted %d)", g_reseats, g_drivenMs, kDriveMs);
                StopDrive(atv);
                g_arm = Arm::Done;
                return;
            }
            WriteBool(atv, g_bInputFwd, false);
            ++g_reseats;
            g_sitTries = 0;
            g_lastSitTry = now;
            g_arm = Arm::Sit;
            UE_LOGI("[ATVP] ARM: ejected (crash) -- re-seat %d/%d, banked %lld/%d ms driven",
                    g_reseats, kMaxReseats, g_drivenMs, kDriveMs);
            return;
        }

        // Pulse the throttle rather than holding it (kPulseOnMs).
        const long long sinceEdge = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_pulseEdge).count();
        if (sinceEdge >= (g_pulseOn ? kPulseOnMs : kPulseOffMs)) {
            g_pulseOn = !g_pulseOn;
            g_pulseEdge = now;
            WriteBool(atv, g_bInputFwd, g_pulseOn);
        }

        if (g_drivenMs < kDriveMs) return;
        StopDrive(atv);
        g_arm = Arm::Done;
    }
}

}  // namespace coop::dev::atv_probe
