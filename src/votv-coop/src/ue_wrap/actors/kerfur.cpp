// ue_wrap/actors/kerfur.cpp -- see ue_wrap/actors/kerfur.h for the contract.

#include "ue_wrap/actors/kerfur.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstring>
#include <cwchar>

namespace ue_wrap::kerfur {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

// kerfurOmega_C resolves once (loads with the level; re-resolve while still null).
void* ResolveKerfurClass() {
    static void* sCls = nullptr;
    if (!sCls) sCls = R::FindClass(P::name::NpcClass_KerfurOmega);
    return sCls;
}

// Cached field offsets (resolved ONCE vs kerfurOmega_C; the offsets are inherited by every
// skin subclass, so resolving on the base is correct + stable). Sentinel -2 = "not attempted";
// FindPropertyOffset returns -1 when absent (>= 0 is a valid offset). The hot reads
// (ReadKerfurState per tick) never re-walk once these settle.
int32_t g_offState     = -2;  // TEnumAsByte<enum_kerfurCommand> "State"
int32_t g_offSpooky    = -2;  // bool "isSpooky"
int32_t g_offFace      = -2;  // int32 "faceMaterialIndex"
int32_t g_offKill      = -2;  // bool "kill" (murderfur-mode guard)

void ResolveKerfurOffsets() {
    void* kc = ResolveKerfurClass();
    if (!kc) return;
    if (g_offState  == -2) g_offState  = R::FindPropertyOffset(kc, L"State");
    if (g_offSpooky == -2) g_offSpooky = R::FindPropertyOffset(kc, L"isSpooky");
    if (g_offFace   == -2) g_offFace   = R::FindPropertyOffset(kc, L"faceMaterialIndex");
    if (g_offKill   == -2) g_offKill   = R::FindPropertyOffset(kc, L"kill");
}

}  // namespace

bool IsKerfurActor(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    void* kc = ResolveKerfurClass();
    if (!kc) return false;
    void* cls = R::ClassOf(actor);
    if (!cls) return false;
    if (cls == kc) return true;
    void* bases[1] = {kc};
    return R::IsDescendantOfAny(cls, bases, 1);
}

bool HasSaveKey(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    void* cls = R::ClassOf(actor);
    if (!cls) return false;
    // 1-entry memo: the host sender iterates same-class NPCs (kerfurs) at the connect edge;
    // avoid re-walking the FProperty chain for the repeated class. Cold path (per-NPC at
    // connect), so a single-slot memo is plenty.
    static void*   sMemoCls = nullptr;
    static int32_t sMemoOff = -2;
    int32_t off;
    if (cls == sMemoCls) {
        off = sMemoOff;
    } else {
        off = R::FindPropertyOffset(cls, L"Key");
        sMemoCls = cls;
        sMemoOff = off;
    }
    if (off < 0) return false;  // class has no int_save "Key" (host-spawned transient enemy)
    const R::FName key = *reinterpret_cast<const R::FName*>(
        reinterpret_cast<const uint8_t*>(actor) + off);
    const std::wstring w = R::ToString(key);
    return !(w.empty() || w == L"None");  // a non-None key -> this NPC is a save object
}

bool ReadKerfurState(void* actor, uint8_t& state, bool& spooky, uint8_t& face) {
    if (!IsKerfurActor(actor)) return false;
    ResolveKerfurOffsets();
    if (g_offState < 0) return false;
    state  = *reinterpret_cast<const uint8_t*>(reinterpret_cast<const uint8_t*>(actor) + g_offState);
    spooky = (g_offSpooky >= 0) &&
             *reinterpret_cast<const bool*>(reinterpret_cast<const uint8_t*>(actor) + g_offSpooky);
    face   = (g_offFace >= 0)
                 ? static_cast<uint8_t>(*reinterpret_cast<const int32_t*>(
                       reinterpret_cast<const uint8_t*>(actor) + g_offFace))
                 : uint8_t{0};
    return true;
}

