// ue_wrap/passwordlock.cpp -- see ue_wrap/passwordlock.h. Engine access for VOTV
// password keypads (ApasswordLock_C).
//
// All field offsets are resolved from the live class via reflection
// (FindPropertyOffset) rather than hardcoded, so they stay correct across game
// builds (version-tagging rule). The known Alpha 0.9.0-n offsets are kept only as a
// logged fallback if the reflected walk ever fails to find the property.

#include "ue_wrap/devices/passwordlock.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::passwordlock {
namespace {

namespace R = reflection;

// Resolved once at EnsureResolved, then read-only. Published via the g_resolved
// release-store / acquire-load.
std::atomic<bool> g_resolved{false};

void*   g_lockCls    = nullptr;  // passwordLock_C UClass
int32_t g_keyOff     = -1;       // AtriggerBase_C::Key   (Alpha 0.9.0-n: 0x0260)
int32_t g_inPwOff    = -1;       // ApasswordLock_C::inPassword (0x0380, FString)
int32_t g_doorOff    = -1;       // ApasswordLock_C::door (Adoor_C*) (0x0338) -- the gated door
int32_t g_isResetOff = -1;       // ApasswordLock_C::isReset    (0x0360, bool) -- set-new-code mode
int32_t g_activeOff  = -1;       // ApasswordLock_C::active     (0x0330, bool) -- LED selector + door power
int32_t g_isAccOff   = -1;       // ApasswordLock_C::isAcc      (0x037C, bool) -- crosshair-over-key_acc HOVER
int32_t g_isDenyOff  = -1;       // ApasswordLock_C::isDeny     (0x037D, bool) -- crosshair-over-key_deny HOVER
void*   g_inputNumFn = nullptr;  // ApasswordLock_C::inputNumber(int32 Num)
void*   g_updFn      = nullptr;  // ApasswordLock_C::upd()  (best-effort refresh; may be null)
void*   g_openFn     = nullptr;  // ApasswordLock_C::Open(bool Active) -- the native submit chain

// Documented Alpha 0.9.0-n fallbacks (CXXHeaderDump/passwordLock.hpp + triggerBase.hpp).
constexpr int32_t kKeyOffFallback     = 0x0260;
constexpr int32_t kInPwOffFallback    = 0x0380;
constexpr int32_t kDoorOffFallback    = 0x0338;
constexpr int32_t kIsResetOffFallback = 0x0360;
constexpr int32_t kActiveOffFallback  = 0x0330;
constexpr int32_t kIsAccOffFallback   = 0x037C;
constexpr int32_t kIsDenyOffFallback  = 0x037D;

int32_t ResolveOff(void* cls, const wchar_t* name, int32_t fallback) {
    int32_t off = R::FindPropertyOffset(cls, name);
    if (off < 0) {
        UE_LOGW("passwordlock: reflected %ls offset not found -- using fallback 0x%04X", name, fallback);
        off = fallback;
    }
    return off;
}

std::wstring ReadFString(const void* obj, int32_t off) {
    if (!obj || off < 0) return std::wstring();
    const R::FString& s = *reinterpret_cast<const R::FString*>(
        reinterpret_cast<const char*>(obj) + off);
    if (!s.Data || s.Num <= 1 || s.Num > 4096) return std::wstring();
    return std::wstring(s.Data, s.Data + (s.Num - 1));  // Num counts the null terminator
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* lockCls = R::FindClass(L"passwordLock_C");
    if (!lockCls) return false;  // BP class not loaded yet -- caller retries

    // Key is declared on AtriggerBase_C; FindPropertyOffset does NOT climb to super,
    // so query the declaring class. The rest are declared on passwordLock_C.
    int32_t keyOff = -1;
    if (void* trigCls = R::FindClass(L"triggerBase_C")) keyOff = R::FindPropertyOffset(trigCls, L"Key");
    if (keyOff < 0) {
        UE_LOGW("passwordlock: reflected Key offset not found -- using fallback 0x%04X", kKeyOffFallback);
        keyOff = kKeyOffFallback;
    }
    const int32_t inPwOff    = ResolveOff(lockCls, L"inPassword", kInPwOffFallback);
    const int32_t doorOff    = ResolveOff(lockCls, L"door",       kDoorOffFallback);
    const int32_t isResetOff = ResolveOff(lockCls, L"isReset",    kIsResetOffFallback);
    const int32_t activeOff  = ResolveOff(lockCls, L"active",     kActiveOffFallback);
    const int32_t isAccOff   = ResolveOff(lockCls, L"isAcc",      kIsAccOffFallback);
    const int32_t isDenyOff  = ResolveOff(lockCls, L"isDeny",     kIsDenyOffFallback);

    void* inputNumFn = R::FindFunction(lockCls, L"inputNumber");
    if (!inputNumFn) {
        UE_LOGW("passwordlock: inputNumber UFunction not found -- not ready");
        return false;  // the receiver cannot mirror typing without it -- retry
    }
    void* updFn   = R::FindFunction(lockCls, L"upd");    // best-effort; tolerated null
    // The native submit chain (the BP calls it `open`; the cooked UFunction FName renders
    // `Open` in the CXX dump -- FName-compare both spellings). Tolerated null: without it
    // the short-code submit mirror degrades to the plain state mirror (logged at call).
    void* openFn = R::FindFunction(lockCls, L"open");
    if (!openFn) openFn = R::FindFunction(lockCls, L"Open");

    g_lockCls    = lockCls;
    g_keyOff     = keyOff;
    g_inPwOff    = inPwOff;
    g_doorOff    = doorOff;
    g_isResetOff = isResetOff;
    g_activeOff  = activeOff;
    g_isAccOff   = isAccOff;
    g_isDenyOff  = isDenyOff;
    g_inputNumFn = inputNumFn;
    g_updFn      = updFn;
    g_openFn     = openFn;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("passwordlock: resolved passwordLock_C=%p Key@0x%04X inPassword@0x%04X "
            "door@0x%04X isReset@0x%04X active@0x%04X isAcc@0x%04X isDeny@0x%04X "
            "inputNumber=%p upd=%p open=%p",
            lockCls, keyOff, inPwOff, doorOff, isResetOff, activeOff, isAccOff, isDenyOff,
            inputNumFn, updFn, openFn);
    return true;
}

bool IsPasswordLock(void* obj) {
    if (!obj || !g_lockCls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_lockCls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

std::wstring GetKeyString(void* lock) {
    if (!lock || g_keyOff < 0) return std::wstring();
    const R::FName& key = *reinterpret_cast<const R::FName*>(
        reinterpret_cast<const char*>(lock) + g_keyOff);
    return R::ToString(key);
}

bool ReadState(void* lock, State& out) {
    if (!lock || !g_resolved.load(std::memory_order_acquire)) return false;
    out.buffer = ReadFString(lock, g_inPwOff);
    out.active = (g_activeOff >= 0) &&
                 *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(lock) + g_activeOff);
    return true;
}

void* GatedDoor(void* lock) {
    if (!lock || !g_resolved.load(std::memory_order_acquire) || g_doorOff < 0) return nullptr;
    void* door = *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(lock) + g_doorOff);
    return (door && R::IsLive(door)) ? door : nullptr;
}

bool IsResetMode(void* lock) {
    if (!lock || !g_resolved.load(std::memory_order_acquire) || g_isResetOff < 0) return false;
    return *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(lock) + g_isResetOff);
}

