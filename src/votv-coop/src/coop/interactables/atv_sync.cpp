// coop/atv_sync.cpp -- see coop/atv_sync.h. ATV/quadbike (AATV_C) rig sync.
//
// THE MIRROR SIMULATES (arc 1, 2026-08-29 -- this REPLACES the freeze/teleport lane, RULE 2).
// A peer that does not author an ATV leaves its physics ON, switches its BRAIN off, and is
// CORRECTED toward the authority. It does not freeze, it is not teleported per packet, and there
// is no un-freeze. Why the inversion, measured rather than argued: AATV_C is a five-body
// constraint rig (sus_*/ax_*) whose entire visible output is suspension travel, and
// SetActorLocation moves the ROOT ONLY -- so the old kinematic apply teleported one body 20x/s and
// dragged four constrained bodies behind it. The autonomous two-peer probe put numbers on it
// (docs/vehicles/ATV.md 13): native travel 2-4 cm, the shipped lane 29.58 cm and 1.1 m of drift,
// and the worst of it authored by the RELEASE path, which handed the other peer's copy a 158 cm/s
// launch from an already-stale pose. That whole path is gone rather than bounded.
//
// MTA precedent (RULE 2026-05-28): CNetAPI::ReadVehiclePuresync writes SetMoveSpeed/SetTurnSpeed
// hard every packet; CClientVehicle::UpdateTargetPosition:3901 warps past a speed-scaled
// threshold; CUnoccupiedVehicleSync elects a syncer for an unoccupied vehicle and sends it ONLY
// when it changed. One deliberate divergence, cited at the site: we correct through VELOCITY
// rather than MTA's per-frame transform nudge, because their vehicle is one rigid body and ours
// is a rig that a per-frame root nudge would stretch.
//
// TWO PREDICATES, NEVER ONE. `IsPoseAuthor` = I drive or carry it. `OwnsTick` = that, OR I am the
// host and nobody authors it. On the host with nobody driving the first is false and the second
// is true; fusing them reproduces the PR #9 defect, where losing one meaning fired the other's
// edge. `occupantSlot` (the SEAT, which device_occupancy's deny reads) and `authorSlot` (who
// streams) stay two fields for the same reason: a peer GRABBING an ATV must not deny its seat.
//
// Keyed by Key@0x0618 (save-placed, cross-peer stable) -- NOT eid/Element (YAGNI for one
// always-present keyed actor; the grime/window dirt sync made the same divergence vs its element
// blueprint). The index/connect-snapshot shape follows the keyed-interactable modules
// (power_sync/keypad_sync).
//
// SEAT CONTENTION (PR #9, arigalit). Each entry tracks `occupantSlot` (0xFF = free). A peer takes
// pose authority only while the seat is free or already its own, so a second peer walking up to an
// ATV someone is driving is denied at the INPUT seam (device_occupancy) and never runs the native
// seating logic. That is a client-side PRODUCER suppression, not a receive gate -- COOP_SYNCER_MODEL
// section 2b's required shape.
//
// The seat is SELF-ELECTED, not arbitrated, and that has one consequence worth naming: if two peers
// mount inside the same round-trip, neither has heard the other yet, so both elect themselves. The
// deny gate cannot see it (each reads occupantSlot 0xFF). LOWER SLOT WINS resolves it in OnReliable
// below -- a total order both peers already agree on, costing nothing on the wire. It is a tie-break,
// NOT authority: a peer that lies about its slot is not defeated by it, which is why the real fix is
// an arbitrated seat claim (act-as-host, COOP_SYNCER_MODEL section 2b) and this is not that.

#include "coop/interactables/atv_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"  // WireKeyFromString / StringFromWireKey / FnvKey (shared)
#include "coop/player/players_registry.h"   // Registry::Local / LocalPeerId / kMaxPeers
#include "coop/player/roster_ledger.h"      // SubscribeSlotReplaced -- a departed author must not hold an ATV

#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"          // ReadMainPlayerGrabState (grabber authority) + Get/SetActorRootPhysicsVelocity
#include "ue_wrap/core/game_thread.h"       // RegisterInterceptor -- the collision half of "brains off"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"     // R-2: gen-stamped index (dead-world guard)
#include "coop/element/object_scan_hub.h"      // R-2: the shared sliced scan pass
#include "ue_wrap/core/types.h"           // FVector, FRotator, NormalizeAxis

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace coop::atv_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace A = ue_wrap::atv;
using ue_wrap::FVector;
using ue_wrap::FRotator;
using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;
using coop::net::FnvKey;

constexpr uint64_t kDriveSendMs = 50;   // ~20 Hz while a peer authors it (drives or carries it)
constexpr uint64_t kIdleSendMs  = 200;  // 5 Hz idle-syncer ceiling -- AND only when it changed

// The idle-syncer change gate, CUnoccupiedVehicleSync::WriteVehicleInformation:295-350 in our
// units. A PARKED ATV SENDS NOTHING, which is what makes host-syncs-idle-vehicles free; MTA's own
// thresholds are FLOAT_EPSILON/0.1 in metres and MIN_ROTATION_DIFF in degrees.
constexpr float kIdleMovedCm   = 1.0f;
constexpr float kIdleTurnedDeg = 0.5f;
constexpr float kIdleMovingCmS = 1.0f;

// The corrector. Warp is speed-scaled after CClientVehicle.cpp:3901 (their 15 + 10*|v| is in GTA
// units); ours is sized off the measured rig -- the ATV is ~2 m long and its native suspension
// travel is 2-4 cm (docs/vehicles/ATV.md 13.1), so 2 m plus half a second of travel is "so far
// apart that closing it smoothly would look worse than a cut".
constexpr float kWarpBaseCm     = 200.f;
constexpr float kWarpPerSpeedS  = 0.5f;
constexpr float kWarpAngleDeg   = 45.f;   // orientation alone can justify a warp: a body can spin
                                          // in place without ever tripping the distance threshold
constexpr float kCorrDeadbandCm = 5.f;    // inside this, the wire velocity alone is the correction
constexpr float kCorrWindowS    = 0.10f;  // close the position error over ~100 ms
constexpr float kCorrMaxCmS     = 400.f;  // bound the corrective term: it must nudge, never launch

