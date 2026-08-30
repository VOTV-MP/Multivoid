// coop/dev/atv_probe.cpp -- see header.

#include "coop/dev/atv_probe.h"

#include "coop/config/config.h"
#include "coop/element/object_scan_hub.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
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

// The arm waits this long after the FIRST sample whose body is placed, not after
// boot: the ATV is in GUObjectArray with body=(0,0,0) for ~15 samples before its
// components exist, and seating into a rig that is not there yet proves nothing.
constexpr int kSitDelayMs = 25000;
// How much CUMULATIVE driven time the arm wants. The 2026-08-29 baseline
// (docs/vehicles/ATV.md 13) measured the two peers agreeing to 0.3 cm AT REST, so a
// seated-but-stationary ATV re-measures the case that already passes; the corrector is
// only under load while the rig is actually moving.
//
// CUMULATIVE, not contiguous, and that is measured: on the first run that ever drove one
// (2026-08-30, 00:06:47) the ATV covered 9.6 m in 2.5 s at full throttle, hit something,
// and the game RAGDOLLED the driver out at 600 cm/s -- `net: RagdollPose emit #1
// |linVel|=600 cm/s` in the same second the seat emptied. A base full of geometry is not a
// test track, so an arm that demands one contiguous window measures ~2 s and stops. It
// re-seats instead, and counts only the time the rig was actually driven.
constexpr int kDriveMs = 20000;
// Full throttle saturates torqAlpha (1500) in about a second and the rig is then travelling
// fast enough that the next thing it meets ends the run. Pulsing keeps it moving -- which is
// all the corrector needs -- without winding up to crash speed.
constexpr int kPulseOnMs  = 250;
constexpr int kPulseOffMs = 750;
// A crash costs a re-seat; cap them so a rig wedged against a wall cannot loop forever.
constexpr int kMaxReseats = 6;
// The seat verb is refused, not queued, when the player is mid-fall (@46420) -- so a
// refusal is worth retrying a couple of times before the arm gives up and says so.
constexpr int kSitRetries    = 3;
constexpr int kSitRetryMs    = 2000;

// ---- the drive arm ----------------------------------------------------------------
// ATV_C::playerSit IS A DEAD STUB on this build and four runs' worth of "SIT fired ...
// (driven now=0)" is what that looks like from outside: it writes
// UBER[K2Node_Event_player_18] -- a variable with ZERO readers anywhere in the ubergraph
// -- and jumps to ExecuteUbergraph_ATV(9122), which is a bare EX_PopExecutionFlow
// [V, disasm ATV.playerSit + ATV.ExecuteUbergraph_ATV @9122].
//
// The REAL seat verb is actionName(player, hit, name) with name == "sit" -> uber @46046,
// and it is gated three deep before it reaches the seat body at @5616 [V, disasm]:
//     @46420   |player.fallVeloc.Z| < 800        else -> punched off (@46870)
//     @46522   player.checkEquip() reports EMPTY  else -> addHint  (@46753)
//     @46645   playerHit overlaps NOTHING at [0]  else -> addHint  (@46659)
// The seat body attaches + teleports the player onto playerHit, possesses the ATV, and
// sets isDriven := true (@6227) -- so it needs no proximity of its own, which is why the
// arm may call it from wherever the player happens to have spawned.
//
// Torque then needs BOTH isDriven (@29949 gates applyWheelTorque) and a non-zero
// torqAlpha, and torqAlpha's producer BAILS WHOLE at @34866 on
//     empty || brake || brokenn || underwater || battery <= 0
// A parked ATV sits on its handbrake, so the arm releases it through the game's own
// setBrake() (the exact pair the BP itself runs at @7215-7226) rather than poking the
// physics. Everything the arm writes -- brake, input_forward -- is written by the game's
// own key handlers to the same field (@23105, @24061), so this is the input path, not a
// side door into it.
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

