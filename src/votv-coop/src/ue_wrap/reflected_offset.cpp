// ue_wrap/reflected_offset.cpp -- reflection-resolved BP property offsets.
//
// See ue_wrap/reflected_offset.h for the public interface + rationale.

#include "ue_wrap/reflected_offset.h"

#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <atomic>
#include <mutex>
#include <set>
#include <string>

namespace ue_wrap::reflected_offset {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

// Internal: do the resolve + log-once.
//   Returns the FProperty.Offset_Internal of `fieldName` on the UClass
//   named `className`, or -1 if either lookup fails. Logs once per
//   (class, field) pair regardless of outcome -- on success we get a
//   one-time "resolved to 0x%X" line; on failure a one-time WARN. The
//   logged-pairs set is process-static (covers every accessor call).
int32_t Resolve(const wchar_t* className, const wchar_t* fieldName) {
    void* cls = R::FindClass(className);
    int32_t off = -1;
    if (cls) {
        off = R::FindPropertyOffset(cls, fieldName);
    }
    static std::mutex sLogMtx;
    static std::set<std::wstring> sLogged;
    std::wstring key = std::wstring(className) + L"::" + fieldName;
    bool firstTime = false;
    {
        std::lock_guard<std::mutex> lk(sLogMtx);
        firstTime = sLogged.insert(key).second;
    }
    if (firstTime) {
        if (off >= 0) {
            UE_LOGI("reflected_offset: %ls -> 0x%X (resolved once via FindPropertyOffset)",
                    key.c_str(), off);
        } else if (!cls) {
            UE_LOGW("reflected_offset: %ls UNRESOLVED -- class '%ls' not loaded yet (will retry on next call)",
                    key.c_str(), className);
        } else {
            UE_LOGE("reflected_offset: %ls UNRESOLVED -- class loaded but FIELD MISSING (VOTV likely renamed it; sdk_profile.h needs update)",
                    key.c_str());
        }
    }
    return off;
}

// Accessor body: static cache that only memorizes on success.
// First call before BP class loads -> -1; next call retries.
// Once resolved, every subsequent call is a single atomic load.
//
// Thread-safety: the std::atomic<int32_t> race is benign -- two threads
// racing to resolve the same pair will both call Resolve(); both write
// the same value to the atomic. Resolve's own logging mutex prevents
// double-logging.
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

VC_DEFINE_OFFSET(AnimBP_kerfur_walkSpeed,           P::name::AnimBPKerfurRegularClass, L"walkSpeed")
VC_DEFINE_OFFSET(AnimBP_kerfur_Pawn,                P::name::AnimBPKerfurRegularClass, L"Pawn")
VC_DEFINE_OFFSET(AnimBP_kerfur_Controller,          P::name::AnimBPKerfurRegularClass, L"Controller")
VC_DEFINE_OFFSET(AnimBP_kerfur_Movement,            P::name::AnimBPKerfurRegularClass, L"Movement")
VC_DEFINE_OFFSET(AnimBP_kerfur_Character,           P::name::AnimBPKerfurRegularClass, L"Character")
VC_DEFINE_OFFSET(AnimBP_kerfur_animWalkAlpha,       P::name::AnimBPKerfurRegularClass, L"animWalkAlpha")
VC_DEFINE_OFFSET(AnimBP_kerfur_animWalkRate,        P::name::AnimBPKerfurRegularClass, L"animWalkRate")
VC_DEFINE_OFFSET(AnimBP_kerfur_lookingAtPlayer,     P::name::AnimBPKerfurRegularClass, L"lookingAtPlayer")
VC_DEFINE_OFFSET(AnimBP_kerfur_kerfur,              P::name::AnimBPKerfurRegularClass, L"kerfur")
VC_DEFINE_OFFSET(AnimBP_kerfur_walkSpeedMultiplier, P::name::AnimBPKerfurRegularClass, L"walkSpeedMultiplier")
VC_DEFINE_OFFSET(AnimBP_kerfur_spd,                 P::name::AnimBPKerfurRegularClass, L"spd")
VC_DEFINE_OFFSET(AnimBP_kerfur_useLegIK,            P::name::AnimBPKerfurRegularClass, L"useLegIK")
VC_DEFINE_OFFSET(AnimBP_kerfur_removeArms,          P::name::AnimBPKerfurRegularClass, L"removeArms")
VC_DEFINE_OFFSET(AnimBP_kerfur_headLookAt,          P::name::AnimBPKerfurRegularClass, L"headLookAt")
VC_DEFINE_OFFSET(AnimBP_kerfur_isFace,              P::name::AnimBPKerfurRegularClass, L"isFace")

#undef VC_DEFINE_OFFSET

}  // namespace ue_wrap::reflected_offset