// Per-ATV state. Receiver-side interpolation state is GONE: a mirror is not interpolated toward a
// pose, it is a simulating body whose velocity we bias at packet arrival. All game-thread only.
struct AtvEntry {
    void*   actor = nullptr;
    int32_t idx   = -1;
    uint64_t lastSentMs   = 0;
    uint8_t  occupantSlot = 0xFF;  // the SEATED driver's peer slot (0xFF = seat free). THE SEAT --
                                   // device_occupancy::IsOccupiedByOther reads this and denies an
                                   // E-press from it, so a grabber must never appear here.
    uint8_t  authorSlot   = 0xFF;  // who STREAMS it (driver or grabber; 0xFF = nobody -> host syncs)
    bool     brainOn      = true;  // last value we wrote to SetBrainEnabled (latch: engine call only on change)
    bool     brainKnown   = false; // ...have we written it at all yet
    bool     wasPoseAuthor = false;// POSE authority last tick -- the release-edge detect, and it is
                                   // deliberately not the same variable as ownsTick (PR #9)
    bool     isClientSpawnedMirror = false;  // v77: a runtime ATV WE fresh-spawned (AtvSpawn) -> K2 on destroy
    // idle-syncer change gate
    FVector  lastSyncPos{};
    FRotator lastSyncRot{};
    bool     haveLastSync = false;
};

std::atomic<coop::net::Session*> g_session{nullptr};

// g_atvs is GAME-THREAD ONLY: Install / Tick / OnReliable (event_feed drain) /
// QueueConnectBroadcastForSlot / OnDisconnect all run on the game thread, serially within the
// net-pump, so no synchronization is needed (the drain + the sync ticks never overlap). Tick
// mutates only entry FIELDS; structural inserts/erases happen in the hub pass's
// HubPassComplete (scan_hub::Tick runs before this module's Tick in the pump order, and both
// are GT-serial) and OnReliable (the drain, before the ticks).
std::unordered_map<std::wstring, AtvEntry> g_atvs;
size_t   g_lastLogCount = SIZE_MAX;
uint64_t g_lastLogHash  = 0;
bool     g_installed    = false;  // latch the one-time index+log (Install is the per-tick ensure path)

// ---- v77 runtime-spawned-ATV identity (GAP B) ---------------------------------------------------
// PREMISE CORRECTED 2026-08-29 (whole-pak census, docs/vehicles/ATV.md 11.4). This block used to say
// "a bought ATV is delivered ONLY on the host"; that is FALSE -- nothing sells an ATV (473 list_store
// rows + 189 craft recipes, zero hits). The code below never depended on it: its predicate is the
// broader and correct "an ATV first seen after the baseline window", and such an ATV really exists --
// list_props row 'atv' carries spawnAsObject = ATV_C with hidden = false, resolved by lib.PropToObject
// and spawned through spawnPropThroughGamemode from ui_spawnmenu. So this lane STAYS (an earlier note
// gated it for RULE-2 deletion on this census; the census cancelled the deletion, not the lane).
// Such an ATV has NO save-twin on the other peers, and its OWN int_save Key is minted RANDOM per peer
// (the kerfur trap) -> useless cross-peer. The host gives each one a SYNTHETIC stable wire key
// ("coopatv#N") and announces it (AtvSpawn); clients fresh-spawn a native AATV_C under that key.
// Default SAVE-PLACED ATVs (deterministic key, both peers loaded them) stay on the real-key path.
std::unordered_set<std::wstring>        g_savePlacedKeys;    // HOST: real keys seen BEFORE any client connected = save-placed (a joiner loads them)
std::unordered_set<void*>               g_savePlacedActors;  // HOST: ATV ACTORS present before any client connected -- so a save ATV that mints its UCS key LATE (after connect) is recognised by its actor, not misread as a runtime spawn (-> client dupe)
std::unordered_map<void*, std::wstring> g_synthForActor;     // actor -> synthetic wire key (host runtime-spawned + client mirror)
uint32_t                                g_synthCounter = 0;  // HOST: monotonic synth-key id

const wchar_t* const kSynthPrefix = L"coopatv#";  // distinguishes synth keys from real ATV keys ("atv"/base64)

bool IsSynthKey(const std::wstring& k) {
    return k.compare(0, 8, kSynthPrefix) == 0;  // "coopatv#" is 8 chars
}

// Fill a WireClassName from a wide class name (ASCII; VOTV class names are ASCII). Truncates at 63.
void FillWireClassName(coop::net::WireClassName& out, const std::wstring& name) {
    out.len = 0;
    for (size_t i = 0; i < name.size() && i < sizeof(out.data); ++i)
        out.data[out.len++] = static_cast<char>(name[i]);
}
std::wstring WireClassNameToString(const coop::net::WireClassName& in) {
    std::wstring s;
    const uint8_t n = in.len <= sizeof(in.data) ? in.len : static_cast<uint8_t>(sizeof(in.data));
    s.reserve(n);
    for (uint8_t i = 0; i < n; ++i) s.push_back(static_cast<wchar_t>(static_cast<unsigned char>(in.data[i])));
    return s;
}

// HOST: announce a runtime ATV so clients (that have no save-twin) fresh-spawn a native mirror.
// slot < 0 -> broadcast to all (a NEW ATV appearing mid-session); slot >= 0 -> send to one
// joiner (connect-snapshot). Reads the actor's class + current pose.
void SendAtvSpawn(const std::wstring& synthKey, void* actor, int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !actor) return;
    void* cls = R::ClassOf(actor);
    if (!cls) return;
    FVector loc; FRotator rot;
    if (!A::GetRootTransform(actor, loc, rot)) return;
    coop::net::AtvSpawnPayload p{};
    WireKeyFromString(synthKey, p.synthKey);
    FillWireClassName(p.className, R::ToString(R::NameOf(cls)));
    p.x = loc.X; p.y = loc.Y; p.z = loc.Z;
    p.pitch = rot.Pitch; p.yaw = rot.Yaw; p.roll = rot.Roll;
    if (slot < 0) s->SendReliable(coop::net::ReliableKind::AtvSpawn, &p, sizeof(p));
    else          s->SendReliableToSlot(slot, coop::net::ReliableKind::AtvSpawn, &p, sizeof(p));
}

void SendAtvDestroy(const std::wstring& synthKey) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    coop::net::AtvDestroyPayload p{};
    WireKeyFromString(synthKey, p.synthKey);
    s->SendReliable(coop::net::ReliableKind::AtvDestroy, &p, sizeof(p));
}

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// True iff THIS peer's local player is currently seated in `actor` according to the local engine.
bool IsLocalOccupant(void* actor, void* localPlayer) {
    return localPlayer && A::IsDriven(actor) && A::GetOccupantPlayer(actor) == localPlayer;
}

// True iff THIS peer can claim or currently holds driver authority.
// Prevents claiming driver authority if another network peer is already seated (fixes double-mount races).
bool CanClaimOrIsDriver(void* actor, void* localPlayer, uint8_t occupantSlot, uint8_t localSlot) {
    if (!IsLocalOccupant(actor, localPlayer)) return false;
    return occupantSlot == 0xFF || occupantSlot == localSlot;
}