// Field offsets the ue_wrap ATV surface does not expose (it has fuel/health/brake).
// Resolved by NAME -- an offset literal here would be a second copy of the version
// surface that docs/VERSION_MIGRATION.md exists to keep to two files.
int32_t g_batteryOff = -1;
int32_t g_dirtOff    = -1;
int32_t g_dirtVelOff = -1;
// THE VALUE THE TICK GATES ITS SUSPENSION ON -- and the one nothing had ever read.
// ExecuteUbergraph_ATV expr 1228-1229 is `Array_Contains(wheelsOnSurface, ..)` -> EX_JumpIfNot
// guarding expr 1230-1237, which ends in `mesh.AddForce(GetUpVector * ..)`; the same test at
// 1198-1202 picks the SelectFloat that becomes SetMassScale on backWheelRoot/frontWheel_L/R.
// The array is written from inside the WHEEL ComponentHit segments (exprs 366-379 and 577-586,
// beside processTire and checkAirtime) -- i.e. by the very delegates atv_hit_guard cancels on
// every non-owner. If a mirror's count is 0 while its author's is not, the mirror is a rig
// whose suspension WE switched off, and every pose correction this lane makes is being applied
// to a vehicle that is already sitting wrong.
int32_t g_wheelsOnSurfaceOff = -1;   // TArray: Num is the int32 at +8
int32_t g_airtimeOff         = -1;
int32_t g_tirescountOff      = -1;
bool    g_offsResolved = false;

// THE VALUES, NOT THE LENGTH -- and the first version of this read got that wrong.
// `wheelsOnSurface` is a TArray<bool> whose CDO default ALREADY has four elements
// [false,false,false,false] (ATV.json Exports[246]/Data[8]), so its Num is 4 BY CONSTRUCTION and
// answers nothing. The tick's gate is `Array_Contains(wheelsOnSurface, <a wheel>)`, which tests
// the four VALUES. Reading Num printed `wos=4` on both peers in every run -- including on an
// actor whose transform did not exist yet -- and that non-answer was then written into
// docs/vehicles/ATV.md 17.3 as evidence that the array "is not the live contact set". It was
// evidence of nothing. Caught by the post-ship audit of the very commit whose thesis was "the
// instrument was blind to the axis that failed".
//
// Layout: FScriptArray = {void* Data; int32 Num; int32 Max} (the dump's ElementSize 16 confirms
// it). A TArray<bool> stores one BYTE per element -- it is not TBitArray. Returns a 4-bit mask,
// or -1 for unreadable, which must stay distinguishable from 0 ("no wheel is on a surface").
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

// tirescount is an IntProperty (ATV.json: SerializedType=IntProperty, ElementSize 4). Reading it
// with ReadFloat type-punned 4 into 5.6e-45, which "%.1f" prints as a perfectly plausible 0.0 --
// on both peers, in every run. The -1 sentinel could not fire because the offset RESOLVES; the
// failure mode is a third one the sentinel was never designed for: read, wrong type, plausible.
int32_t ReadInt(void* obj, int32_t off) {
    if (!obj || off < 0) return -1;
    return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
}

void* g_partsFn = nullptr;   // ATV_C::vehicleGetParts (8 out-params, no in-params)

// The hub hands us matches into `g_pending`; `g_atvs` is the completed swap.
std::vector<void*> g_pending;
std::vector<void*> g_atvs;

std::chrono::steady_clock::time_point g_lastSample{};
uint32_t g_sample = 0;

constexpr int kSampleMs = 500;
// THE COUNTERS HAVE TO REACH A LOG THE ARCHIVE HAS. atv_sync prints the hit-guard and corrector
// tallies exactly once, from OnDisconnect -- and every autonomous scenario KILLS both peers
// rather than disconnecting, so that line is absent from all 18 archived runs. The acceptance
// arm that reads them (A3's "armed but NEVER FIRED" check) has therefore never been able to fire
// on real evidence, and the 2026-08-30 mask change -- five delegates per non-owner that now
// dispatch instead of being cancelled -- was invisible to every counter in the tree. Emitting
// them from the probe costs three relaxed atomic loads every 10 s and puts them where the
// evidence store already looks.
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

    // The drive arm's own surface. Resolved unconditionally (not behind the arm flag) so
    // a run whose arm never fires still records WHICH of these was missing -- an arm that
    // silently cannot fire is the failure this whole rewrite exists to end.
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

