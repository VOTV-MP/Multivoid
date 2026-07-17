// ue_wrap/wisp.cpp -- see ue_wrap/wisp.h. Engine access for the VOTV Killer Wisp
// (Akillerwisp_C / FName "killerwisp_C").
//
// All field offsets are resolved from the live class via reflection (FindPropertyOffset)
// rather than hardcoded, so they stay correct across game builds (version-tagging rule).
// The known Alpha 0.9.0-n offsets (CXXHeaderDump/killerwisp.hpp) are kept only as a logged
// fallback if the reflected walk ever fails to find the property.

#include "ue_wrap/actors/wisp.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"       // SetAnimTickAlways (force-mesh-tick)
#include "ue_wrap/core/fname_utils.h"  // StringToFName (the 'fatality' montage section)
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/trace.h"       // LineBlockedStatDyn (the canReach parity trace)
#include "ue_wrap/core/types.h"       // FVector (InGrabRange distance)

#include <atomic>
#include <cmath>
#include <cstdint>

namespace ue_wrap::wisp {
namespace {

namespace R = reflection;
namespace P = profile;

// Resolved once at EnsureResolved, then read-only (g_resolved release/acquire).
std::atomic<bool> g_resolved{false};

void*   g_wispCls       = nullptr;  // killerwisp_C UClass
int32_t g_tryGrabOff    = -1;       // tryGrab       (0x05D8, bool)
int32_t g_grabOff       = -1;       // grab          (0x0600, bool)
int32_t g_killedOff     = -1;       // killed        (0x0618, bool)
int32_t g_playerDmgOff  = -1;       // playerDamaged (0x0635, bool)
int32_t g_harmlessOff   = -1;       // harmless      (0x0658, bool)
int32_t g_targetOff     = -1;       // Target        (0x0610, APawn*)
int32_t g_legLOff       = -1;       // leg_L         (0x0550, UStaticMeshComponent*)
int32_t g_armLOff       = -1;       // arm_L         (0x0558)
int32_t g_legROff       = -1;       // LEG_R         (0x0560)
int32_t g_armROff       = -1;       // arm_R         (0x0568)
void*   g_releaseFn     = nullptr;  // releasePlayer() -- the grab-cancel verb

// Documented Alpha 0.9.0-n fallbacks (CXXHeaderDump/killerwisp.hpp).
constexpr int32_t kTryGrabFallback   = 0x05D8;
constexpr int32_t kGrabFallback      = 0x0600;
constexpr int32_t kKilledFallback    = 0x0618;
constexpr int32_t kPlayerDmgFallback = 0x0635;
constexpr int32_t kHarmlessFallback  = 0x0658;
constexpr int32_t kTargetFallback    = 0x0610;
constexpr int32_t kLegLFallback      = 0x0550;
constexpr int32_t kArmLFallback      = 0x0558;
constexpr int32_t kLegRFallback      = 0x0560;
constexpr int32_t kArmRFallback      = 0x0568;

int32_t ResolveOff(void* cls, const wchar_t* name, int32_t fallback) {
    int32_t off = R::FindPropertyOffset(cls, name);
    if (off < 0) {
        UE_LOGW("wisp: reflected %ls offset not found -- using fallback 0x%04X", name, fallback);
        off = fallback;
    }
    return off;
}

template <typename T>
T ReadAt(void* obj, int32_t off) {
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(obj) + off);
}

}  // namespace

namespace {
// Resolve every killerwisp member from a KNOWN killerwisp_C UClass -- shared by
// EnsureResolved (FindClass path, probe/dev callers) and IsKillerWisp's candidate-class
// self-resolve (no GUObjectArray walk). Returns true once published.
bool ResolveFromClass(void* cls);
}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* cls = R::FindClass(P::name::NpcClass_KillerWisp);  // L"killerwisp_C"
    if (!cls) return false;  // BP class not loaded yet -- caller retries on a later tick
    return ResolveFromClass(cls);
}