// True iff THIS peer's local player is currently grav-hand GRABBING `actor` (carrying it in the
// air like an object -- NOT seated). The ATV has no grabbed/held flag of its own (isDriven stays
// false, Player stays null during a grab -- those are written only on the seating path), so the
// grabber identity lives entirely on the player side: mainPlayer.grabbing_actor / holding_actor.
// Dual-field test (mirrors local_streams.cpp's held-prop discipline): the light PhysicsHandle grab
// stamps grabbing_actor; we also accept holding_actor so the predicate can't be wrong-footed by
// which field the engine populates for the ATV's simulating root.
bool IsLocalGrabber(void* actor, void* localPlayer) {
    if (!localPlayer || !actor) return false;
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (!ue_wrap::engine::ReadMainPlayerGrabState(localPlayer, gs)) return false;
    return gs.grabbingActor == actor || gs.holdingActor == actor;
}

// POSE AUTHORITY: this peer STREAMS `actor` -- it drives it or carries it. Say which predicate you
// mean at every site; this one is NOT tick ownership (see OwnsTickFor below).
bool IsPoseAuthor(void* actor, void* localPlayer, uint8_t occupantSlot, uint8_t localSlot) {
    return CanClaimOrIsDriver(actor, localPlayer, occupantSlot, localSlot) ||
           IsLocalGrabber(actor, localPlayer);
}

// TICK OWNERSHIP: exactly one peer runs an ATV's brain. The peer that authors it, else the HOST --
// MTA's CUnoccupiedVehicleSync election. This is a DIFFERENT SET from pose authority: on the host
// with nobody driving, IsPoseAuthor is false and this is true. Everyone else runs the rig with its
// brain off, which is what keeps the accumulators, applyWheelTorque and the hit-authored damage on
// one machine while the physics still runs on all of them.
bool OwnsTickFor(bool isPoseAuthor, bool isHost, uint8_t authorSlot) {
    return isPoseAuthor || (isHost && authorSlot == 0xFF);
}

// Fill an AtvStatePayload from a live ATV read. False if the transform read fails. `grabbed` marks
// the authority as the grav-hand grabber (stateBits bit2). `authored` marks that SOME peer is
// actively driving/grabbing this ATV (bit3) -- set on the connect-snapshot so a joiner freezes only
// authored ATVs and leaves idle ones physics-on + grabbable (a live stream is always from an
// authority, so passing authored=true there is just consistent).
bool ReadPayload(void* actor, const std::wstring& key, uint8_t occupantSlot, uint8_t authorSlot,
                 bool adopt, coop::net::AtvStatePayload& p, bool grabbed = false) {
    FVector loc; FRotator rot;
    if (!A::GetRootTransform(actor, loc, rot)) return false;
    FVector lin{}, ang{};
    // Best-effort: a failed read leaves zeros, which is the honest value for "we could not measure
    // it" and is also what a body at rest reports.
    ue_wrap::engine::GetActorRootPhysicsVelocity(actor, lin, ang);
    std::memset(&p, 0, sizeof(p));
    WireKeyFromString(key, p.key);
    p.x = loc.X; p.y = loc.Y; p.z = loc.Z;
    p.pitch = rot.Pitch; p.yaw = rot.Yaw; p.roll = rot.Roll;
    p.linVelX = lin.X; p.linVelY = lin.Y; p.linVelZ = lin.Z;
    p.angVelX = ang.X; p.angVelY = ang.Y; p.angVelZ = ang.Z;
    p.occupantSlot = occupantSlot;
    p.authorSlot   = authorSlot;
    uint8_t sb = 0;
    if (A::IsDriven(actor)) sb |= 0x1;
    if (A::GetBrake(actor)) sb |= 0x2;
    if (grabbed)            sb |= 0x4;
    p.stateBits = sb;
    p.adopt = adopt ? 1 : 0;
    return true;
}

float Len(const FVector& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }

uint64_t g_warps = 0;   // diagnostic counters -- a corrector nobody can see is a corrector nobody
uint64_t g_corrs = 0;   // can falsify (the instrument-blindness lesson)

// THE CORRECTOR. Called ONLY on packet arrival for an ATV this peer does not author -- there is no
// per-frame mirror work at all any more. The body simulates between packets; we bias its velocity
// so it converges, and cut to the authority's pose when it is too far gone to converge gracefully.
//
// MTA shape with one deliberate divergence (RULE 2026-05-28). CNetAPI::ReadVehiclePuresync writes
// the wire velocity HARD every packet -- we do that. CClientVehicle::UpdateTargetPosition:3896
// then nudges the TRANSFORM by a per-frame slice of the position error; we bias VELOCITY instead,
// because their vehicle is one rigid body and AATV_C is a five-body constraint rig: a per-frame
// root nudge stretches sus_*/ax_* by the slice every frame, which is the same mechanism as the
// defect this whole model exists to remove. Letting the solver keep the rig rigid and steering it
// by velocity is the only correction that leaves the suspension free to do its own job.
void ApplyCorrection(AtvEntry& e, const coop::net::AtvStatePayload& p, bool snap) {
    FVector cur; FRotator curRot;
    if (!A::GetRootTransform(e.actor, cur, curRot)) return;

    const FVector wirePos{ p.x, p.y, p.z };
    const FRotator wireRot{ p.pitch, p.yaw, p.roll };
    const FVector wireLin{ p.linVelX, p.linVelY, p.linVelZ };
    const FVector wireAng{ p.angVelX, p.angVelY, p.angVelZ };

    const FVector err{ wirePos.X - cur.X, wirePos.Y - cur.Y, wirePos.Z - cur.Z };
    const float dist  = Len(err);
    const float warpD = kWarpBaseCm + kWarpPerSpeedS * Len(wireLin);
    const float dPitch = std::fabs(ue_wrap::NormalizeAxis(wireRot.Pitch - curRot.Pitch));
    const float dYaw   = std::fabs(ue_wrap::NormalizeAxis(wireRot.Yaw   - curRot.Yaw));
    const float dRoll  = std::fabs(ue_wrap::NormalizeAxis(wireRot.Roll  - curRot.Roll));
    const float dAng   = dPitch > dYaw ? (dPitch > dRoll ? dPitch : dRoll) : (dYaw > dRoll ? dYaw : dRoll);

    // The tests are INVERTED on purpose, MTA's own idiom (CClientVehicle.cpp:3905's comment): a
    // comparison against NaN is always false, so writing them this way makes a NaN WARP rather
    // than quietly feed a corrective velocity computed from garbage.
    if (snap || !(dist <= warpD) || !(dAng <= kWarpAngleDeg)) {
        A::TeleportRig(e.actor, wirePos, wireRot);
        ue_wrap::engine::SetActorRootPhysicsVelocity(e.actor, wireLin, wireAng);
        ++g_warps;
        return;
    }

    FVector lin = wireLin;
    if (dist > kCorrDeadbandCm) {
        FVector corr{ err.X / kCorrWindowS, err.Y / kCorrWindowS, err.Z / kCorrWindowS };
        const float mag = Len(corr);
        if (mag > kCorrMaxCmS) {
            const float k = kCorrMaxCmS / mag;
            corr.X *= k; corr.Y *= k; corr.Z *= k;
        }
        lin.X += corr.X; lin.Y += corr.Y; lin.Z += corr.Z;
    }
    // Rotation gets NO continuous corrective term in this commit: mapping a rotator delta onto an
    // angular-velocity vector is only exact for small aligned deltas, and an ATV on the ground
    // takes its orientation from the terrain it is standing on once its position and velocity
    // agree. Orientation divergence is caught by the kWarpAngleDeg arm above instead -- one
    // mechanism, measurable, rather than a term whose gain we would be guessing.
    ue_wrap::engine::SetActorRootPhysicsVelocity(e.actor, lin, wireAng);
    ++g_corrs;
}

