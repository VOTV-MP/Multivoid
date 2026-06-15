// ue_wrap/wisp.cpp -- see ue_wrap/wisp.h. Engine access for the VOTV Killer Wisp
// (Akillerwisp_C / FName "killerwisp_C").
//
// All field offsets are resolved from the live class via reflection (FindPropertyOffset)
// rather than hardcoded, so they stay correct across game builds (version-tagging rule).
// The known Alpha 0.9.0-n offsets (CXXHeaderDump/killerwisp.hpp) are kept only as a logged
// fallback if the reflected walk ever fails to find the property.

#include "ue_wrap/wisp.h"

#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"       // SetAnimTickAlways (force-mesh-tick)
#include "ue_wrap/fname_utils.h"  // StringToFName (the 'fatality' montage section)
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <atomic>
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

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* cls = R::FindClass(P::name::NpcClass_KillerWisp);  // L"killerwisp_C"
    if (!cls) return false;  // BP class not loaded yet -- caller retries on a later tick

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

bool IsKillerWisp(void* obj) {
    if (!obj || !g_resolved.load(std::memory_order_acquire) || !g_wispCls) return false;
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