namespace {
bool ResolveFromClass(void* cls) {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    // FName lookups are case-SENSITIVE (the 2026-06-12 casing bug) -- the names below
    // match the live CXXHeaderDump member spellings exactly (note LEG_R is upper-case).
    const int32_t tryGrabOff   = ResolveOff(cls, L"tryGrab",       kTryGrabFallback);
    const int32_t grabOff      = ResolveOff(cls, L"grab",          kGrabFallback);
    const int32_t killedOff    = ResolveOff(cls, L"killed",        kKilledFallback);
    const int32_t playerDmgOff = ResolveOff(cls, L"playerDamaged", kPlayerDmgFallback);
    const int32_t harmlessOff  = ResolveOff(cls, L"harmless",      kHarmlessFallback);
    const int32_t targetOff    = ResolveOff(cls, L"Target",        kTargetFallback);
    const int32_t legLOff      = ResolveOff(cls, L"leg_L",         kLegLFallback);
    const int32_t armLOff      = ResolveOff(cls, L"arm_L",         kArmLFallback);
    const int32_t legROff      = ResolveOff(cls, L"LEG_R",         kLegRFallback);
    const int32_t armROff      = ResolveOff(cls, L"arm_R",         kArmRFallback);

    // releasePlayer() -- the grab-cancel verb (RE: entry ubergraph @13289). Tolerated
    // null here (the host neutralization can fall back to destroying the BP wisp), but
    // logged so a rename surfaces.
    void* releaseFn = R::FindFunction(cls, L"releasePlayer");
    if (!releaseFn) UE_LOGW("wisp: releasePlayer UFunction not found -- grab-cancel verb unavailable");

    g_wispCls      = cls;
    g_tryGrabOff   = tryGrabOff;
    g_grabOff      = grabOff;
    g_killedOff    = killedOff;
    g_playerDmgOff = playerDmgOff;
    g_harmlessOff  = harmlessOff;
    g_targetOff    = targetOff;
    g_legLOff      = legLOff;
    g_armLOff      = armLOff;
    g_legROff      = legROff;
    g_armROff      = armROff;
    g_releaseFn    = releaseFn;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("wisp: resolved killerwisp_C (grab@0x%X killed@0x%X playerDamaged@0x%X Target@0x%X "
            "releasePlayer=%p)", static_cast<unsigned>(grabOff), static_cast<unsigned>(killedOff),
            static_cast<unsigned>(playerDmgOff), static_cast<unsigned>(targetOff), releaseFn);
    return true;
}
}  // namespace

bool IsKillerWisp(void* obj) {
    if (!obj) return false;
    if (!g_resolved.load(std::memory_order_acquire)) {
        // Self-resolve from the CANDIDATE's own class -- O(1), NO GUObjectArray walk (the
        // EnsurePlainWispResolved shape). CRITICAL fix (perf audit 2026-07-03 night):
        // every production caller (host attack Tick, client tear mirror, grab hold) gates
        // on IsKillerWisp BEFORE any member accessor that runs EnsureResolved, so without
        // this the wrapper only ever resolved when the dev probe's unguarded ReadState
        // ran -- the whole killerwisp lane was probe-armed-only
        // ([[lesson-gated-probe-verify-the-gate]]). While unresolved this pays one
        // NameEquals per candidate; sessions that never spawn a killerwisp keep paying
        // that (trivial: FName compare per tracked NPC per tick), everyone else latches
        // on the first real killerwisp. Exact-name only: a hypothetical subclass resolves
        // via EnsureResolved instead (killerwisp_C has no subclasses in VOTV).
        void* candCls = R::ClassOf(obj);
        if (!candCls || !R::NameEquals(R::NameOf(candCls), P::name::NpcClass_KillerWisp))
            return false;
        if (!ResolveFromClass(candCls)) return false;
    }
    if (!g_wispCls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_wispCls };
    return R::IsDescendantOfAny(cls, bases, 1);  // class-or-subclass (bounded super walk)
}