// The idle syncer's change gate (CUnoccupiedVehicleSync::WriteVehicleInformation). Returns true
// iff this ATV is worth a packet -- it moved, turned, or is moving. Updates the baseline when it
// answers yes, so a slow drift accumulates into a send instead of being repeatedly rounded away.
bool IdleWorthSending(AtvEntry& e) {
    FVector loc; FRotator rot;
    if (!A::GetRootTransform(e.actor, loc, rot)) return false;
    FVector lin{}, ang{};
    ue_wrap::engine::GetActorRootPhysicsVelocity(e.actor, lin, ang);
    bool send = !e.haveLastSync || Len(lin) > kIdleMovingCmS;
    if (!send) {
        const FVector d{ loc.X - e.lastSyncPos.X, loc.Y - e.lastSyncPos.Y, loc.Z - e.lastSyncPos.Z };
        send = Len(d) > kIdleMovedCm ||
               std::fabs(ue_wrap::NormalizeAxis(rot.Pitch - e.lastSyncRot.Pitch)) > kIdleTurnedDeg ||
               std::fabs(ue_wrap::NormalizeAxis(rot.Yaw   - e.lastSyncRot.Yaw))   > kIdleTurnedDeg ||
               std::fabs(ue_wrap::NormalizeAxis(rot.Roll  - e.lastSyncRot.Roll))  > kIdleTurnedDeg;
    }
    if (send) { e.lastSyncPos = loc; e.lastSyncRot = rot; e.haveLastSync = true; }
    return send;
}

// Write the brain latch only on a real change -- SetActorTickEnabled is a UFunction dispatch, and
// this runs per ATV per tick.
void SetBrain(AtvEntry& e, bool on) {
    if (e.brainKnown && e.brainOn == on) return;
    A::SetBrainEnabled(e.actor, on);
    e.brainOn = on;
    e.brainKnown = true;
}

// ---- the COLLISION half of "brains off" ---------------------------------------------------
// SetActorTickEnabled does not reach a ComponentHit delegate: it is dispatched by the physics
// scene. `[V]` all seven of ATV_C's reach real authored state -- impulse() subtracts
// |NormalImpulse|/500000*2*getBumperMult() from `health` and calls explode() at <=0, processTire()
// burns tire durability and ejectWheel()s at 0, and the Capsule one pops a lib_C::addHint at the
// local player. With the rig now simulating on every peer, a non-owner running them would blow up
// a vehicle whose authority still has it. So they are cancelled PRE-dispatch on a non-owner.
//
// The predicate cannot read g_atvs: the interceptor contract does not promise the game thread.
// Instead Tick publishes the small set of ATVs THIS peer owns the tick for into an atomic array,
// and the callback is a pointer scan over it. DEFAULT IS CANCEL, which is the safe direction --
// a stale/empty set costs the authority some collision damage (a loss, and a quiet one), while
// the opposite fails toward a mirror destroying itself.
constexpr int kMaxOwnedPublished = 16;
std::atomic<void*> g_ownedAtvs[kMaxOwnedPublished];
std::atomic<bool>  g_guardActive{false};   // false in single-player: the game must keep its damage
std::atomic<unsigned long long> g_hitCancelled{0};
std::atomic<unsigned long long> g_hitAllowed{0};
bool g_hitGuardArmed = false;              // all 7 delegates registered -- else the lane runs INERT

void PublishOwned(void** owned, int n) {
    for (int i = 0; i < kMaxOwnedPublished; ++i)
        g_ownedAtvs[i].store(i < n ? owned[i] : nullptr, std::memory_order_release);
}

bool OnAtvHitPre(void* self, void* /*params*/) {
    if (!g_guardActive.load(std::memory_order_acquire)) return false;  // not in a session: never suppress
    for (int i = 0; i < kMaxOwnedPublished; ++i) {
        void* p = g_ownedAtvs[i].load(std::memory_order_acquire);
        if (!p) break;
        if (p == self) { g_hitAllowed.fetch_add(1, std::memory_order_relaxed); return false; }
    }
    g_hitCancelled.fetch_add(1, std::memory_order_relaxed);
    return true;   // cancel-on-true: the BndEvt stub never jumps into the ubergraph
}

void InstallHitGuard() {
    if (g_hitGuardArmed) return;
    void* fns[8] = {};
    const int n = A::ResolveHitDelegates(fns, 8);
    int ok = 0;
    for (int i = 0; i < n; ++i)
        if (ue_wrap::game_thread::RegisterInterceptor(fns[i], &OnAtvHitPre)) ++ok;
    if (ok == 7) {
        g_hitGuardArmed = true;
        UE_LOGI("atv: hit guard armed -- 7/7 ComponentHit delegates intercepted (a non-owner cannot "
                "author damage/explode/ejectWheel)");
    } else {
        // FAIL CLOSED. Without all seven we will not run the simulate-and-correct model at all:
        // Tick leaves every ATV's brain ON and mirrors nothing, so peers diverge visibly rather
        // than one of them silently destroying a vehicle the other still has.
        UE_LOGE("atv: hit guard NOT armed (%d/7 resolved, %d/7 registered) -- ATV sync stays INERT "
                "this session; the interceptor table may be full (kMaxInterceptors)", n, ok);
    }
}

