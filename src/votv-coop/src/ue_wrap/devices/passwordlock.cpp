// ue_wrap/devices/passwordlock.cpp -- see ue_wrap/devices/passwordlock.h. Engine access for the
// password keypads (ApasswordLock_C).

#include "ue_wrap/devices/passwordlock.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::passwordlock {
namespace {

namespace R = reflection;

// Resolved once at EnsureResolved, then read-only; published by the g_resolved release-store.
std::atomic<bool> g_resolved{false};
bool g_unusable = false;  // the class loaded and a name did not resolve: left out, said once

void*   g_lockCls    = nullptr;
int32_t g_keyOff     = -1;  // triggerBase_C::Key
int32_t g_inPwOff    = -1;  // inPassword (FString), the typed buffer
int32_t g_pwOff      = -1;  // password (FString)
int32_t g_doorOff    = -1;  // door (door_C*), the gated door
int32_t g_isResetOff = -1;  // isReset, set-new-code mode
int32_t g_activeOff  = -1;  // active, the verdict and the power it hands on
int32_t g_isAccOff   = -1;  // isAcc, the look-at on the accept key
int32_t g_isDenyOff  = -1;  // isDeny, the look-at on the cancel key
int32_t g_enteringOff = -1; // entering, set by open until its tail has run
int32_t g_pairOff    = -1;  // pair (passwordLock_C*), the keypad it hands its state to
int32_t g_protectedOff = -1; // protected, the mode in which reset() changes nothing
void*   g_inputNumFn = nullptr;
void*   g_openFn     = nullptr;
void*   g_open2Fn    = nullptr;
void*   g_resetFn    = nullptr;
void*   g_falseFn    = nullptr;
void*   g_setActiveFn = nullptr;
void*   g_anyKeyFn   = nullptr;  // playerAnykey, the drill's keyboard; resolved on first use

bool ReadBool(const void* obj, int32_t off) {
    return off >= 0 && *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(obj) + off);
}

void WriteBool(void* obj, int32_t off, bool v) {
    *reinterpret_cast<bool*>(reinterpret_cast<char*>(obj) + off) = v;
}

std::wstring ReadFString(const void* obj, int32_t off) {
    if (!obj || off < 0) return std::wstring();
    const R::FString& s = *reinterpret_cast<const R::FString*>(reinterpret_cast<const char*>(obj) + off);
    if (!s.Data || s.Num <= 1 || s.Num > 4096) return std::wstring();
    return std::wstring(s.Data, s.Data + (s.Num - 1));  // Num counts the terminator
}

// Two FStrings equal by content; the length first, so an empty buffer is one compare.
bool FStringEquals(const void* obj, int32_t offA, int32_t offB) {
    const auto& a = *reinterpret_cast<const R::FString*>(reinterpret_cast<const char*>(obj) + offA);
    const auto& b = *reinterpret_cast<const R::FString*>(reinterpret_cast<const char*>(obj) + offB);
    const int32_t na = (a.Data && a.Num > 1) ? a.Num - 1 : 0;
    const int32_t nb = (b.Data && b.Num > 1) ? b.Num - 1 : 0;
    if (na != nb) return false;
    for (int32_t i = 0; i < na; ++i)
        if (a.Data[i] != b.Data[i]) return false;
    return true;
}