bool ReadState(void* wisp, State& out) {
    if (!wisp || !EnsureResolved() || !IsKillerWisp(wisp)) return false;
    out.tryGrab       = ReadAt<bool>(wisp, g_tryGrabOff);
    out.grab          = ReadAt<bool>(wisp, g_grabOff);
    out.killed        = ReadAt<bool>(wisp, g_killedOff);
    out.playerDamaged = ReadAt<bool>(wisp, g_playerDmgOff);
    out.harmless      = ReadAt<bool>(wisp, g_harmlessOff);
    out.target        = ReadAt<void*>(wisp, g_targetOff);
    return true;
}

bool InGrabRange(void* wisp, void* target) {
    if (!wisp || !target) return false;
    // The BP grab-arm overlap radius (killerwisp.json @5702: SphereOverlapActors radius 550).
    constexpr float kGrabRadius   = 550.0f;
    constexpr float kGrabRadiusSq = kGrabRadius * kGrabRadius;
    const ue_wrap::FVector w = ue_wrap::engine::GetActorLocation(wisp);
    const ue_wrap::FVector t = ue_wrap::engine::GetActorLocation(target);
    const float dx = w.X - t.X, dy = w.Y - t.Y, dz = w.Z - t.Z;
    return (dx * dx + dy * dy + dz * dz) <= kGrabRadiusSq;
}

float DistanceTo(void* wisp, void* target) {
    if (!wisp || !target) return 3.4e38f;
    const ue_wrap::FVector w = ue_wrap::engine::GetActorLocation(wisp);
    const ue_wrap::FVector t = ue_wrap::engine::GetActorLocation(target);
    const float dx = w.X - t.X, dy = w.Y - t.Y, dz = w.Z - t.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool WriteTarget(void* wisp, void* pawnOrNull) {
    if (!wisp || !EnsureResolved() || !IsKillerWisp(wisp)) return false;
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(wisp) + g_targetOff) = pawnOrNull;
    return true;
}

bool CanReach(void* wisp, void* target) {
    if (!wisp || !target) return false;
    // canReach parity: the native gate traces lib_obj.obj_statDyn = {WorldStatic,
    // WorldDynamic} -- exactly trace::LineBlockedStatDyn's set (the primitive was
    // extracted from here 2026-07-04 when nameplate occlusion became the second
    // consumer). Unresolvable (-1) -> blocked, the native default.
    const ue_wrap::FVector start = ue_wrap::engine::GetActorLocation(wisp);
    const ue_wrap::FVector end   = ue_wrap::engine::GetActorLocation(target);
    return ue_wrap::trace::LineBlockedStatDyn(wisp, start, end) == 0;
}

bool GrabSocketWorldLocation(void* wisp, ue_wrap::FVector& out) {
    void* mesh = BodyMesh(wisp);
    if (!mesh) return false;
    return ue_wrap::engine::GetBoneWorldLocationByName(mesh, L"playerGrab", out);
}

// ---- v2 victim-side grab choreography (the native Capture player template) ------------

