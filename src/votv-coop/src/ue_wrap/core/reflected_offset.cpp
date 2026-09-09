// ue_wrap/core/reflected_offset.cpp -- reflection-resolved BP property offsets.
//
// See ue_wrap/core/reflected_offset.h for the interface, for what each field is, and for why
// no number lives on this side.

#include "ue_wrap/core/reflected_offset.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <atomic>
#include <mutex>
#include <set>
#include <string>

namespace ue_wrap::reflected_offset {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

// Internal: resolve, and log once.
//   Returns the FProperty.Offset_Internal of `fieldName` on the UClass named `className`, or
//   -1 if either lookup fails. Logs once per (class, field) pair whatever the outcome: a
//   one-time resolved line on success, a one-time warning on failure. The logged-pair sets
//   are process-static, so they cover every accessor call.
int32_t Resolve(const wchar_t* className, const wchar_t* fieldName) {
    void* cls = R::FindClass(className);
    int32_t off = -1;
    if (cls) {
        off = R::FindPropertyOffset(cls, fieldName);
    }
    // Success and failure are memoised in SEPARATE sets, so a "class not loaded yet" warning that
    // lands first cannot suppress the success line that follows it. Sharing one set left the log
    // showing the warning for good, including after the BP class had loaded and the offset had
    // resolved.
    static std::mutex sLogMtx;
    static std::set<std::wstring> sLoggedSuccess;
    static std::set<std::wstring> sLoggedFail;
    std::wstring key = std::wstring(className) + L"::" + fieldName;
    bool firstSuccess = false, firstFail = false;
    {
        std::lock_guard<std::mutex> lk(sLogMtx);
        if (off >= 0) firstSuccess = sLoggedSuccess.insert(key).second;
        else          firstFail    = sLoggedFail.insert(key).second;
    }
    if (firstSuccess) {
        UE_LOGI("reflected_offset: %ls -> 0x%X (resolved once via FindPropertyOffset)",
                key.c_str(), off);
    } else if (firstFail) {
        if (!cls) {
            UE_LOGW("reflected_offset: %ls UNRESOLVED -- class '%ls' not loaded yet (will retry on next call)",
                    key.c_str(), className);
        } else {
            UE_LOGE("reflected_offset: %ls UNRESOLVED -- class loaded but FIELD MISSING (VOTV likely renamed it; sdk_profile.h needs update)",
                    key.c_str());
        }
    }
    return off;
}

// Accessor body: a static cache that memoises only on success. A first call before the BP
// class loads returns -1; the next call retries. Once resolved, every later call is a single
// atomic load.
//
// Thread-safety: the race on the atomic is benign -- two threads resolving the same pair both
// call Resolve() and both store the same value. Resolve's own mutex prevents double logging.
struct OffsetCache {
    std::atomic<int32_t> value{-1};
};

int32_t GetOrResolve(OffsetCache& cache, const wchar_t* className, const wchar_t* fieldName) {
    int32_t cached = cache.value.load(std::memory_order_acquire);
    if (cached >= 0) return cached;
    const int32_t resolved = Resolve(className, fieldName);
    if (resolved >= 0) cache.value.store(resolved, std::memory_order_release);
    return resolved;
}

#define VC_DEFINE_OFFSET(fn, klass, field) \
    int32_t fn() { static OffsetCache cache; return GetOrResolve(cache, klass, field); }

}  // namespace