bool CallNoParams(void* lock, void* fn) {
    if (!lock || !fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(lock, f);
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;
    if (g_unusable) return false;

    void* lockCls = R::FindClass(L"passwordLock_C");
    if (!lockCls) return false;  // not loaded yet: the caller retries

    // Key is declared on the trigger base and FindPropertyOffset does not climb, so it is asked
    // of the declaring class; the rest are the keypad's own.
    void* trigCls = R::FindClass(L"triggerBase_C");
    const int32_t keyOff     = trigCls ? R::FindPropertyOffset(trigCls, L"Key") : -1;
    const int32_t inPwOff    = R::FindPropertyOffset(lockCls, L"inPassword");
    const int32_t pwOff      = R::FindPropertyOffset(lockCls, L"password");
    const int32_t doorOff    = R::FindPropertyOffset(lockCls, L"door");
    const int32_t isResetOff = R::FindPropertyOffset(lockCls, L"isReset");
    const int32_t activeOff  = R::FindPropertyOffset(lockCls, L"active");
    const int32_t isAccOff   = R::FindPropertyOffset(lockCls, L"isAcc");
    const int32_t isDenyOff  = R::FindPropertyOffset(lockCls, L"isDeny");
    const int32_t enteringOff = R::FindPropertyOffset(lockCls, L"entering");
    const int32_t pairOff    = R::FindPropertyOffset(lockCls, L"pair");
    const int32_t protectedOff = R::FindPropertyOffset(lockCls, L"protected");
    void* inputNumFn  = R::FindFunction(lockCls, L"inputNumber");
    void* openFn      = R::FindFunction(lockCls, L"open");
    void* open2Fn     = R::FindFunction(lockCls, L"open2");
    void* resetFn     = R::FindFunction(lockCls, L"reset");
    void* falseFn     = R::FindFunction(lockCls, L"falseEnterEvent");
    void* setActiveFn = R::FindFunction(lockCls, L"setActive");
    if (keyOff < 0 || inPwOff < 0 || pwOff < 0 || doorOff < 0 || isResetOff < 0 || activeOff < 0 ||
        isAccOff < 0 || isDenyOff < 0 || enteringOff < 0 || pairOff < 0 || protectedOff < 0 || !inputNumFn ||
        !openFn || !open2Fn || !resetFn || !falseFn || !setActiveFn) {
        g_unusable = true;
        UE_LOGE("passwordlock: passwordLock_C is loaded but not every name resolved (Key@%d inPassword@%d "
                "password@%d door@%d isReset@%d active@%d isAcc@%d isDeny@%d entering@%d pair@%d protected@%d "
                "inputNumber=%p open=%p open2=%p reset=%p falseEnterEvent=%p setActive=%p) -- the keypads stay "
                "unsynced", keyOff, inPwOff, pwOff, doorOff, isResetOff, activeOff, isAccOff, isDenyOff,
                enteringOff, pairOff, protectedOff, inputNumFn, openFn, open2Fn, resetFn, falseFn, setActiveFn);
        return false;
    }

    g_lockCls     = lockCls;
    g_keyOff      = keyOff;
    g_inPwOff     = inPwOff;
    g_pwOff       = pwOff;
    g_doorOff     = doorOff;
    g_isResetOff  = isResetOff;
    g_activeOff   = activeOff;
    g_isAccOff    = isAccOff;
    g_isDenyOff   = isDenyOff;
    g_enteringOff = enteringOff;
    g_pairOff     = pairOff;
    g_protectedOff = protectedOff;
    g_inputNumFn  = inputNumFn;
    g_openFn      = openFn;
    g_open2Fn     = open2Fn;
    g_resetFn     = resetFn;
    g_falseFn     = falseFn;
    g_setActiveFn = setActiveFn;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("passwordlock: resolved passwordLock_C=%p Key@0x%04X inPassword@0x%04X active@0x%04X "
            "isReset@0x%04X, and its six verbs", lockCls, keyOff, inPwOff, activeOff, isResetOff);
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
    const R::FName& key = *reinterpret_cast<const R::FName*>(reinterpret_cast<const char*>(lock) + g_keyOff);
    return R::ToString(key);
}

bool ReadState(void* lock, State& out) {
    if (!lock || !g_resolved.load(std::memory_order_acquire)) return false;
    out.buffer  = ReadFString(lock, g_inPwOff);
    out.active  = ReadBool(lock, g_activeOff);
    out.isReset = ReadBool(lock, g_isResetOff);
    out.password = ReadFString(lock, g_pwOff);
    return true;
}

bool ReadHover(void* lock, bool& onAccept, bool& onCancel) {
    if (!lock || !g_resolved.load(std::memory_order_acquire)) return false;
    onAccept = ReadBool(lock, g_isAccOff);
    onCancel = ReadBool(lock, g_isDenyOff);
    return true;
}

bool BufferMatchesPassword(void* lock) {
    if (!lock || !g_resolved.load(std::memory_order_acquire)) return false;
    return FStringEquals(lock, g_inPwOff, g_pwOff);
}

bool IsEntering(void* lock) {
    return lock && g_resolved.load(std::memory_order_acquire) && ReadBool(lock, g_enteringOff);
}

void* PairOf(void* lock) {
    if (!lock || !g_resolved.load(std::memory_order_acquire)) return nullptr;
    void* pair = *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(lock) + g_pairOff);
    return (pair && pair != lock && R::IsLive(pair)) ? pair : nullptr;
}