// CAN WE WRITE THE WHOLE RIG, OR ONLY ITS ROOT?
//
// The lane's live question (docs/vehicles/ATV.md 0.4 / 9.4): every write we make to a mirrored
// ATV addresses ONE of its five bodies. `TeleportRig` escapes that because the game's own
// `teleportVehicle` re-places the wheels; our velocity write does not, because
// SetActorRootPhysicsVelocity resolves to UPrimitiveComponent::SetPhysicsLinearVelocity on the
// ROOT component only (engine_attach.cpp:182-196), and the four wheels are separate COMPONENTS,
// not extra bodies inside the root -- so `SetAllPhysicsLinearVelocity` would not reach them
// either. A rig-wide write therefore needs the component pointers, and nothing has ever checked
// that they are reachable. This census answers exactly that, once, from a live ATV: which of the
// rig's component properties exist on ATV_C, which are non-null on an instance, and what class
// each is. If they resolve, the class fix is buildable; if they do not, the invariant "never
// author a five-body rig from one body" needs a different instrument and we learn that BEFORE
// designing around it rather than after.
//
// Names are the components the seven ComponentHit delegates are bound to (atv.cpp
// kHitDelegateNames), plus `car1_frontWheel_L`, which appears in no delegate name and may
// therefore not exist -- a NOT-FOUND on it is data, not a failure.
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

// The five terms @34866 ORs together to skip the torque block whole, plus what the
// throttle actually produced. Logged every sample while driving, so a run in which the
// ATV never moves names its own reason instead of leaving the corrector to be blamed.
void LogGates(void* atv, const char* phase) {
    UE_LOGI("[ATVP] ARM %s: driven=%d empty=%d brake=%d broken=%d underwater=%d batt=%.2f "
            "fwd=%d torq=%.3f speed=%.2f",
            phase, ue_wrap::atv::IsDriven(atv) ? 1 : 0,
            ReadBool(atv, g_bEmpty) ? 1 : 0, ReadBool(atv, g_bBrake) ? 1 : 0,
            ReadBool(atv, g_bBroken) ? 1 : 0, ReadBool(atv, g_bUnderwater) ? 1 : 0,
            ReadFloat(atv, g_batteryOff), ReadBool(atv, g_bInputFwd) ? 1 : 0,
            ReadFloat(atv, g_torqOff), ReadFloat(atv, g_speedOff));
}

// ---- scan-hub consumer ------------------------------------------------------------
bool HubEnsure()            { return ue_wrap::atv::EnsureResolved(); }
bool HubIsInstance(void* o) { return ue_wrap::atv::IsAtv(o); }
void HubPassBegin(void*, bool)  { g_pending.clear(); }
void HubMatch(void*, void* obj) { if (obj) g_pending.push_back(obj); }
size_t HubPassComplete(void*, bool, uint32_t) {
    g_atvs.swap(g_pending);
    g_pending.clear();
    return g_atvs.size();
}