void DriveKerfurState(void* actor, uint8_t state, bool spooky) {
    if (!IsKerfurActor(actor)) return;
    ResolveKerfurOffsets();
    if (g_offState >= 0)
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(actor) + g_offState) = state;
    if (g_offSpooky >= 0)
        *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(actor) + g_offSpooky) = spooky;
}

void NeutralizeAiTimers(void* actor) {
    // Legit no-op, NOT an error: callers (npc_mirror) deliberately invoke this on ANY parked
    // NPC and expect a quiet skip for non-kerfurs. Kept silent to match the sibling
    // (DriveKerfurState/ReadKill) and to avoid spamming on the expected path.
    if (!IsKerfurActor(actor)) return;
    static void* sKslCdo  = nullptr;
    static void* sClearFn = nullptr;
    if (!sKslCdo) sKslCdo = R::FindClassDefaultObject(L"KismetSystemLibrary");
    if (sKslCdo && !sClearFn) {
        if (void* kc = R::FindClass(L"KismetSystemLibrary"))
            sClearFn = R::FindFunction(kc, L"K2_ClearTimer");
    }
    if (!sKslCdo || !sClearFn) {
        static bool sWarned = false;
        if (!sWarned) { sWarned = true;
            UE_LOGW("kerfur: K2_ClearTimer unresolved -- mirror AI timers NOT neutralized "
                    "(KslCdo=%p, ClearFn=%p); timer_face/timer_kerf/checkDoor may run local AI "
                    "on the parked mirror", sKslCdo, sClearFn); }
        return;
    }
    // K2_ClearTimer(Object, FunctionName) -- the exact inverse of the BP's K2_SetTimerDelegate
    // ({self, name}) arm (kerfur BeginPlay @8184/@8714/@24444). FunctionName is an FString
    // {data, num=len+1, max} (num/max include the null terminator -- the space_renderer pattern).
    //
    // Every failure path below is LOGGED. The original `if (!f.SetRaw(...)) continue;` plus the
    // unchecked Set() meant a signature mismatch (e.g. K2_ClearTimer taking a K2_TimerHandle
    // instead of Object+FunctionName) or a wrong event name produced a SILENT no-op -- the
    // whole reason the kerfur AI-timer leak was untraceable. Now the exact failing step, and
    // which of the three timers cleared, is visible in multivoid.log on a single run.
    static const wchar_t* kTimers[3] = {L"timer_face", L"timer_kerf", L"checkDoor"};
    int cleared = 0;
    for (const wchar_t* fn : kTimers) {
        ue_wrap::ParamFrame f(sClearFn);
        if (!f.valid()) {
            UE_LOGW("kerfur: NeutralizeAiTimers: ParamFrame(%ls) invalid -- skipping", fn);
            continue;
        }
        // Object MUST bind. An unchecked Set() on an absent param would target a null actor and
        // silently no-op. One miss means the SIGNATURE is wrong for all three timers, so break
        // rather than loop over doomed calls.
        if (!f.Set<void*>(L"Object", actor)) {
            UE_LOGW("kerfur: NeutralizeAiTimers: K2_ClearTimer has no 'Object' param "
                    "(signature mismatch) -- cannot clear '%ls'", fn);
            break;
        }
        const int32_t num = static_cast<int32_t>(std::wcslen(fn)) + 1;
        struct { const wchar_t* data; int32_t n; int32_t m; } fs{fn, num, num};
        if (!f.SetRaw(L"FunctionName", &fs, sizeof(fs))) {
            UE_LOGW("kerfur: NeutralizeAiTimers: K2_ClearTimer has no 'FunctionName' param "
                    "(signature mismatch) -- cannot clear '%ls'", fn);
            break;
        }
        if (ue_wrap::Call(sKslCdo, f)) {
            UE_LOGI("kerfur: NeutralizeAiTimers: cleared timer '%ls' on %p", fn, actor);
            ++cleared;
        } else {
            UE_LOGW("kerfur: NeutralizeAiTimers: K2_ClearTimer('%ls') returned false -- "
                    "timer not armed / event name may be wrong", fn);
        }
    }
    UE_LOGI("kerfur: NeutralizeAiTimers: %d/3 timers cleared on %p", cleared, actor);
}