bool IsProtected(void* lock) {
    if (!lock || !g_resolved.load(std::memory_order_acquire)) return false;
    if (ReadBool(lock, g_protectedOff)) return true;
    void* pair = PairOf(lock);
    return pair && ReadBool(pair, g_protectedOff);
}

bool CallInputNumber(void* lock, int32_t digit) {
    if (!lock || !g_inputNumFn || digit < 0 || digit > 9) return false;
    ParamFrame f(g_inputNumFn);
    if (!f.valid() || !f.Set<int32_t>(L"num", digit)) return false;
    return Call(lock, f);
}

bool CallOpen(void* lock, bool accept) {
    if (!lock || !g_openFn) return false;
    ParamFrame f(g_openFn);
    if (!f.valid() || !f.Set<bool>(L"active", accept)) return false;
    return Call(lock, f);
}

bool CallOpen2(void* lock) { return CallNoParams(lock, g_open2Fn); }
bool CallReset(void* lock) { return CallNoParams(lock, g_resetFn); }
bool CallFalseEnter(void* lock) { return CallNoParams(lock, g_falseFn); }

bool CallSetActive(void* lock, bool isPairCall) {
    if (!lock || !g_setActiveFn) return false;
    ParamFrame f(g_setActiveFn);
    if (!f.valid() || !f.Set<bool>(L"isPairCall", isPairCall)) return false;
    return Call(lock, f);
}

bool WriteActive(void* lock, bool active) {
    if (!lock || !g_resolved.load(std::memory_order_acquire)) return false;
    WriteBool(lock, g_activeOff, active);
    return true;
}

bool WriteResetMode(void* lock, bool on) {
    if (!lock || !g_resolved.load(std::memory_order_acquire)) return false;
    WriteBool(lock, g_isResetOff, on);
    return true;
}

bool WriteBuffer(void* lock, const std::wstring& digits) {
    if (!lock || !g_resolved.load(std::memory_order_acquire)) return false;
    if (digits.empty()) {
        // An empty FString is a zero count; the storage stays as slack, as FString::Reset leaves it,
        // and the engine frees or reuses it on the next assignment.
        R::FString& s = *reinterpret_cast<R::FString*>(reinterpret_cast<char*>(lock) + g_inPwOff);
        if (s.Num > 0) s.Num = 0;
        return true;
    }
    return field_io::WriteFStringField(lock, g_inPwOff, digits);
}

bool WritePassword(void* lock, const std::wstring& password) {
    if (!lock || !g_resolved.load(std::memory_order_acquire)) return false;
    return field_io::WriteFStringField(lock, g_pwOff, password);
}

void* GatedDoor(void* lock) {
    if (!lock || !g_resolved.load(std::memory_order_acquire)) return nullptr;
    void* door = *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(lock) + g_doorOff);
    return (door && R::IsLive(door)) ? door : nullptr;
}

bool CallPressOffDigits(void* lock) {
    if (!lock || !g_inputNumFn) return false;
    ParamFrame f(g_inputNumFn);
    if (!f.valid() || !f.Set<int32_t>(L"num", -1)) return false;
    return Call(lock, f);
}

bool WriteHover(void* lock, bool onAccept, bool onCancel) {
    if (!lock || !g_resolved.load(std::memory_order_acquire)) return false;
    WriteBool(lock, g_isAccOff, onAccept);
    WriteBool(lock, g_isDenyOff, onCancel);
    return true;
}

bool CallPlayerAnykey(void* lock, const wchar_t* keyName, bool pressed) {
    if (!lock || !keyName || !g_resolved.load(std::memory_order_acquire)) return false;
    if (!g_anyKeyFn) g_anyKeyFn = R::FindFunction(g_lockCls, L"playerAnykey");
    if (!g_anyKeyFn) return false;
    ParamFrame f(g_anyKeyFn);
    // An FKey is its name first; the details the display name needs are looked up from the name.
    const R::FName name = fname_utils::StringToFName(keyName);
    if (!f.valid() || !f.SetRaw(L"key", &name, static_cast<int32_t>(sizeof(name))) || !f.Set<bool>(L"pressed", pressed))
        return false;
    return Call(lock, f);
}

}  // namespace ue_wrap::passwordlock