// ---- R-2 shared-scan hub consumer (design: votv-shared-scan-hub-R2-DESIGN-2026-08-23.md).
// The per-module walk is RETIRED; the hub's shared sliced pass drives these callbacks --
// PRESERVING interp/sender state for keys that persist (only actor/idx are updated), and the
// v77 identity classification verbatim: a save-placed ATV (real key, both peers loaded it)
// keeps its real key; a HOST-side mid-session PURCHASED ATV gets a synthetic key + an
// AtvSpawn announce so clients fresh-spawn it. Note the join edge: a spawn landing inside
// the <=1-pass (~2 s) index staleness window at a join is announced on the NEXT pass, when
// the joiner is already connected -- the announce reaches it; no re-announce machinery needed.
uint32_t g_indexGen = 0;  // world gen of the last completed pass (stale-gen index = EMPTY)
bool IndexCurrent() { return g_indexGen == ue_wrap::world_identity::Generation(); }
struct ScanFound { std::wstring wireKey; void* obj; int32_t idx; std::wstring realKey; };
std::vector<ScanFound> g_scanFound;   // pass scratch (GT-only)
bool g_scanIsHost = false;            // pass context, captured at pass begin
bool g_scanCapturing = false;

void HubPassBegin(void*, bool) {
    g_scanFound.clear();
    auto* s = g_session.load(std::memory_order_acquire);
    g_scanIsHost = s && s->role() == coop::net::Role::Host;
    const bool isHost = g_scanIsHost;
    // Baseline-capture window: before any client is connected, EVERY keyed ATV the host has is
    // save-placed (a joiner will load it from the save). After a client connects, a newly-appearing
    // key is a runtime spawn. (Accumulated -- not a single-frame latch -- so a default ATV that is
    // a few seconds slow to mint its UCS key still lands in the save-set before the first joiner.)
    g_scanCapturing = isHost && (!s || !s->connected());
}

void HubMatch(void*, void* obj) {
    const bool isHost = g_scanIsHost;
    const bool capturing = g_scanCapturing;
    auto& found = g_scanFound;
    {
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) return;  // skip CDO
        if (!R::IsLive(obj)) return;
        if (capturing) g_savePlacedActors.insert(obj);  // capture the ACTOR (even before it mints its key)
        std::wstring realKey = A::GetKeyString(obj);
        if (realKey.empty() || realKey == L"None") return;  // not yet keyed -- the next pass picks it up
        std::wstring wireKey;
        auto sf = g_synthForActor.find(obj);
        if (sf != g_synthForActor.end()) {
            wireKey = sf->second;                              // already synth (host runtime-spawn / client mirror)
        } else if (capturing) {
            g_savePlacedKeys.insert(realKey);                 // baseline: a save-placed ATV
            wireKey = realKey;
        } else if (isHost && g_savePlacedKeys.find(realKey) == g_savePlacedKeys.end() &&
                   g_savePlacedActors.find(obj) == g_savePlacedActors.end()) {
            // HOST: a mid-session ATV not in the save-set = a RUNTIME-SPAWNED ATV (spawn menu ->
            // list_props row 'atv' -> spawnAsObject = ATV_C). Mint a synthetic stable wire key +
            // announce so the clients (no save-twin) fresh-spawn a native mirror. (Its own int_save
            // key is random per peer -- never used cross-peer.)
            wireKey = std::wstring(kSynthPrefix) + std::to_wstring(++g_synthCounter);
            g_synthForActor[obj] = wireKey;
            SendAtvSpawn(wireKey, obj, /*slot*/ -1);          // broadcast to all connected clients
            UE_LOGI("atv: runtime-spawned ATV detected -- synthKey='%ls' class='%ls' (host-announced AtvSpawn)",
                    wireKey.c_str(), R::ToString(R::NameOf(R::ClassOf(obj))).c_str());
        } else {
            wireKey = realKey;                                 // save-placed default ATV (both peers have it)
        }
        found.push_back({ std::move(wireKey), obj, R::InternalIndexOf(obj), std::move(realKey) });
    }
}

size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    const bool isHost = g_scanIsHost;
    auto& found = g_scanFound;
    // Drop entries whose ATV vanished. A HOST synth (runtime) ATV that's gone -> AtvDestroy so the
    // clients tear down their fresh-spawned mirror; clean its synth map entry.
    //   FULL pass: `found` is authoritative (it covered [0,N)) -> drop any entry NOT in `found`.
    //   TAIL pass: `found` is only the new tail -> a persistent live entry is NOT in `found`; prune by
    //   IsLiveByIndex instead (drop only entries whose actor actually died). The synth AtvDestroy folds
    //   into this prune (a gone host-synth -> announce + clean its synth map) -- same teardown, different
    //   liveness oracle. Either way the same set is removed (a tail vanish-drop is index-cheap, O(index)).
    for (auto it = g_atvs.begin(); it != g_atvs.end();) {
        bool keep;
        if (isFull) {
            keep = false;
            for (auto& f : found) if (f.wireKey == it->first) { keep = true; break; }
        } else {
            keep = R::IsLiveByIndex(it->second.actor, it->second.idx);
        }
        if (keep) { ++it; continue; }
        if (IsSynthKey(it->first)) {
            if (isHost) SendAtvDestroy(it->first);
            if (it->second.actor) g_synthForActor.erase(it->second.actor);
        }
        it = g_atvs.erase(it);
    }
    // Update/add (preserve interp/sender state for an existing wire key -- only actor/idx are touched).
    for (auto& f : found) {
        AtvEntry& e = g_atvs[f.wireKey];
        e.actor = f.obj;
        e.idx   = f.idx;
    }
    g_indexGen = worldGen;
    // Recompute the keys-hash over the WHOLE index (cheap, O(index)) -- on a tail pass `found` is only the
    // new arrivals, so hashing just `found` would lose the persistent keys + thrash the dedup log.
    uint64_t keysHash = 0;
    for (auto& kv : g_atvs) keysHash ^= FnvKey(kv.first);
    if (g_atvs.size() != g_lastLogCount || keysHash != g_lastLogHash) {
        g_lastLogCount = g_atvs.size();
        g_lastLogHash  = keysHash;
        UE_LOGI("atv: index rebuilt -- %zu live ATV(s), keysHash=0x%016llX (%s pass, +%zu new) "
                "(compare host vs client for cross-peer Key stability)",
                g_atvs.size(), static_cast<unsigned long long>(keysHash),
                isFull ? "full" : "tail", found.size());
    }
    g_scanFound.clear();
    return g_atvs.size();
}