namespace {
// mainPlayer-side grab-state members (the Capture template targets). Resolved once from
// the live local player's own class chain (FindBoolProperty climbs SuperStruct, so the
// APawn / SpringArm bitfields resolve through mainPlayer_C). Tri-state latch like the
// plain-wisp block: 0 unresolved / 1 ok / -1 permanently dead (member name drift).
std::atomic<int> g_grabStateResolve{0};
int32_t g_heldByte = -1;        uint8_t g_heldMask = 0;         // AmainPlayer_C::held
int32_t g_ctrlYawByte = -1;     uint8_t g_ctrlYawMask = 0;      // APawn::bUseControllerRotationYaw
int32_t g_lagOff = -1;                                          // AmainPlayer_C::lag (spring arm)
int32_t g_pawnCtrlByte = -1;    uint8_t g_pawnCtrlMask = 0;     // lag's bUsePawnControlRotation
void*   g_setMoveModeFn = nullptr;                              // UCharacterMovementComponent::SetMovementMode

constexpr uint8_t kMOVE_None    = 0;
constexpr uint8_t kMOVE_Walking = 1;

bool EnsureGrabStateResolved(void* localPlayer) {
    const int st = g_grabStateResolve.load(std::memory_order_acquire);
    if (st == 1) return true;
    if (st == -1) return false;
    void* cls = R::ClassOf(localPlayer);
    if (!cls) return false;
    // TRANSIENT misses (a component instance not yet live) leave the state at 0 so a
    // later grab retries; only a NAME miss on a live chain latches -1 (name drift --
    // retrying cannot heal it).
    void* cmc = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(localPlayer) + P::off::ACharacter_CharacterMovement);
    if (!cmc || !R::IsLive(cmc)) return false;
    const int32_t lagOff = R::FindPropertyOffset(cls, L"lag");
    void* lagComp = (lagOff >= 0)
        ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(localPlayer) + lagOff)
        : nullptr;
    if (lagOff >= 0 && (!lagComp || !R::IsLive(lagComp))) return false;
    int32_t heldByte = -1, ctrlYawByte = -1, pawnCtrlByte = -1;
    uint8_t heldMask = 0, ctrlYawMask = 0, pawnCtrlMask = 0;
    void* setMoveMode = R::FindFunction(R::ClassOf(cmc), L"SetMovementMode");
    if (lagOff < 0 ||
        !R::FindBoolProperty(cls, L"held", heldByte, heldMask) ||
        !R::FindBoolProperty(cls, L"bUseControllerRotationYaw", ctrlYawByte, ctrlYawMask) ||
        !R::FindBoolProperty(R::ClassOf(lagComp), L"bUsePawnControlRotation",
                             pawnCtrlByte, pawnCtrlMask) ||
        !setMoveMode) {
        g_grabStateResolve.store(-1, std::memory_order_release);
        UE_LOGW("wisp: grab-choreography members unresolved (held/ctrlYaw/lag/pawnCtrlRot/"
                "SetMovementMode) -- victim grab degrades to the flat v1 death");
        return false;
    }
    g_heldByte = heldByte;       g_heldMask = heldMask;
    g_ctrlYawByte = ctrlYawByte; g_ctrlYawMask = ctrlYawMask;
    g_lagOff = lagOff;
    g_pawnCtrlByte = pawnCtrlByte; g_pawnCtrlMask = pawnCtrlMask;
    g_setMoveModeFn = setMoveMode;
    g_grabStateResolve.store(1, std::memory_order_release);
    UE_LOGI("wisp: resolved grab-choreography members (held@0x%X lag@0x%X)",
            static_cast<unsigned>(heldByte), static_cast<unsigned>(lagOff));
    return true;
}

void WriteBit(void* obj, int32_t byteOff, uint8_t mask, bool value) {
    uint8_t* b = reinterpret_cast<uint8_t*>(obj) + byteOff;
    if (value) *b |= mask; else *b &= static_cast<uint8_t>(~mask);
}

bool CallSetMovementMode(void* localPlayer, uint8_t mode) {
    void* cmc = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(localPlayer) + P::off::ACharacter_CharacterMovement);
    if (!cmc || !R::IsLive(cmc) || !g_setMoveModeFn) return false;
    ParamFrame f(g_setMoveModeFn);
    if (!f.valid()) return false;
    f.Set<uint8_t>(L"NewMovementMode", mode);
    f.Set<uint8_t>(L"NewCustomMode", uint8_t{0});
    return ue_wrap::Call(cmc, f);
}
}  // namespace