bool IsPressHover(void* lock) {
    if (!lock || !g_resolved.load(std::memory_order_acquire)) return false;
    const char* base = reinterpret_cast<const char*>(lock);
    const bool acc  = g_isAccOff  >= 0 && *reinterpret_cast<const bool*>(base + g_isAccOff);
    const bool deny = g_isDenyOff >= 0 && *reinterpret_cast<const bool*>(base + g_isDenyOff);
    return acc || deny;
}

bool CallInputNumber(void* lock, int32_t digit) {
    if (!lock || !g_inputNumFn || digit < 0 || digit > 9) return false;
    ParamFrame f(g_inputNumFn);
    if (!f.valid()) return false;
    // Param name "Num" is the live FProperty name (passwordLock.hpp:98 inputNumber(int32 Num)).
    f.Set<int32_t>(L"Num", digit);
    return Call(lock, f);
}

bool ClearBuffer(void* lock) {
    if (!lock || !g_resolved.load(std::memory_order_acquire) || g_inPwOff < 0) return false;
    R::FString& s = *reinterpret_cast<R::FString*>(reinterpret_cast<char*>(lock) + g_inPwOff);
    // Empty the FString the leak-free way: Num=0 is the canonical empty TArray<TCHAR>
    // (FString::operator* returns TEXT("") when Num==0); Data/Max are retained as slack
    // and freed by the engine on the next append/reassign (exactly FString::Reset()).
    // NO verb -> no setActive/powerChanged/isReset side effects; the LED is mirrored
    // separately by WriteActive, and the caller repaints via CallUpd.
    if (s.Num > 0) s.Num = 0;
    return true;
}

void CallUpd(void* lock) {
    if (!lock || !g_updFn) return;
    ParamFrame f(g_updFn);
    if (f.valid()) Call(lock, f);
}

bool CallOpen(void* lock, bool accept) {
    if (!lock) return false;
    if (!g_openFn) {
        UE_LOGW("passwordlock: Open UFunction unresolved -- submit mirror degraded to state mirror");
        return false;
    }
    ParamFrame f(g_openFn);
    if (!f.valid()) return false;
    // Param name "Active" per the cooked signature (CXXHeaderDump passwordLock.hpp:
    // `void Open(bool Active)`). The body may defer sub-chains through latent Delays --
    // callers must not expect synchronous state.
    f.Set<bool>(L"Active", accept);
    return Call(lock, f);
}

bool WriteActive(void* lock, bool active) {
    if (!lock || !g_resolved.load(std::memory_order_acquire) || g_activeOff < 0) return false;
    *reinterpret_cast<bool*>(reinterpret_cast<char*>(lock) + g_activeOff) = active;
    return true;
}

}  // namespace ue_wrap::passwordlock