// One ATV, one line. Everything here is a READ.
void SampleOne(void* atv, size_t idx) {
    const std::wstring key = ue_wrap::atv::GetKeyString(atv);

    // The game's own matched 4-body read. If it is unavailable we still log the
    // vitals half rather than dropping the sample -- a partial line is evidence,
    // a missing line is ambiguous between "not resolved" and "no ATV".
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

    // THE QUANTITY THE CORRECTOR WRITES, and the one nothing has ever measured. Three driven
    // runs showed a mirror sinking 23-40 cm the instant it stopped authoring, and the archive
    // could not say whether the lane wrote a downward velocity onto it or ratcheted it down over
    // many packets, because no sample carried a velocity at all. A verdict about a value the
    // instrument never records is a guess with a citation. Root body only -- the same body
    // SetActorRootPhysicsVelocity writes, so the two are comparable by construction.
    // BOTH components. velA was filled and discarded until 2026-08-30, and it is the one
    // quantity that explains why a parked author still routed packets onto the write path --
    // measured and thrown away is the same blindness as not measured at all.
    ue_wrap::FVector velL{}, velA{};
    ue_wrap::engine::GetActorRootPhysicsVelocity(atv, velL, velA);

    CensusRigComponents(atv);

    // wos: how many wheels the rig believes are touching a surface (the suspension gate).
    // mass: the root body's, so a "the mirror is configured differently" reading is available
    // in the same line rather than needing a second run.
    const int32_t wos      = WheelsOnSurfaceMask(atv, g_wheelsOnSurfaceOff);
    const float   airtime  = ReadFloat(atv, g_airtimeOff);      // FloatProperty [V]
    const int32_t tirescnt = ReadInt(atv, g_tirescountOff);     // IntProperty  [V]
    const float   mass     = ue_wrap::engine::GetActorRootMass(atv);

    const float fuel    = ue_wrap::atv::GetFuel(atv);
    const float health  = ue_wrap::atv::GetHealth(atv);
    const float battery = ReadFloat(atv, g_batteryOff);
    const float dirt    = ReadFloat(atv, g_dirtOff);
    const float dirtVel = ReadFloat(atv, g_dirtVelOff);
    const bool  driven  = ue_wrap::atv::IsDriven(atv);
    const void* occ     = ue_wrap::atv::GetOccupantPlayer(atv);
    // WHICH SIDE OF THE MIRROR is this sample from? The 2026-08-29 run could not say, so
    // docs/vehicles/ATV.md 11.1 stayed open even though the probe ran perfectly: an idle ATV is
    // never mirrored, and nothing asserted the receiver was actually mirroring during the driven
    // window. Read through atv_sync's own published set rather than recomputing the predicate --
    // an instrument that reimplements the code under test agrees with itself, not with it.
    const bool  ownsTick = coop::atv_sync::OwnsTick(atv);

    if (haveParts) {
        // |wheel - body| is ROTATION-INVARIANT, so it isolates suspension travel from
        // the body tipping/turning. A rigid frozen rig holds these three constant to
        // the bit; a live one breathes. THIS is the frozen-corpse measurement.
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
                // RIDE HEIGHT: the body's Z above the mean of its own three rig bodies. This is
                // the term susFR/FL/BK cannot express -- they are 3-D distances over a ~92 cm
                // mostly-HORIZONTAL arm, so a 40 cm VERTICAL deformation moves them by ~1.1 cm,
                // inside the "2-4 cm of normal travel" band the acceptance arm is built on. The
                // lane graded itself green on a quantity ~36x blind to the only axis that ever
                // failed. Computed here, in the sample, so no consumer can forget it.
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

// Is there ANOTHER peer whose world is loaded -- i.e. somebody who can mirror this rig?
//
// The scan starts at slot 0 and skips OUR OWN slot, which is the whole subtlety and cost a
// run to find. `slotWorldReady_` means different things on the two roles [V,
// subsystems.cpp:356 + event_feed.cpp:236]: a HOST marks a CLIENT slot when that client
// announces ClientWorldReady, while a CLIENT marks SLOT 0 at its connect edge because "the
// host always has a world". A loop over slots 1..N is therefore correct on a host and
// vacuously false on a client -- which is exactly what the drive arm reported for a whole
// 150 s run once the arm moved to the client ("no peer is world-ready yet", forever).
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

// WHICH of the seat verb's three gates refused. Two of them are cheap and side-effect
// free, so the arm measures them instead of leaving the next reader to guess:
//   @46420  |player.fallVeloc.Z| < 800   -- a plain struct field read
//   @46522  player.checkEquip() -> empty -- [V, disasm] a PURE read: it casts holding_actor,
//           reads its data and sets `empty = !IsValidClass(data.class)`. Nothing is mutated.
// The third (@46645, playerHit overlapping nothing) is left to elimination, and the ATV's own
// occupant pointer is printed beside it as the nearest cheap proxy.
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

// Seat the local player through the game's own interaction verb. Returns true once the
// arm is finished with the seat attempt (seated, or out of retries).
bool TrySit(void* atv) {
    if (!g_actionNameFn) {
        UE_LOGW("[ATVP] ARM sit: actionName unresolved -- arm cannot fire");
        return true;
    }
    void* local = coop::players::Registry::Get().Local();
    if (!local) return false;   // not in gameplay yet; retry next tick

    // The verb dispatches on a STRING compare (@46046 NotEqual_StriStri(name, 'sit')), so
    // the frame carries a real FString. It aliases this local for the duration of the
    // synchronous ProcessEvent and the callee only compares it -- the same convention
    // asset_load's LoadObject path uses.
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
        // Name the three gates so the next reader does not re-disassemble them. We cannot
        // read checkEquip or the seat overlap from here without calling into the player,
        // which would be a second write; the log says what to check instead.
        UE_LOGW("[ATVP] ARM sit: REFUSED %d times -- the seat verb is gated on "
                "|fallVeloc.Z|<800, empty hands (checkEquip), and an unoccupied playerHit "
                "[disasm @46420/@46522/@46645]; the run cannot exercise the corrector",
                g_sitTries);
        return true;
    }
    return false;
}

// Release the handbrake and hold the throttle. Both fields are the ones the game's own
// key handlers write (@7215-7226, @23105) -- setBrake() is what makes `brake` physical.
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
        UE_LOGI("[ATVP] counters hitguard=%s %llu cancelled / %llu allowed / %llu permitted  "
                "corrector=%llu nudged / %llu warped / %llu cut / %llu parked-replace",
                hg.armed ? "armed" : "NEVER-ARMED",
                static_cast<unsigned long long>(hg.cancelled),
                static_cast<unsigned long long>(hg.allowed),
                static_cast<unsigned long long>(hg.permitted),
                static_cast<unsigned long long>(cc.corrections),
                static_cast<unsigned long long>(cc.warps),
                static_cast<unsigned long long>(cc.stallWarps),
                static_cast<unsigned long long>(cc.restPlaces));
    }

    // WHICH PEER DRIVES IS THE INI'S CALL, not this file's. The arm used to be host-only,
    // and on 2026-08-29 that made it unrunnable: the seat verb refuses a player whose hands
    // are full (@46522), and the host's save has him holding a prop_coingun_C -- MEASURED,
    // gates 1 and 3 passed. The alternatives were to make the arm put the item away (a
    // world mutation inside a measurement run) or to let the peer with empty hands drive.
    // The second costs nothing and tests the HARDER direction: a client-authored ATV, with
    // the host as the mirror, which is the authority path `authorSlot` exists for. So the
    // arm runs wherever [dev] atv_probe_sit=1 is set -- set it on exactly ONE peer, or two
    // authors race for the same rig.
    (void)isHost;
    if (!g_sitArmed || g_arm == Arm::Done || g_atvs.empty()) return;

    // TARGET LATCH, and WHERE it happens is the whole point. g_atvs is rebuilt every scan
    // pass and index 0 is not a stable identity, so the arm must hold ONE actor across the
    // drive -- otherwise it could throttle one ATV and dismount another. But latching on
    // the first tick is worse: measured 2026-08-29, index 0 is a PLACED actor on pass 1 and
    // an UNPLACED one on passes 2-3 (the same key, the same index), so an early latch
    // pinned a rig that never left the origin and the arm waited out the entire run
    // reporting "not placed" while the sampler happily logged a placed body 2x/s. So the
    // latch happens at the SIT edge -- after the rig is proven placed -- and only then.
    void* atv = g_armAtv ? g_armAtv : g_atvs[0];

    if (g_arm == Arm::Wait) {
        // SAY WHY WE ARE STILL WAITING. The first build of this gate had four silent
        // early-returns and a run came back with 282 samples and not one ARM line -- the
        // arm can fail to fire for four different reasons and none of them left a trace
        // (docs/LESSONS.md, "a counter you never print is not an instrument"). Every ~5 s
        // while waiting, name the blocking condition.
        static uint64_t s_lastWhyMs = 0;
        const uint64_t nowMs =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count());
        const bool why = (nowMs - s_lastWhyMs) >= 5000;
        if (why) s_lastWhyMs = nowMs;

        // Clock the delay from the first sample that is BOTH placed and mirrored-by-somebody.
        // Anchoring on the ATV alone would open the drive window while the client is still
        // downloading the save, and the run would grade a mirror that did not exist yet.
        if (!AnyPeerWorldReady(session)) {
            if (why) UE_LOGI("[ATVP] ARM wait: no peer is world-ready yet");
            g_firstValid = {};
            return;
        }
        // Clock the delay from the first PLACED sample, not from boot.
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

        // Bank only the time the rig was genuinely driven. A crash empties the seat and the
        // clock must not keep running through the recovery, or the arm would "finish" having
        // measured two seconds.
        const long long dt = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_lastDriveSample).count();
        g_lastDriveSample = now;
        const bool driven = ue_wrap::atv::IsDriven(atv);
        if (driven) g_drivenMs += dt;

        if (!driven) {
            // Ejected. Re-seat through the same verb; the seat body teleports the player in,
            // so it works from wherever the ragdoll came to rest.
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

        // Pulse the throttle rather than holding it: see kPulseOnMs.
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