bool ApplyGrabToLocalPlayer(void* wispActor, void* localPlayer) {
    if (!wispActor || !localPlayer || !R::IsLive(wispActor) || !R::IsLive(localPlayer)) return false;
    void* wispMesh = BodyMesh(wispActor);
    if (!wispMesh) { UE_LOGW("wisp: ApplyGrab -- no live wisp body mesh"); return false; }
    if (!EnsureGrabStateResolved(localPlayer)) return false;
    // The Capture template, in the bytecode's own order.
    if (!CallSetMovementMode(localPlayer, kMOVE_None))
        UE_LOGW("wisp: ApplyGrab -- SetMovementMode(None) failed (continuing)");
    if (!ue_wrap::engine::AttachActorToComponentSocket(localPlayer, wispMesh, L"playerGrab")) {
        // Attach is the load-bearing step: without it there is no ride -- restore movement.
        CallSetMovementMode(localPlayer, kMOVE_Walking);
        UE_LOGW("wisp: ApplyGrab -- socket attach FAILED, movement restored (flat death remains)");
        return false;
    }
    void* playerMesh = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(localPlayer) + P::off::ACharacter_Mesh);
    if (playerMesh && R::IsLive(playerMesh))
        ue_wrap::engine::SetSceneComponentVisibility(playerMesh, true, false);
    WriteBit(localPlayer, g_heldByte, g_heldMask, true);
    WriteBit(localPlayer, g_ctrlYawByte, g_ctrlYawMask, false);
    void* lagComp = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(localPlayer) + g_lagOff);
    if (lagComp && R::IsLive(lagComp))
        WriteBit(lagComp, g_pawnCtrlByte, g_pawnCtrlMask, false);
    UE_LOGI("wisp: ApplyGrab -- local player attached to 'playerGrab' (MOVE_None, held=1, "
            "camera decoupled) -- the native Capture template");
    return true;
}

bool ReleaseGrabOnLocalPlayer(void* localPlayer) {
    if (!localPlayer || !R::IsLive(localPlayer)) return false;
    if (g_grabStateResolve.load(std::memory_order_acquire) != 1) return false;  // never grabbed
    ue_wrap::engine::DetachActorFromParent(localPlayer);
    CallSetMovementMode(localPlayer, kMOVE_Walking);
    WriteBit(localPlayer, g_heldByte, g_heldMask, false);
    WriteBit(localPlayer, g_ctrlYawByte, g_ctrlYawMask, true);
    void* lagComp = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(localPlayer) + g_lagOff);
    if (lagComp && R::IsLive(lagComp))
        WriteBit(lagComp, g_pawnCtrlByte, g_pawnCtrlMask, true);
    UE_LOGI("wisp: ReleaseGrab -- local player detached, movement + camera flags restored");
    return true;
}

void* ReadLimbComponent(void* wisp, Limb limb) {
    if (!wisp || !EnsureResolved() || !IsKillerWisp(wisp)) return nullptr;
    int32_t off = -1;
    switch (limb) {
        case Limb::ArmL: off = g_armLOff; break;
        case Limb::LegR: off = g_legROff; break;
        case Limb::LegL: off = g_legLOff; break;
        case Limb::ArmR: off = g_armROff; break;
    }
    return (off >= 0) ? ReadAt<void*>(wisp, off) : nullptr;
}

bool CallReleasePlayer(void* wisp) {
    if (!wisp || !EnsureResolved() || !IsKillerWisp(wisp)) return false;
    if (!g_releaseFn) {
        UE_LOGW("wisp: CallReleasePlayer -- releasePlayer UFunction unresolved");
        return false;
    }
    ue_wrap::ParamFrame f(g_releaseFn);  // zero-arg verb
    if (!f.valid()) {
        UE_LOGE("wisp: CallReleasePlayer -- ParamFrame(releasePlayer) invalid");
        return false;
    }
    return ue_wrap::Call(wisp, f);
}

// ---- Inc1b: tear-mirror substrate -----------------------------------------------------