bool ReadKill(void* actor) {
    if (!IsKerfurActor(actor)) return false;
    ResolveKerfurOffsets();
    if (g_offKill < 0) return false;
    return *reinterpret_cast<const bool*>(reinterpret_cast<const uint8_t*>(actor) + g_offKill);
}

void SetCommandState(void* actor, uint8_t state) {
    if (!IsKerfurActor(actor)) return;
    ResolveKerfurOffsets();
    if (g_offState >= 0)
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(actor) + g_offState) = state;
}

bool RunActionName(void* kerfurActor, void* playerActor, const wchar_t* name) {
    if (!IsKerfurActor(kerfurActor) || !name) return false;
    static void* sActionNameFn = nullptr;
    if (!sActionNameFn) {
        if (void* kc = ResolveKerfurClass())
            sActionNameFn = R::FindFunction(kc, L"actionName");  // case-insensitive (engine FName)
    }
    if (!sActionNameFn) {
        static bool sWarned = false;
        if (!sWarned) { sWarned = true;
            UE_LOGW("kerfur: actionName unresolved -- cannot run menu verb on host"); }
        return false;
    }
    ue_wrap::ParamFrame f(sActionNameFn);
    if (!f.valid()) return false;
    f.Set<void*>(L"Player", playerActor);
    const int32_t num = static_cast<int32_t>(std::wcslen(name)) + 1;
    struct { const wchar_t* data; int32_t n; int32_t m; } fs{name, num, num};
    f.SetRaw(L"Name", &fs, sizeof(fs));
    // Hit (FHitResult) is left zeroed -- the State-change verbs do not read it.
    return ue_wrap::Call(kerfurActor, f);
}

void IssueFollowMoveTo(void* kerfurPawn, void* targetActor,
                       float destX, float destY, float destZ, float acceptRadius) {
    if (!kerfurPawn || !R::IsLive(kerfurPawn)) return;
    static void* sAiCdo  = nullptr;
    static void* sMoveFn = nullptr;
    if (!sAiCdo) sAiCdo = R::FindClassDefaultObject(L"AIBlueprintHelperLibrary");
    if (sAiCdo && !sMoveFn) {
        if (void* ac = R::FindClass(L"AIBlueprintHelperLibrary"))
            sMoveFn = R::FindFunction(ac, L"CreateMoveToProxyObject");
    }
    if (!sAiCdo || !sMoveFn) {
        static bool sWarned = false;
        if (!sWarned) { sWarned = true;
            UE_LOGW("kerfur: CreateMoveToProxyObject unresolved -- ownership-follow disabled"); }
        return;
    }
    // UAIBlueprintHelperLibrary::CreateMoveToProxyObject(WorldContextObject, Pawn, Destination,
    // TargetActor, AcceptanceRadius, bStopOnOverlap). Pawn = the kerfur (it has the AIController);
    // TargetActor (the puppet) is best-effort follow, Destination is the always-honored fallback.
    ue_wrap::ParamFrame f(sMoveFn);
    if (!f.valid()) return;
    f.Set<void*>(L"WorldContextObject", kerfurPawn);
    f.Set<void*>(L"Pawn", kerfurPawn);
    const float dest[3] = {destX, destY, destZ};
    f.SetRaw(L"Destination", dest, sizeof(dest));
    f.Set<void*>(L"TargetActor", targetActor);  // may be null -> Destination-only
    f.Set<float>(L"AcceptanceRadius", acceptRadius);
    f.Set<uint8_t>(L"bStopOnOverlap", uint8_t{0});
    ue_wrap::Call(sAiCdo, f);  // returns a proxy we don't keep (fire-and-forget)
}

}  // namespace ue_wrap::kerfur