// A peer whose seat/authorship we are holding has left (or been REPLACED -- slots recycle
// lowest-free, so a slot can go X->Y with no absence in between, which is why this hangs off the
// ledger's row transition and not off any per-slot boolean of ours). Release every ATV they held.
//
// This is not tidiness, it is the failure mode this commit INTRODUCES: `authorSlot` is what elects
// the host as an idle ATV's syncer, so an authorSlot stuck on a departed peer means nobody ever
// runs that ATV's brain again and nobody corrects it. Before this commit a stuck occupantSlot only
// blocked mounting.
void OnSlotReplaced(int slot, const coop::roster_ledger::Row& /*outgoing*/,
                    const coop::roster_ledger::Row& /*incoming*/) {
    if (slot < 0 || slot > 0xFE) return;
    const uint8_t s8 = static_cast<uint8_t>(slot);
    int freed = 0;
    for (auto& kv : g_atvs) {
        AtvEntry& e = kv.second;
        if (e.occupantSlot == s8) { e.occupantSlot = 0xFF; ++freed; }
        if (e.authorSlot   == s8) { e.authorSlot   = 0xFF; ++freed; }
    }
    if (freed > 0)
        UE_LOGI("atv: slot %d departed -- freed %d ATV seat/author reservation(s)", slot, freed);
}

void SubscribeDepartures() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    coop::roster_ledger::SubscribeSlotReplaced(&OnSlotReplaced);
}

void RegisterWithScanHub() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        "atv", nullptr, &A::EnsureResolved, &A::IsAtv,
        &HubPassBegin, &HubMatch, &HubPassComplete, /*settleScans*/ 15});
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // Install is the per-tick idempotent "ensure installed" path (net_pump re-calls it every tick
    // until the class loads) -- latch the one-time initial index + log so we don't full-walk the
    // GUObjectArray + spam the log every tick (the Tick's throttled rebuild owns ongoing indexing).
    if (!g_installed && A::EnsureResolved()) {
        RegisterWithScanHub();  // the hub builds the index on its own cadence
        InstallHitGuard();      // the seven ComponentHit interceptors -- Tick refuses to run without them
        SubscribeDepartures();  // a departed author must not hold an ATV hostage
        g_installed = true;
    }
}

void OnReliable(const coop::net::AtvStatePayload& payload, uint8_t /*senderPeerSlot*/) {
    std::wstring key = StringFromWireKey(payload.key);
    if (key.empty()) { UE_LOGW("atv: OnReliable empty key -- dropping"); return; }
    if (!A::EnsureResolved()) return;
    if (!IndexCurrent()) return;  // audit W-2: a stale-gen index holds another world's ATVs (R-1 class); the 20 Hz stream re-sends
    // NaN/Inf guard before the kinematic engine writes (event_feed also guards; defensive).
    if (!std::isfinite(payload.x) || !std::isfinite(payload.y) || !std::isfinite(payload.z) ||
        !std::isfinite(payload.pitch) || !std::isfinite(payload.yaw) || !std::isfinite(payload.roll)) {
        UE_LOGW("atv: OnReliable non-finite pose -- dropping key='%ls'", key.c_str());
        return;
    }
    auto it = g_atvs.find(key);
    if (it == g_atvs.end()) return;  // not indexed yet -- the throttled rebuild will pick it up
    AtvEntry& e = it->second;
    if (!R::IsLiveByIndex(e.actor, e.idx)) return;

    void* localPlayer = coop::players::Registry::Get().Local();
    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();

    // SIMULTANEOUS-MOUNT TIE-BREAK -- this MUST precede the authority early-return below, which reads
    // e.occupantSlot. Two peers mounting inside one round-trip both see a free seat and both elect
    // themselves; each then treats the other's stream as an echo to ignore, and the double-drive the
    // seat gate exists to prevent becomes PERMANENT. Lower slot wins: a total order both peers already
    // hold, no wire cost, deterministic. Only a genuine claim (not 0xFF) can take the seat, and only
    // from a peer we outrank -- so this can never demote us to a peer that is merely echoing.
    if (payload.occupantSlot != 0xFF && payload.occupantSlot < localSlot &&
        IsLocalOccupant(e.actor, localPlayer) &&
        (e.occupantSlot == 0xFF || e.occupantSlot == localSlot)) {
        UE_LOGI("atv: seat contention on '%ls' -- slot %u outranks local slot %u; yielding pose authority",
                key.c_str(), static_cast<unsigned>(payload.occupantSlot),
                static_cast<unsigned>(localSlot));
        e.occupantSlot = payload.occupantSlot;   // we are now a mirror; Tick's authority-lost edge fires
    }

    // If WE are the legitimate authority of this ATV (driving OR grav-hand grabbing it), ignore the incoming
    // pose so a relayed/echoed copy can't fight our live driving/carrying.
    if (IsPoseAuthor(e.actor, localPlayer, e.occupantSlot, localSlot)) return;

    // Track the incoming seat and author. `authorSlot` is what elects the host as the idle syncer
    // (0xFF = nobody is driving or carrying it).
    e.occupantSlot = payload.occupantSlot;
    e.authorSlot   = payload.authorSlot;

    // A connect-snapshot warps verbatim (the joiner has no business converging smoothly onto a
    // world it has not seen yet); a live packet corrects. Either way the rig keeps simulating --
    // there is no freeze branch here any more, and that is the point: the old code's "adopt an
    // idle ATV" and "mirror an authored ATV" cases needed different physics states, and the state
    // machine between them is where the release defect lived.
    ApplyCorrection(e, payload, /*snap*/ payload.adopt != 0);
}

void OnAtvRelease(const coop::net::AtvReleasePayload& payload, uint8_t /*senderPeerSlot*/) {
    std::wstring key = StringFromWireKey(payload.key);
    if (key.empty()) { UE_LOGW("atv: OnAtvRelease empty key -- dropping"); return; }
    if (!A::EnsureResolved()) return;
    if (!IndexCurrent()) return;  // audit W-2: a stale-gen index holds another world's ATVs
    auto it = g_atvs.find(key);
    if (it == g_atvs.end()) return;  // not indexed yet -- nothing whose author we could clear
    AtvEntry& e = it->second;
    if (!R::IsLiveByIndex(e.actor, e.idx)) return;

    void* localPlayer = coop::players::Registry::Get().Local();
    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();

    // If WE author it, ignore a stale or echoed release so it cannot perturb our live drive/carry.
    if (IsPoseAuthor(e.actor, localPlayer, e.occupantSlot, localSlot)) return;

    // THE WHOLE HANDLER. The sender stopped authoring; free the seat and the author. No physics
    // write, no un-freeze, no inherited velocity -- nothing was ever frozen and every AtvState
    // already carried the velocity. On the host this election is what makes it the ATV's idle
    // syncer on the very next tick, so the correction stream continues instead of ending.
    e.occupantSlot = 0xFF;
    e.authorSlot   = 0xFF;
    UE_LOGI("atv: OnAtvRelease key='%ls' -- author cleared (the rig kept simulating throughout)",
            key.c_str());
}