namespace {
// The killerWispAnim1 montage asset (the `fatality` section lives in it). Resolved once via
// FindObject by leaf name; cached. UAnimMontage objects are cooked assets, present once the
// wisp class loads. null until resolvable.
void* g_fatalityMontage = nullptr;
void* g_montagePlayFn   = nullptr;  // UAnimInstance::Montage_Play
void* g_montageJumpFn   = nullptr;  // UAnimInstance::Montage_JumpToSection

void* BodyAnimInstance(void* wisp) {
    void* mesh = BodyMesh(wisp);
    if (!mesh) return nullptr;
    void* anim = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(mesh) + P::off::USkeletalMesh_AnimScriptInstance);
    return (anim && R::IsLive(anim)) ? anim : nullptr;
}
}  // namespace

void* BodyMesh(void* wisp) {
    if (!wisp || !EnsureResolved() || !IsKillerWisp(wisp)) return nullptr;
    void* mesh = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(wisp) + P::off::ACharacter_Mesh);
    return (mesh && R::IsLive(mesh)) ? mesh : nullptr;
}

bool ForceMeshTick(void* wisp) {
    void* mesh = BodyMesh(wisp);
    if (!mesh) return false;
    return ue_wrap::engine::SetAnimTickAlways(mesh);
}

// ---- plain wisp_C landing drive (2026-07-03 wisp mirror lane) --------------------------

namespace {
// Resolved once from the live wisp_C class (loads with the map -- trigger_wispSwarm_2 imports
// it). Separate from the killerwisp statics above: DIFFERENT class, different members.
// Tri-state: 0 unresolved (retry), 1 resolved, -1 PERMANENTLY dead (class resolved but a member
// didn't -- name drift on a future game build). The caller retries PER FRAME per unlanded wisp
// mirror, so a member miss must latch dead rather than re-run the FindClass full walk each frame.
std::atomic<int> g_plainState{0};
void*   g_plainWispCls  = nullptr;  // wisp_C UClass (exact -- colored wisp_o/b/g are SIBLINGS)
int32_t g_landedByte    = -1;       // `landed` FBoolProperty byte offset
uint8_t g_landedMask    = 0;        //          ... and bit mask
void*   g_dirFn         = nullptr;  // dir(bool Condition) -- Play/Reverse the fade timeline

// `candidateCls` = the CALLER'S actor's class (ClassOf -- O(1)); resolution never walks
// GUObjectArray (perf audit W2: the old FindClass here was 2 full walks on the first landing
// frame). The candidate is name-verified before being trusted as wisp_C.
bool EnsurePlainWispResolved(void* candidateCls) {
    const int st = g_plainState.load(std::memory_order_acquire);
    if (st == 1) return true;
    if (st == -1) return false;  // permanently dead this process (logged once below)
    if (!candidateCls || !R::NameEquals(R::NameOf(candidateCls), P::name::NpcClass_Wisp))
        return false;  // not a wisp_C caller -- no resolution material (state stays 0)
    int32_t landedByte = -1; uint8_t landedMask = 0;
    void* dirFn = nullptr;
    if (!R::FindBoolProperty(candidateCls, L"landed", landedByte, landedMask) ||
        !(dirFn = R::FindFunction(candidateCls, L"dir"))) {
        // The class exists but a member doesn't: name drift. Retrying cannot heal it at
        // runtime -- latch DEAD so the per-frame caller stops paying for the attempt.
        g_plainState.store(-1, std::memory_order_release);
        UE_LOGW("wisp: wisp_C resolved but landed/dir member missing (name drift?) -- landing "
                "drive PERMANENTLY unavailable this process (mirrors stay faded-out)");
        return false;
    }
    g_plainWispCls = candidateCls;
    g_landedByte   = landedByte;
    g_landedMask   = landedMask;
    g_dirFn        = dirFn;
    g_plainState.store(1, std::memory_order_release);
    UE_LOGI("wisp: resolved wisp_C landing drive (landed@0x%X mask=%02X dir=%p)",
            static_cast<unsigned>(landedByte), landedMask, dirFn);
    return true;
}
}  // namespace