VC_DEFINE_OFFSET(MainPlayer_heavyGrab,            L"mainPlayer_C", L"heavyGrab")
VC_DEFINE_OFFSET(MainPlayer_grabHandle,           L"mainPlayer_C", L"grabHandle")
VC_DEFINE_OFFSET(MainPlayer_grabTimeline,         L"mainPlayer_C", L"grab")        // the UTimelineComponent member is named `grab`
VC_DEFINE_OFFSET(MainPlayer_grabbing_actor,       L"mainPlayer_C", L"grabbing_actor")
VC_DEFINE_OFFSET(MainPlayer_grabbing_component,   L"mainPlayer_C", L"grabbing_component")
VC_DEFINE_OFFSET(MainPlayer_grabsHeavy,           L"mainPlayer_C", L"grabsHeavy")
VC_DEFINE_OFFSET(MainPlayer_grabLen,              L"mainPlayer_C", L"grabLen")
VC_DEFINE_OFFSET(MainPlayer_Heavy,                L"mainPlayer_C", L"Heavy")
VC_DEFINE_OFFSET(MainPlayer_holding_actor,        L"mainPlayer_C", L"holding_actor")
VC_DEFINE_OFFSET(MainPlayer_lookAtActor,          L"mainPlayer_C", L"lookAtActor")
VC_DEFINE_OFFSET(MainPlayer_isRagdoll,            L"mainPlayer_C", L"isRagdoll")
VC_DEFINE_OFFSET(MainPlayer_dead,                 L"mainPlayer_C", L"dead")
VC_DEFINE_OFFSET(MainPlayer_activeInterface,      L"mainPlayer_C", L"activeInterface")
VC_DEFINE_OFFSET(MainPlayer_HitResult,            L"mainPlayer_C", L"HitResult")
VC_DEFINE_OFFSET(MainPlayer_releaseEToUse,        L"mainPlayer_C", L"releaseEToUse")
VC_DEFINE_OFFSET(MainPlayer_actionIndex,          L"mainPlayer_C", L"actionIndex")

VC_DEFINE_OFFSET(AnimBP_kerfur_walkSpeed,           P::name::AnimBPKerfurRegularClass, L"walkSpeed")
VC_DEFINE_OFFSET(AnimBP_kerfur_Pawn,                P::name::AnimBPKerfurRegularClass, L"Pawn")
VC_DEFINE_OFFSET(AnimBP_kerfur_Controller,          P::name::AnimBPKerfurRegularClass, L"Controller")
VC_DEFINE_OFFSET(AnimBP_kerfur_Movement,            P::name::AnimBPKerfurRegularClass, L"Movement")
VC_DEFINE_OFFSET(AnimBP_kerfur_animWalkAlpha,       P::name::AnimBPKerfurRegularClass, L"animWalkAlpha")
VC_DEFINE_OFFSET(AnimBP_kerfur_animWalkRate,        P::name::AnimBPKerfurRegularClass, L"animWalkRate")
VC_DEFINE_OFFSET(AnimBP_kerfur_lookingAtPlayer,     P::name::AnimBPKerfurRegularClass, L"lookingAtPlayer")
VC_DEFINE_OFFSET(AnimBP_kerfur_kerfur,              P::name::AnimBPKerfurRegularClass, L"kerfur")
VC_DEFINE_OFFSET(AnimBP_kerfur_walkSpeedMultiplier, P::name::AnimBPKerfurRegularClass, L"walkSpeedMultiplier")
VC_DEFINE_OFFSET(AnimBP_kerfur_spd,                 P::name::AnimBPKerfurRegularClass, L"spd")
VC_DEFINE_OFFSET(AnimBP_kerfur_useLegIK,            P::name::AnimBPKerfurRegularClass, L"useLegIK")
VC_DEFINE_OFFSET(AnimBP_kerfur_removeArms,          P::name::AnimBPKerfurRegularClass, L"removeArms")
VC_DEFINE_OFFSET(AnimBP_kerfur_isFace,              P::name::AnimBPKerfurRegularClass, L"isFace")
VC_DEFINE_OFFSET(AnimBP_kerfur_lookAt,              P::name::AnimBPKerfurRegularClass, L"lookAt")
VC_DEFINE_OFFSET(AnimBP_kerfur_customLookAt,        P::name::AnimBPKerfurRegularClass, L"customLookAt")

#undef VC_DEFINE_OFFSET

}  // namespace ue_wrap::reflected_offset