void OnAtvSpawn(const coop::net::AtvSpawnPayload& payload, uint8_t /*senderPeerSlot*/) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == coop::net::Role::Host) return;  // client-only (the host owns the real ATV)
    if (!A::EnsureResolved()) return;
    std::wstring synthKey = StringFromWireKey(payload.synthKey);
    if (synthKey.empty()) { UE_LOGW("atv: OnAtvSpawn empty synthKey -- dropping"); return; }
    if (g_atvs.find(synthKey) != g_atvs.end()) return;  // already spawned (re-announce / connect dup) -- idempotent
    if (!std::isfinite(payload.x) || !std::isfinite(payload.y) || !std::isfinite(payload.z) ||
        !std::isfinite(payload.pitch) || !std::isfinite(payload.yaw) || !std::isfinite(payload.roll)) {
        UE_LOGW("atv: OnAtvSpawn non-finite pose synthKey='%ls' -- dropping", synthKey.c_str());
        return;
    }
    std::wstring className = WireClassNameToString(payload.className);
    if (className.empty()) { UE_LOGW("atv: OnAtvSpawn empty className synthKey='%ls' -- dropping", synthKey.c_str()); return; }
    const FVector loc{ payload.x, payload.y, payload.z };
    const FRotator rot{ payload.pitch, payload.yaw, payload.roll };
    void* spawned = A::SpawnMirror(className, loc, rot);  // physics LEFT ON -- a native idle grabbable ATV
    if (!spawned) {
        UE_LOGW("atv: OnAtvSpawn SpawnMirror failed synthKey='%ls' class='%ls'", synthKey.c_str(), className.c_str());
        return;
    }
    AtvEntry e{};
    e.actor = spawned;
    e.idx = R::InternalIndexOf(spawned);
    e.isClientSpawnedMirror = true;
    g_atvs[synthKey] = std::move(e);
    g_synthForActor[spawned] = synthKey;
    UE_LOGI("atv: spawned runtime-ATV mirror synthKey='%ls' class='%ls' actor=%p loc=(%.0f, %.0f, %.0f)",
            synthKey.c_str(), className.c_str(), spawned, loc.X, loc.Y, loc.Z);
}

void OnAtvDestroy(const coop::net::AtvDestroyPayload& payload, uint8_t /*senderPeerSlot*/) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == coop::net::Role::Host) return;  // client-only
    std::wstring synthKey = StringFromWireKey(payload.synthKey);
    if (synthKey.empty()) return;
    if (!IndexCurrent()) return;  // audit W-2: the pass prunes a dead-world entry itself
    auto it = g_atvs.find(synthKey);
    if (it == g_atvs.end()) return;
    void* actor = it->second.actor;
    if (it->second.isClientSpawnedMirror) A::DestroyMirror(actor);  // K2_DestroyActor our fresh spawn
    if (actor) g_synthForActor.erase(actor);
    g_atvs.erase(it);
    UE_LOGI("atv: destroyed runtime-ATV mirror synthKey='%ls'", synthKey.c_str());
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;  // host-only snapshot
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    // R-2: the forced sync rebuild is gone -- the hub keeps the index <=1 pass (~2 s) fresh;
    // a spawn inside that window is announced on the next pass to the (by then connected)
    // joiner -- see the hub-consumer block note.
    void* localPlayer = coop::players::Registry::Get().Local();
    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
    int sent = 0, spawns = 0;
    for (auto& kv : g_atvs) {
        AtvEntry& e = kv.second;
        if (!R::IsLiveByIndex(e.actor, e.idx)) continue;
        // GAP B: a PURCHASED (synth-keyed) ATV -- the joiner has NO save-twin of it -> announce it
        // FIRST so the joiner fresh-spawns it. Same Normal lane as AtvState, so the AtvSpawn arrives
        // before the pose below (spawn-then-pose, in order).
        if (IsSynthKey(kv.first)) { SendAtvSpawn(kv.first, e.actor, peerSlot); ++spawns; }
        // The joiner gets pose AND velocity, and warps to it (adopt=1). Nothing is frozen on
        // either side, so the old "authored" boolean -- which existed only to tell a joiner
        // whether to freeze -- has no consumer. What the joiner needs is WHO holds it: if the host
        // itself is the author, that is localSlot; otherwise e.authorSlot already names the peer
        // (or 0xFF, which makes the host its syncer). Carrying the velocity is a real improvement
        // for a mid-join: an ATV in the air at the moment someone joins now arrives moving and
        // lands, instead of hanging where it was.
        const bool hostAuthors = IsPoseAuthor(e.actor, localPlayer, e.occupantSlot, localSlot);
        const uint8_t authorSlot = hostAuthors ? localSlot : e.authorSlot;
        coop::net::AtvStatePayload p{};
        if (!ReadPayload(e.actor, kv.first, e.occupantSlot, authorSlot, /*adopt*/true, p)) continue;
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::AtvState, &p, sizeof(p));
        ++sent;
    }
    UE_LOGI("atv: connect-snapshot -- sent %d ATV pose(s) (%d runtime-ATV announce(s)) to slot %d (of %zu indexed)",
            sent, spawns, peerSlot, g_atvs.size());
}