bool DriveWispLanding(void* wispActor) {
    if (!wispActor || !R::IsLive(wispActor)) return false;
    void* cls = R::ClassOf(wispActor);
    if (!cls || !EnsurePlainWispResolved(cls)) return false;
    // Exact-class gate (safety net; the caller arms per-mirror by class already). The colored
    // wisp_o/b/g siblings are never mirrored -- and are DIFFERENT classes, not subclasses.
    if (cls != g_plainWispCls) return false;
    // Replay the native landing edge exactly as the BP's own tick would (ubergraph [96]-[98]):
    // landed := true (a plain BP bool the BP writes inline via EX_LetBool -- no setter exists,
    // so the masked byte write IS the game's own mechanism), then dir(true) -> fade-in timeline
    // forward + PointLight ramp, dispatched through the normal UFunction path.
    uint8_t* b = reinterpret_cast<uint8_t*>(wispActor) + g_landedByte;
    *b |= g_landedMask;
    ue_wrap::ParamFrame f(g_dirFn);
    if (!f.valid()) return false;
    f.Set<uint8_t>(L"Condition", 1);
    return ue_wrap::Call(wispActor, f);
}

bool PlayFatalityMontage(void* wisp) {
    void* anim = BodyAnimInstance(wisp);
    if (!anim) { UE_LOGW("wisp: PlayFatalityMontage -- no live body AnimInstance"); return false; }
    // Resolve the montage asset + the two AnimInstance UFunctions once.
    if (!g_fatalityMontage)
        g_fatalityMontage = R::FindObject(L"killerWispAnim1_Skeleton_Montage", L"AnimMontage");
    if (!g_fatalityMontage)
        g_fatalityMontage = R::FindObject(L"killerWispAnim1_Skeleton_Montage", nullptr);  // class-agnostic retry
    if (!g_montagePlayFn || !g_montageJumpFn) {
        if (void* aiCls = R::FindClass(L"AnimInstance")) {
            if (!g_montagePlayFn) g_montagePlayFn = R::FindFunction(aiCls, L"Montage_Play");
            if (!g_montageJumpFn) g_montageJumpFn = R::FindFunction(aiCls, L"Montage_JumpToSection");
        }
    }
    if (!g_fatalityMontage || !g_montagePlayFn) {
        UE_LOGW("wisp: PlayFatalityMontage -- montage asset=%p or Montage_Play=%p unresolved "
                "(tear degrades to force-tick + gibs)", g_fatalityMontage, g_montagePlayFn);
        return false;
    }
    // Montage_Play(MontageToPlay, InPlayRate=1.0, ...) -- the rest defaults (zero-init).
    {
        ue_wrap::ParamFrame f(g_montagePlayFn);
        if (!f.valid()) return false;
        f.Set<void*>(L"MontageToPlay", g_fatalityMontage);
        f.Set<float>(L"InPlayRate", 1.0f);
        if (!ue_wrap::Call(anim, f)) { UE_LOGW("wisp: Montage_Play call failed"); return false; }
    }
    // Montage_JumpToSection('fatality', MontageToJumpTo). Skip-tolerant: if the jump fn is
    // missing the montage still plays from the start (which begins with grab/d1.. anyway).
    if (g_montageJumpFn) {
        R::FName section = ue_wrap::fname_utils::StringToFName(L"fatality");
        ue_wrap::ParamFrame f(g_montageJumpFn);
        if (f.valid()) {
            f.SetRaw(L"SectionName", &section, sizeof(section));
            f.Set<void*>(L"MontageToJumpTo", g_fatalityMontage);
            ue_wrap::Call(anim, f);
        }
    }
    return true;
}

}  // namespace ue_wrap::wisp