void Tick() {
    // EVERY early return below means "this module is not currently deciding who owns what", and in
    // that state the collision guard MUST be inert -- it suppresses damage by DEFAULT, so leaving
    // it armed against a set we are no longer refreshing would silently make the local ATV
    // invulnerable. Disarm first; the paths that earn it re-arm at the bottom.
    struct DisarmUnlessArmed {
        bool armed = false;
        ~DisarmUnlessArmed() { if (!armed) g_guardActive.store(false, std::memory_order_release); }
    } scope;

    if (!A::EnsureResolved()) return;
    RegisterWithScanHub();  // safety net for any order where Tick precedes Install
    if (!IndexCurrent()) return;  // index belongs to a dead world -- wait for the hub's next pass
    auto* s = g_session.load(std::memory_order_acquire);

    if (!s || !s->connected()) return;
    void* localPlayer = coop::players::Registry::Get().Local();
    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
    const uint64_t nowMs = NowMs();

    // FAIL CLOSED (the hit guard): without all seven ComponentHit interceptors a non-owner would
    // author damage on a rig we are about to leave simulating -- so we do not leave it simulating.
    // Every ATV keeps its brain, nothing is mirrored, and the ERROR line at Install says why.
    if (!g_hitGuardArmed) return;

    const bool isHost = s->role() == coop::net::Role::Host;
    void* owned[kMaxOwnedPublished];
    int   ownedN = 0;

    for (auto& kv : g_atvs) {
        AtvEntry& e = kv.second;
        if (!R::IsLiveByIndex(e.actor, e.idx)) continue;

        const bool isDriver  = CanClaimOrIsDriver(e.actor, localPlayer, e.occupantSlot, localSlot);
        const bool isGrabber = !isDriver && IsLocalGrabber(e.actor, localPlayer);  // mutually exclusive
        const bool authority = isDriver || isGrabber;

        // AUTHORITY-LOST edge: we authored it last tick (driver OR grabber) and no longer do.
        // Authority can be lost TWO ways and they need opposite handling, so the edge consults the
        // REASON rather than firing on the transition. A DISMOUNT/UNGRAB genuinely frees the ATV --
        // clear the seat and the author and say so. A YIELD (the OnReliable tie-break: still
        // physically seated, but outranked by a lower slot) means someone ELSE now holds it:
        // clearing the slot there would erase the winner's claim, IsLocalOccupant is still true so
        // we would re-claim next tick and flap, and the release would hand the ATV's authorship
        // back to nobody underneath the peer that just won it.
        // (The v146 release carries no velocity and re-enables no physics -- see OnAtvRelease.)
        const bool yielded = IsLocalOccupant(e.actor, localPlayer) &&
                             e.occupantSlot != 0xFF && e.occupantSlot != localSlot;
        if (e.wasPoseAuthor && !authority && !yielded) {
            e.occupantSlot = 0xFF;
            e.authorSlot   = 0xFF;
            coop::net::AtvReleasePayload rp{};
            WireKeyFromString(kv.first, rp.key);
            s->SendReliable(coop::net::ReliableKind::AtvRelease, &rp, sizeof(rp));
            UE_LOGI("atv: authority released key='%ls' -- author cleared; the host now syncs it idle",
                    kv.first.c_str());
        }
        e.wasPoseAuthor = authority;

        if (authority) {
            if (isDriver) e.occupantSlot = localSlot;   // claim the seat locally
            e.authorSlot = localSlot;
        }

        // TICK OWNERSHIP -- a different question from pose authority, and the only thing that
        // decides whose machine runs this rig's brain.
        const bool ownsTick = OwnsTickFor(authority, isHost, e.authorSlot);
        SetBrain(e, ownsTick);
        if (ownsTick && ownedN < kMaxOwnedPublished) owned[ownedN++] = e.actor;

        if (authority) {
            if (nowMs - e.lastSentMs >= kDriveSendMs) {
                e.lastSentMs = nowMs;
                coop::net::AtvStatePayload p{};
                const uint8_t occSlot = isDriver ? localSlot : uint8_t{0xFF};  // grabber: no seated driver
                if (ReadPayload(e.actor, kv.first, occSlot, localSlot, /*adopt*/false, p, /*grabbed*/isGrabber))
                    s->SendReliable(coop::net::ReliableKind::AtvState, &p, sizeof(p));
            }
        } else if (ownsTick) {
            // THE IDLE SYNCER (host, nobody driving). MTA's CUnoccupiedVehicleSync: a slower
            // cadence AND a change gate, so a parked ATV costs literally nothing while one rolling
            // down a hill still converges on every peer.
            if (nowMs - e.lastSentMs >= kIdleSendMs && IdleWorthSending(e)) {
                e.lastSentMs = nowMs;
                coop::net::AtvStatePayload p{};
                if (ReadPayload(e.actor, kv.first, e.occupantSlot, /*authorSlot*/0xFF, /*adopt*/false, p))
                    s->SendReliable(coop::net::ReliableKind::AtvState, &p, sizeof(p));
            }
        }
        // A mirror does NOTHING here. It is a simulating body, corrected at packet arrival.
    }

    // Publish BEFORE arming, so the guard never runs against a set we have not refreshed.
    PublishOwned(owned, ownedN);
    g_guardActive.store(true, std::memory_order_release);
    scope.armed = true;
}

void OnDisconnect() {
    // Disarm FIRST: from here on nothing publishes an owned set, and a live hit must reach the
    // game (this peer is back to single-player and owns everything).
    g_guardActive.store(false, std::memory_order_release);
    PublishOwned(nullptr, 0);
    for (auto& kv : g_atvs) {
        const bool live = R::IsLiveByIndex(kv.second.actor, kv.second.idx);
        if (kv.second.isClientSpawnedMirror) {
            if (live) A::DestroyMirror(kv.second.actor);   // a fresh-spawned runtime mirror is a coop artifact -> remove
        } else if (live) {
            // Give every surviving ATV its brain back, unconditionally. The old code restored only
            // ATVs it had a `preparedAsMirror` flag for, which is the "missing disconnect restore"
            // the C1 crutch entry names: the flag was cleared by a release, so an ATV that had been
            // mirrored and then released was left in single-player with whatever state it had.
            A::SetBrainEnabled(kv.second.actor, true);
        }
    }
    const size_t n = g_atvs.size();
    g_atvs.clear();
    g_synthForActor.clear();
    g_savePlacedKeys.clear();
    g_savePlacedActors.clear();
    g_synthCounter = 0;
    g_installed = false;  // a new session re-indexes via the next Install (latched again)
    if (n > 0)
        UE_LOGI("atv: OnDisconnect -- cleared %zu ATV(s) (brains restored; runtime mirrors destroyed); "
                "hit guard: %llu cancelled / %llu allowed this session",
                n, static_cast<unsigned long long>(g_hitCancelled.load()),
                static_cast<unsigned long long>(g_hitAllowed.load()));
}

bool OwnsTick(void* actor) {
    if (!actor) return false;
    // Reads the SAME published set the collision guard consults, on purpose: an instrument that
    // recomputes the predicate would agree with itself while disagreeing with the code under test.
    for (int i = 0; i < kMaxOwnedPublished; ++i) {
        void* p = g_ownedAtvs[i].load(std::memory_order_acquire);
        if (!p) return false;
        if (p == actor) return true;
    }
    return false;
}

// Check if an ATV actor is occupied by a remote peer (used to block local mount interactions).
bool IsOccupiedByOther(void* actor, uint8_t* outOccupantSlot) {
    if (!actor) return false;
    if (!IndexCurrent()) return false;   // stale-gen index holds another world's ATVs (audit W-2 class)
    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
    for (const auto& kv : g_atvs) {
        if (kv.second.actor == actor) {
            const uint8_t occ = kv.second.occupantSlot;
            if (occ != 0xFF && occ != localSlot) {
                if (outOccupantSlot) *outOccupantSlot = occ;
                return true;
            }
            return false;
        }
    }
    return false;
}

}  // namespace coop::atv_sync
